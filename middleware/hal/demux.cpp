#include "demux.hpp"

#include <string>

namespace hal {

namespace {

constexpr const char* kModule = "HAL-DMX";
constexpr std::size_t kTsPacketSize = 188;
constexpr std::uint16_t kNullPid = 0x1FFF;

// The demux whose filter callbacks run on this thread right now (its mutex is held).
thread_local const Demux* tls_delivering = nullptr;

struct DeliveryMark {  // also cleared if a callback throws
    explicit DeliveryMark(const Demux* demux) { tls_delivering = demux; }
    ~DeliveryMark() { tls_delivering = nullptr; }
};

}  // namespace

Demux::Demux(LogFn log) : log_(std::move(log)) { last_cc_.fill(-1); }

bool Demux::refuse_in_callback(const char* method) const {
    if (tls_delivering != this) return false;
    if (log_) log_(LogLevel::Error, kModule, std::string(method) + " called from a filter callback: refused");
    return true;
}

Demux::FilterId Demux::add_filter(FilterType type, std::uint16_t pid, int table_id, DataCallback on_data) {
    if (pid > kNullPid || table_id < -1 || table_id > 255 || !on_data) return 0;
    if (refuse_in_callback("add_filter")) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    auto f = std::make_unique<Filter>();
    f->id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;  // 0 stays invalid after a wrap
    f->type = type;
    f->pid = pid;
    f->table_id = table_id;
    f->on_data = std::move(on_data);
    if (type == FilterType::Section) f->section.reserve(kSectionBufferSize + kTsPacketSize);  // B3
    const FilterId id = f->id;
    by_pid_[pid].push_back(f.get());
    filters_[id] = std::move(f);
    return id;
}

bool Demux::start_filter(FilterId id) {
    if (refuse_in_callback("start_filter")) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = filters_.find(id);
    if (it == filters_.end()) return false;
    it->second->started = true;
    return true;
}

void Demux::stop_filter(FilterId id) {
    if (refuse_in_callback("stop_filter")) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = filters_.find(id);
    if (it == filters_.end()) return;
    it->second->started = false;
    it->second->assembling = false;
    it->second->section.clear();
}

void Demux::remove_filter(FilterId id) {
    if (refuse_in_callback("remove_filter")) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = filters_.find(id);
    if (it == filters_.end()) return;
    auto& list = by_pid_[it->second->pid];
    for (auto p = list.begin(); p != list.end(); ++p) {
        if (*p == it->second.get()) {
            list.erase(p);
            break;
        }
    }
    filters_.erase(it);
}

Demux::Stats Demux::stats() const {
    if (tls_delivering == this) return stats_;  // the mutex is already held by this thread
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void Demux::reset_stats() {
    if (refuse_in_callback("reset_stats")) return;
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats{};
    last_cc_.fill(-1);
}

void Demux::feed(std::uint8_t* packets, std::size_t count) {
    if (refuse_in_callback("feed")) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const DeliveryMark mark(this);
    for (std::size_t i = 0; i < count; ++i) feed_packet(packets + i * kTsPacketSize);
}

void Demux::feed_packet(std::uint8_t* p) {
    ++stats_.ts_packets;
    if (p[0] != 0x47) {  // REQ-15: dropped, counted
        ++stats_.sync_errors;
        return;
    }
    const std::uint16_t pid = static_cast<std::uint16_t>(((p[1] & 0x1F) << 8) | p[2]);
    const bool unit_start = (p[1] & 0x40) != 0;
    const int scrambling = (p[3] >> 6) & 0x3;
    const int afc = (p[3] >> 4) & 0x3;
    const int cc = p[3] & 0x0F;
    const bool has_adaptation = (afc & 0x2) != 0;
    const bool has_payload = (afc & 0x1) != 0;
    const bool discontinuity = has_adaptation && p[4] > 0 && (p[5] & 0x80) != 0;

    // REQ-15: continuity counter per PID. It increments with every packet that has a payload; a
    // repeated packet keeps it once; the discontinuity_indicator announces a jump. Null packets are
    // not checked.
    bool continuous = true;
    if (pid != kNullPid) {
        const int last = last_cc_[pid];
        if (last >= 0 && !discontinuity) {
            const bool ok = has_payload ? (cc == ((last + 1) & 0x0F) || cc == last) : (cc == last);
            if (!ok) {
                ++stats_.cc_errors;
                continuous = false;
            }
        }
        last_cc_[pid] = static_cast<std::int8_t>(cc);
    }

    const auto& list = by_pid_[pid];
    if (list.empty()) return;
    int payload_start = 4;
    if (has_adaptation) payload_start = 5 + p[4];
    const bool payload_ok = has_payload && payload_start < static_cast<int>(kTsPacketSize);
    for (Filter* f : list) {
        if (!f->started) continue;
        if (f->type == FilterType::Ts) {
            f->on_data(p, kTsPacketSize, scrambling != 0);
        } else if (payload_ok && scrambling == 0) {  // PSI is never scrambled
            assemble(*f, p + payload_start, kTsPacketSize - static_cast<std::size_t>(payload_start), unit_start,
                     continuous);
        }
    }
}

void Demux::assemble(Filter& f, const std::uint8_t* payload, std::size_t len, bool unit_start, bool continuous) {
    if (!continuous && f.assembling) discard_section(f, "continuity counter discontinuity");
    if (!unit_start) {
        if (!f.assembling) return;  // wait for the next payload_unit_start_indicator = 1
        f.section.insert(f.section.end(), payload, payload + len);
        process_sections(f);
        return;
    }
    const std::size_t pointer = payload[0];
    if (1 + pointer > len) {  // invalid pointer_field
        if (f.assembling) discard_section(f, "invalid pointer_field");
        return;
    }
    if (f.assembling) {  // the bytes before the pointer end the previous section
        f.section.insert(f.section.end(), payload + 1, payload + 1 + pointer);
        process_sections(f);
        if (f.assembling && !f.section.empty()) discard_section(f, "section not complete at the next unit start");
    }
    f.section.clear();
    f.assembling = true;
    f.section.insert(f.section.end(), payload + 1 + pointer, payload + len);
    process_sections(f);
}

void Demux::process_sections(Filter& f) {
    while (f.assembling && !f.section.empty()) {
        if (f.section[0] == 0xFF) {  // stuffing: no further section in this packet
            f.assembling = false;
            f.section.clear();
            return;
        }
        if (f.section.size() < 3) return;  // section header continues in the next packet
        const std::size_t total = 3 + (((f.section[1] & 0x0F) << 8) | f.section[2]);
        if (total > kSectionBufferSize) {  // B3: discarded, counted; restart at the next unit start
            discard_section(f, "section longer than 4096 bytes");
            return;
        }
        if (f.section.size() < total) return;  // continues in the next packet
        if (f.table_id < 0 || f.section[0] == f.table_id) f.on_data(f.section.data(), total, false);
        f.section.erase(f.section.begin(), f.section.begin() + static_cast<std::ptrdiff_t>(total));
    }
}

void Demux::discard_section(Filter& f, const char* reason) {
    f.assembling = false;
    f.section.clear();
    ++stats_.sections_discarded;
    // ER-24: logged, at most once per second (the count of the others is added to the next line).
    const auto now = std::chrono::steady_clock::now();
    if (now - last_discard_log_ >= std::chrono::seconds(1)) {
        if (log_) {
            log_(LogLevel::Warn, kModule,
                 "section discarded on PID " + std::to_string(f.pid) + ": " + reason +
                     (discards_not_logged_ ? " (+" + std::to_string(discards_not_logged_) + " not logged)" : ""));
        }
        last_discard_log_ = now;
        discards_not_logged_ = 0;
    } else {
        ++discards_not_logged_;
    }
}

}  // namespace hal
