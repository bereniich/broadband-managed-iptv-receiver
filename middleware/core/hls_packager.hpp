// AL-OUT: the HLS packager of the DTV middleware (Sections 2.2.4, 3.7, 4.2.3 and 4.2.4; REQ-22 – REQ-24,
// REQ-52; buffers B5 – B7 of Section 4.5.4). Builds the output stream from the TS filter packets in
// the received order, cuts it into segments that start with PAT, PMT and a key frame, keeps the live
// playlist of the last 6 segments and at most 8 segment files, marks loop boundaries with
// #EXT-X-DISCONTINUITY and holds complete segments while writing is disabled (key validation).
// Runs on the AL worker; AL-SES passes the packets (S11) and reacts to the events.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "hls_playlist.hpp"
#include "pal/platform.hpp"
#include "psi_parser.hpp"
#include "ts_packet.hpp"

namespace al {

class HlsPackager {
public:
    static constexpr std::uint64_t kMinSegmentTicks = 1 * kPcrTicksPerSecond;   // REQ-23
    static constexpr std::uint64_t kMaxSegmentTicks = 2 * kPcrTicksPerSecond;   // IF-INT-02, B5
    static constexpr std::uint64_t kMaxPcrStepTicks = 1 * kPcrTicksPerSecond;   // REQ-24
    static constexpr std::size_t kListedSegments = 6;                          // REQ-23
    static constexpr std::size_t kKeptUnlisted = 2;                            // REQ-23, B7
    static constexpr std::size_t kMaxHeldSegments = 4;                         // B6
    static constexpr std::size_t kSegmentReserve = 1024 * 1024;                // B5

    struct Events {
        std::function<void()> on_first_segment;   // EV-SEG1: the first segment is listed
        std::function<void()> on_error;           // EV-OUT-ERR: write_file returned false (once)
    };

    HlsPackager(pal::IHlsStore& store, pal::ILogger& logger);
    ~HlsPackager();

    HlsPackager(const HlsPackager&) = delete;
    HlsPackager& operator=(const HlsPackager&) = delete;

    // S3: IHlsStore::open_session; writing disabled. A previous session is closed first. Returns false
    // when open_session fails (AL-SES turns it into EV-OUT-ERR).
    bool open(std::uint64_t session, std::uint64_t generation, Events events);
    // T-12: the PIDs of the output stream.
    void set_stream(const StreamDescription& stream);
    // S11: one TS packet of a TS filter (PID 0, the PMT PID, the video or the audio PID).
    void feed(const std::uint8_t* packet);
    // T-16, T-19: on -> the held segments are written in order (REQ-52).
    void set_write_enabled(bool on);
    // RELEASE: held data discarded, IHlsStore::close_session.
    void close();

    bool write_enabled() const { return write_enabled_; }
    std::size_t held_segments() const { return held_.size(); }

private:
    using Packet = std::array<std::uint8_t, kTsPacketSize>;

    struct Segment {
        std::vector<std::uint8_t> bytes;
        std::uint64_t duration_ticks = 0;
        bool discontinuity = false;
    };

    void start_segment(const std::uint8_t* key_frame_packet, std::optional<std::uint64_t> pcr);
    void append(const std::uint8_t* packet);
    void finish_segment(std::uint64_t duration_ticks);
    void discard_segment(const std::string& reason);
    void write_segment(Segment segment);
    void fail(const std::string& what);
    void log(pal::LogLevel level, const std::string& message);

    pal::IHlsStore& store_;
    pal::ILogger& logger_;

    bool opened_ = false;
    bool failed_ = false;
    std::uint64_t session_ = 0;
    std::uint64_t generation_ = 0;
    Events events_;
    std::optional<StreamDescription> stream_;
    std::optional<Packet> last_pat_;
    std::optional<Packet> last_pmt_;
    std::optional<std::uint64_t> last_pcr_;

    // Segment being built (B5).
    bool building_ = false;
    std::vector<std::uint8_t> current_;
    std::optional<std::uint64_t> start_pcr_;
    bool current_discontinuity_ = false;
    bool next_discontinuity_ = false;   // the next segment follows a loop boundary

    bool write_enabled_ = false;
    std::deque<Segment> held_;           // B6
    std::deque<PlaylistEntry> listed_;   // the last 6 written segments
    std::uint64_t next_sequence_ = 0;
    std::uint64_t discontinuity_sequence_ = 0;
    bool first_listed_ = false;
};

}  // namespace al
