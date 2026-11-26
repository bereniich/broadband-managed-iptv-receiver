// SRV-ARC: archive timeline (Section 4.7.4, D-08).
//
// At head-end start the content file of a service is read once and a PCR index is built; the file
// itself is not kept in memory (B10). A start time T maps to the content position T mod D.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "services.hpp"

namespace srv {

constexpr int64_t kTsPacketSize = 188;
constexpr int64_t kTicksPerSecond = 90000;  // PCR_base and RTP timestamp clock (90 kHz)

using TsPacket = std::array<uint8_t, kTsPacketSize>;

// Missing or invalid content file (ER-38). The message is "<service_id>: <path>: <reason>".
class ArchiveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class WindowCheck { Ok, OutOfArchive, FutureTime };

// Archive window of Section 3.1: T is valid if now - window_s <= T < now.
WindowCheck check_archive_window(int64_t t, int64_t now, int64_t window_s);

class Archive {
public:
    // A packet of the PCR PID that carries a PCR: its byte offset and its content time
    // c = PCR_base - PCR_base of the first entry, in 90 kHz ticks.
    struct Entry {
        int64_t offset = 0;
        int64_t c = 0;
    };
    struct Position {
        int64_t offset = 0;
        int64_t c = 0;  // ticks
    };

    // Reads `path` once (Section 4.7.4, load): PCR index, first PAT and PMT packet, duration check
    // against the sum of the program durations (|measured - D| <= 0.1 s). Throws ArchiveError for a
    // missing file, a file that is not a transport stream of whole packets with sync bytes, a file
    // without PAT, PMT (each in one packet) or PCR, PCR values that do not increase, or a duration
    // mismatch. The file stays open read-only for the senders.
    static std::shared_ptr<const Archive> load(const Service& service, const std::string& path);

    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    // REQ-42: p = T mod D, the index entry with the largest c <= p.
    Position position(int64_t t) const;
    // The index entry with the largest c <= c_ticks (c_ticks in [0, D)).
    Position entry_at_or_before(int64_t c_ticks) const;
    // Content time of a byte offset (Section 4.7.5 step 3): linear interpolation between the index
    // entries around it, clamped to [0, D]. Before the first entry it is 0, after the last entry it
    // is extrapolated with the rate of the last interval.
    int64_t content_time(int64_t offset) const;

    const std::string& service_id() const { return service_id_; }
    const std::string& path() const { return path_; }
    int64_t duration_s() const { return duration_s_; }               // D from services.json
    int64_t duration_ticks() const { return duration_s_ * kTicksPerSecond; }
    double measured_duration_s() const { return measured_duration_s_; }
    int64_t file_size() const { return file_size_; }                  // whole packets
    int fd() const { return fd_; }                                    // read-only, for pread()
    const TsPacket& pat() const { return pat_; }
    const TsPacket& pmt() const { return pmt_; }
    uint16_t pmt_pid() const { return pmt_pid_; }
    uint16_t pcr_pid() const { return pcr_pid_; }
    const std::vector<Entry>& index() const { return index_; }

private:
    Archive() = default;

    std::string service_id_;
    std::string path_;
    int64_t duration_s_ = 0;
    double measured_duration_s_ = 0;
    int64_t file_size_ = 0;
    int fd_ = -1;
    TsPacket pat_{};
    TsPacket pmt_{};
    uint16_t pmt_pid_ = 0;
    uint16_t pcr_pid_ = 0;
    std::vector<Entry> index_;
};

}  // namespace srv
