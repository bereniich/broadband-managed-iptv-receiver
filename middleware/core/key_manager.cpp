#include "key_manager.hpp"

#include <algorithm>
#include <utility>

namespace al {

namespace {

constexpr const char* kModule = "AL-DSC";

}  // namespace

const char* to_string(Protection protection) {
    switch (protection) {
        case Protection::Clear: return "clear";
        case Protection::ProtectedNoKey: return "protected-no-key";
        case Protection::ProtectedKey: return "protected-key";
    }
    return "unknown";
}

KeyManager::KeyManager(pal::IDemux& demux, pal::ILogger& logger, TaskQueue& queue)
    : demux_(demux), logger_(logger), queue_(queue) {}

KeyManager::~KeyManager() {
    close();
    keys_.clear_all();
}

bool KeyManager::store(const std::string& service_id, const std::string& key_hex) {
    std::optional<Key> key = parse_key_hex(key_hex);
    if (!key) return false;
    keys_.store(service_id, *key);
    logger_.log(pal::LogLevel::Info, kModule, "key stored service=" + service_id + " key=" + mask_key(*key));
    zero_key(*key);
    return true;
}

void KeyManager::discard(const std::string& service_id) {
    if (!keys_.contains(service_id)) return;
    keys_.discard(service_id);
    logger_.log(pal::LogLevel::Info, kModule, "key discarded service=" + service_id);
}

bool KeyManager::start(std::uint64_t session, std::uint64_t generation, const std::string& service_id,
                       const StreamDescription& stream, Events events) {
    close();
    active_ = true;
    session_ = session;
    generation_ = generation;
    service_id_ = service_id;
    video_pid_ = stream.video_pid;
    events_ = std::move(events);

    // REQ-49: the key is on the descrambler before the TS filters are started.
    if (std::optional<Key> key = keys_.lookup(service_id)) {
        std::vector<std::uint16_t> pids{stream.video_pid};
        if (stream.audio_pid) pids.push_back(*stream.audio_pid);
        demux_.set_key(pids.data(), pids.size(), key->data());
        key_set_ = true;
        key_mask_ = mask_key(*key);
        zero_key(*key);
    }

    // REQ-19: PID 0, the PMT PID, the video PID and the audio PID if present.
    std::vector<std::uint16_t> pids{0, stream.pmt_pid, stream.video_pid};
    if (stream.audio_pid) pids.push_back(*stream.audio_pid);
    for (std::uint16_t pid : pids) {
        const std::uint64_t g = generation_;
        TaskQueue& queue = queue_;
        // Runs on the tuner thread: copy the packet into a fixed array and post (rule T1, B4).
        auto on_data = [this, &queue, g](const pal::IDemux::FilterData& data) {
            if (data.len != kTsPacketSize) return;
            std::array<std::uint8_t, kTsPacketSize> packet;
            std::copy(data.data, data.data + kTsPacketSize, packet.begin());
            const bool scrambled = data.scrambled;
            queue.post(TaskKind::Data, [this, g, packet, scrambled] { on_packet(g, packet, scrambled); });
        };
        const pal::IDemux::FilterId id = demux_.open_filter({pal::FilterType::Ts, pid, -1}, on_data);
        if (id == 0 || !demux_.start_filter(id)) {
            if (id != 0) demux_.close_filter(id);
            log(pal::LogLevel::Error, "cannot open the TS filter on PID " + std::to_string(pid));
            close();
            return false;
        }
        filters_.push_back(id);
    }
    return true;
}

void KeyManager::close() {
    if (!active_) return;
    for (pal::IDemux::FilterId id : filters_) demux_.close_filter(id);
    filters_.clear();
    demux_.clear_keys();
    active_ = false;   // queued packets of the attempt are discarded in on_packet()
    events_ = Events{};
    service_id_.clear();
    key_set_ = false;
    key_mask_.clear();
    protection_.reset();
    result_ = KeyResult::Pending;
    checked_ = 0;
}

void KeyManager::on_packet(std::uint64_t generation, const std::array<std::uint8_t, kTsPacketSize>& packet,
                           bool scrambled) {
    if (!current(generation)) return;   // released or older attempt
    if (ts_pid(packet.data()) == video_pid_) {
        handle_video(generation, packet.data(), scrambled);
        if (!current(generation)) return;   // released by a handler (T-14, T-17)
    }
    const auto on_packet = events_.on_packet;   // a copy: the handler may call close()
    if (on_packet) on_packet(packet.data(), scrambled);
}

void KeyManager::handle_video(std::uint64_t generation, const std::uint8_t* packet, bool scrambled) {
    if (!protection_) {
        // REQ-44: only a packet with at least 16 payload bytes decides.
        if (ts_payload_size(packet) < kMinEncryptedPayload) return;
        if (!scrambled) {
            protection_ = Protection::Clear;
            log(pal::LogLevel::Info, "protection clear");
        } else {
            protection_ = key_set_ ? Protection::ProtectedKey : Protection::ProtectedNoKey;
            log(pal::LogLevel::Info, std::string("protection protected key_set=") + (key_set_ ? "true" : "false"));
        }
        const auto on_video1 = events_.on_video1;
        if (on_video1) on_video1(*protection_);
        if (!current(generation)) return;
    }

    // REQ-51: the first three PES starts received with scrambled = true; the packet that decided
    // the protection is the first of them if it is one. Packets whose payload was too short to be
    // encrypted arrive with scrambled = false and are skipped.
    if (protection_ != Protection::ProtectedKey || result_ != KeyResult::Pending) return;
    if (!ts_pusi(packet) || !scrambled) return;
    if (pes_start_ok(packet)) {
        ++checked_;
        if (checked_ < kPesStartsToCheck) return;
        result_ = KeyResult::Accepted;
        log(pal::LogLevel::Info, "key accepted service=" + service_id_ + " key=" + key_mask_);
        const auto on_key_ok = events_.on_key_ok;
        if (on_key_ok) on_key_ok();
    } else {
        ++checked_;
        result_ = KeyResult::Rejected;
        log(pal::LogLevel::Info, "key rejected service=" + service_id_ + " key=" + key_mask_);
        const auto on_key_bad = events_.on_key_bad;
        if (on_key_bad) on_key_bad();
    }
}

void KeyManager::log(pal::LogLevel level, const std::string& message) {
    logger_.log(level, kModule,
                message + " session=" + std::to_string(session_) + " generation=" + std::to_string(generation_));
}

}  // namespace al
