#include "hls_packager.hpp"

#include <algorithm>
#include <utility>

namespace al {

namespace {

constexpr const char* kModule = "AL-OUT";
constexpr const char* kPlaylistName = "index.m3u8";

}  // namespace

HlsPackager::HlsPackager(pal::IHlsStore& store, pal::ILogger& logger) : store_(store), logger_(logger) {}

HlsPackager::~HlsPackager() { close(); }

bool HlsPackager::open(std::uint64_t session, std::uint64_t generation, Events events) {
    close();
    session_ = session;
    generation_ = generation;
    if (!store_.open_session(session)) {
        log(pal::LogLevel::Error, "cannot create the HLS directory of the session");
        return false;
    }
    opened_ = true;
    events_ = std::move(events);
    current_.reserve(kSegmentReserve);   // B5: reserved once per session
    return true;
}

void HlsPackager::set_stream(const StreamDescription& stream) { stream_ = stream; }

void HlsPackager::close() {
    if (opened_) store_.close_session(session_);
    opened_ = false;
    failed_ = false;
    events_ = Events{};
    stream_.reset();
    last_pat_.reset();
    last_pmt_.reset();
    last_pcr_.reset();
    building_ = false;
    current_.clear();
    current_.shrink_to_fit();
    start_pcr_.reset();
    current_discontinuity_ = false;
    next_discontinuity_ = false;
    write_enabled_ = false;
    held_.clear();
    listed_.clear();
    next_sequence_ = 0;
    discontinuity_sequence_ = 0;
    first_listed_ = false;
}

void HlsPackager::feed(const std::uint8_t* packet) {
    if (!opened_ || failed_ || !stream_) return;
    const std::uint16_t pid = ts_pid(packet);
    const bool is_video = pid == stream_->video_pid;
    const bool is_audio = stream_->audio_pid && pid == *stream_->audio_pid;
    const bool is_pat = pid == 0;
    const bool is_pmt = pid == stream_->pmt_pid;
    if (!is_video && !is_audio && !is_pat && !is_pmt) return;   // REQ-22: only the stream PIDs
    if (((packet[3] >> 6) & 0x3) != 0) return;                   // never output a scrambled packet

    // The last PAT and PMT packets start every segment (REQ-23).
    if (is_pat && ts_pusi(packet)) std::copy(packet, packet + kTsPacketSize, last_pat_.emplace().begin());
    if (is_pmt && ts_pusi(packet)) std::copy(packet, packet + kTsPacketSize, last_pmt_.emplace().begin());

    // PCR of the PCR PID: loop boundary (REQ-24) before the packet is used.
    std::optional<std::uint64_t> pcr;
    if (pid == stream_->pcr_pid) pcr = ts_pcr(packet);
    if (pcr) {
        if (last_pcr_ && (*pcr < *last_pcr_ || *pcr - *last_pcr_ > kMaxPcrStepTicks)) {
            if (building_) {
                if (start_pcr_) {
                    finish_segment(*last_pcr_ - *start_pcr_);   // duration up to the last PCR before the jump
                } else {
                    discard_segment("loop boundary before the first PCR of the segment");
                }
                if (failed_ || !opened_) return;
            }
            next_discontinuity_ = true;
        }
        last_pcr_ = pcr;
    }

    const bool key_frame_start = is_video && ts_pusi(packet) && ts_random_access(packet);
    if (key_frame_start) {
        if (building_ && start_pcr_ && *last_pcr_ - *start_pcr_ >= kMinSegmentTicks) {
            finish_segment(*last_pcr_ - *start_pcr_);
            if (failed_ || !opened_) return;
        }
        if (!building_) {
            start_segment(packet, pcr);
            return;
        }
    }
    if (!building_) return;   // A/V packets before the first key-frame start are dropped
    append(packet);
    if (pcr && !start_pcr_) start_pcr_ = pcr;

    // B5: no key-frame start within 2 s of content.
    if (start_pcr_ && last_pcr_ && *last_pcr_ - *start_pcr_ >= kMaxSegmentTicks) {
        discard_segment("no key-frame start within 2 s of content");
    }
}

void HlsPackager::set_write_enabled(bool on) {
    write_enabled_ = on;
    if (!on) return;
    while (!held_.empty() && opened_ && !failed_) {
        Segment segment = std::move(held_.front());
        held_.pop_front();
        write_segment(std::move(segment));
    }
}

void HlsPackager::start_segment(const std::uint8_t* key_frame_packet, std::optional<std::uint64_t> pcr) {
    if (!last_pat_ || !last_pmt_) return;   // a segment can only start with PAT and PMT
    building_ = true;
    current_.clear();
    append(last_pat_->data());
    append(last_pmt_->data());
    append(key_frame_packet);
    start_pcr_ = pcr;
    current_discontinuity_ = next_discontinuity_;
    next_discontinuity_ = false;
}

void HlsPackager::append(const std::uint8_t* packet) {
    current_.insert(current_.end(), packet, packet + kTsPacketSize);
}

void HlsPackager::finish_segment(std::uint64_t duration_ticks) {
    Segment segment;
    segment.bytes = current_;
    segment.duration_ticks = duration_ticks;
    segment.discontinuity = current_discontinuity_;
    building_ = false;
    current_.clear();
    start_pcr_.reset();
    current_discontinuity_ = false;
    if (write_enabled_) {
        write_segment(std::move(segment));
        return;
    }
    held_.push_back(std::move(segment));   // REQ-52, B6
    if (held_.size() > kMaxHeldSegments) {
        if (held_.front().discontinuity && held_.size() > 1) held_[1].discontinuity = true;
        held_.pop_front();
        log(pal::LogLevel::Warn, "held segment dropped: more than 4 segments held");
    }
}

void HlsPackager::discard_segment(const std::string& reason) {
    if (current_discontinuity_) next_discontinuity_ = true;   // the mark moves to the next segment
    building_ = false;
    current_.clear();
    start_pcr_.reset();
    current_discontinuity_ = false;
    ++discarded_segments_;
    log(pal::LogLevel::Warn, "segment discarded: " + reason);
}

void HlsPackager::write_segment(Segment segment) {
    const std::uint64_t k = next_sequence_;
    const std::string name = segment_name(k);
    // REQ-23, B7: remove the segment that will be 3 positions older than the first listed one,
    // before the new one is written, so that at most 8 segment files exist at any time.
    constexpr std::uint64_t kRemoveDistance = kListedSegments + kKeptUnlisted;
    if (k >= kRemoveDistance) store_.remove_file(session_, segment_name(k - kRemoveDistance));
    if (!store_.write_file(session_, name, segment.bytes.data(), segment.bytes.size())) {
        fail("cannot write " + name);
        return;
    }
    ++next_sequence_;
    listed_.push_back(PlaylistEntry{k, segment.duration_ticks, segment.discontinuity});
    if (listed_.size() > kListedSegments) {
        if (listed_.front().discontinuity) ++discontinuity_sequence_;   // RFC 8216, 6.2.2
        listed_.pop_front();
    }
    const std::string playlist = playlist_text({listed_.begin(), listed_.end()}, discontinuity_sequence_);
    if (!store_.write_file(session_, kPlaylistName, reinterpret_cast<const std::uint8_t*>(playlist.data()),
                           playlist.size())) {
        fail("cannot write index.m3u8");
        return;
    }
    if (segment.discontinuity) log(pal::LogLevel::Info, "#EXT-X-DISCONTINUITY before " + name);
    log(pal::LogLevel::Info, "segment " + name + " duration=" + format_duration(segment.duration_ticks));
    if (!first_listed_) {
        first_listed_ = true;
        const auto on_first_segment = events_.on_first_segment;   // a copy: the handler may call close()
        if (on_first_segment) on_first_segment();
    }
}

void HlsPackager::fail(const std::string& what) {
    failed_ = true;
    log(pal::LogLevel::Error, what);
    const auto on_error = events_.on_error;   // a copy: the handler may call close()
    if (on_error) on_error();
}

void HlsPackager::log(pal::LogLevel level, const std::string& message) {
    logger_.log(level, kModule,
                message + " session=" + std::to_string(session_) + " generation=" + std::to_string(generation_));
}

}  // namespace al
