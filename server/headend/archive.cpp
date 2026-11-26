#include "archive.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace srv {

WindowCheck check_archive_window(int64_t t, int64_t now, int64_t window_s) {
    if (t >= now) return WindowCheck::FutureTime;
    if (t < now - window_s) return WindowCheck::OutOfArchive;
    return WindowCheck::Ok;
}

namespace {

constexpr int64_t kPcrModulus = int64_t{1} << 33;  // PCR_base is a 33-bit counter

int64_t floor_mod(int64_t a, int64_t b) {
    const int64_t r = a % b;
    return r < 0 ? r + b : r;
}

uint16_t packet_pid(const uint8_t* p) { return static_cast<uint16_t>(((p[1] & 0x1F) << 8) | p[2]); }

// Index of the first payload byte, or -1 if the packet has no payload.
int payload_start(const uint8_t* p) {
    const int afc = (p[3] >> 4) & 0x3;
    if ((afc & 0x1) == 0) return -1;
    const int start = (afc & 0x2) ? 5 + p[4] : 4;
    return start < kTsPacketSize ? start : -1;
}

// PCR_base of a packet with an adaptation field carrying a PCR, or -1.
int64_t packet_pcr_base(const uint8_t* p) {
    const int afc = (p[3] >> 4) & 0x3;
    if ((afc & 0x2) == 0 || p[4] < 7 || (p[5] & 0x10) == 0) return -1;
    return (int64_t{p[6]} << 25) | (int64_t{p[7]} << 17) | (int64_t{p[8]} << 9) | (int64_t{p[9]} << 1) |
           (int64_t{p[10]} >> 7);
}

// Start of a PSI section with `table_id` that begins and ends in this packet, or -1.
// `end` receives the index after the last section byte (CRC included).
int single_packet_section(const uint8_t* p, uint8_t table_id, int& end) {
    if ((p[1] & 0x40) == 0) return -1;  // payload_unit_start_indicator
    const int ps = payload_start(p);
    if (ps < 0) return -1;
    const int sec = ps + 1 + p[ps];  // pointer_field
    if (sec + 3 > kTsPacketSize || p[sec] != table_id) return -1;
    const int length = ((p[sec + 1] & 0x0F) << 8) | p[sec + 2];
    end = sec + 3 + length;
    return end <= kTsPacketSize ? sec : -2;  // -2: section continues in another packet
}

}  // namespace

std::shared_ptr<const Archive> Archive::load(const Service& service, const std::string& path) {
    auto fail = [&](const std::string& reason) -> ArchiveError {
        return ArchiveError(service.service_id + ": " + path + ": " + reason);
    };
    std::shared_ptr<Archive> a(new Archive());
    a->service_id_ = service.service_id;
    a->path_ = path;
    a->duration_s_ = service.cycle_duration();

    a->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (a->fd_ < 0) throw fail(std::string("cannot open content file: ") + std::strerror(errno));
    struct stat st {};
    if (fstat(a->fd_, &st) != 0 || !S_ISREG(st.st_mode)) throw fail("not a regular file");
    if (st.st_size < kTsPacketSize || st.st_size % kTsPacketSize != 0) {
        throw fail("size " + std::to_string(st.st_size) + " is not a positive multiple of 188 bytes");
    }
    a->file_size_ = st.st_size;

    bool have_pat = false, have_pmt = false;
    int64_t first_pcr = -1, previous_c = -1;
    std::vector<uint8_t> buffer(static_cast<size_t>(kTsPacketSize) * 8192);
    int64_t offset = 0;
    while (offset < a->file_size_) {
        const ssize_t n = ::read(a->fd_, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || n % kTsPacketSize != 0) throw fail("read error at offset " + std::to_string(offset));
        for (ssize_t i = 0; i < n; i += kTsPacketSize, offset += kTsPacketSize) {
            const uint8_t* p = buffer.data() + i;
            if (p[0] != 0x47) throw fail("no sync byte at offset " + std::to_string(offset));
            const uint16_t pid = packet_pid(p);
            int end = 0;
            if (!have_pat && pid == 0) {
                const int sec = single_packet_section(p, 0x00, end);
                if (sec == -2) throw fail("the first PAT does not fit in one TS packet");
                if (sec >= 0) {
                    for (int k = sec + 8; k + 4 <= end - 4; k += 4) {
                        const int program = (p[k] << 8) | p[k + 1];
                        if (program != 0) {
                            a->pmt_pid_ = static_cast<uint16_t>(((p[k + 2] & 0x1F) << 8) | p[k + 3]);
                            std::copy(p, p + kTsPacketSize, a->pat_.begin());
                            have_pat = true;
                            break;
                        }
                    }
                }
            } else if (have_pat && !have_pmt && pid == a->pmt_pid_) {
                const int sec = single_packet_section(p, 0x02, end);
                if (sec == -2) throw fail("the first PMT does not fit in one TS packet");
                if (sec >= 0 && sec + 10 <= end) {
                    a->pcr_pid_ = static_cast<uint16_t>(((p[sec + 8] & 0x1F) << 8) | p[sec + 9]);
                    std::copy(p, p + kTsPacketSize, a->pmt_.begin());
                    have_pmt = true;
                }
            }
            if (have_pmt && pid == a->pcr_pid_) {
                const int64_t pcr = packet_pcr_base(p);
                if (pcr >= 0) {
                    if (first_pcr < 0) first_pcr = pcr;
                    const int64_t c = floor_mod(pcr - first_pcr, kPcrModulus);
                    if (!a->index_.empty() && c <= previous_c) {
                        throw fail("PCR does not increase at offset " + std::to_string(offset));
                    }
                    a->index_.push_back({offset, c});
                    previous_c = c;
                }
            }
        }
    }
    if (!have_pat) throw fail("no PAT with a program");
    if (!have_pmt) throw fail("no PMT");
    if (a->index_.size() < 2) throw fail("fewer than 2 PCR values on the PCR PID " + std::to_string(a->pcr_pid_));

    // Measured duration: last c + one (average) PCR interval (Section 4.7.4, step 3).
    const int64_t last_c = a->index_.back().c;
    const double interval = static_cast<double>(last_c) / static_cast<double>(a->index_.size() - 1);
    a->measured_duration_s_ = (static_cast<double>(last_c) + interval) / kTicksPerSecond;
    if (a->duration_s_ <= 0 || std::fabs(a->measured_duration_s_ - static_cast<double>(a->duration_s_)) > 0.1) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "measured duration %.3f s differs from the program durations of services.json "
                      "(%lld s) by more than 0.1 s",
                      a->measured_duration_s_, static_cast<long long>(a->duration_s_));
        throw fail(text);
    }
    return a;
}

Archive::~Archive() {
    if (fd_ >= 0) ::close(fd_);
}

Archive::Position Archive::entry_at_or_before(int64_t c_ticks) const {
    auto it = std::upper_bound(index_.begin(), index_.end(), c_ticks,
                               [](int64_t c, const Entry& e) { return c < e.c; });
    if (it != index_.begin()) --it;
    return {it->offset, it->c};
}

Archive::Position Archive::position(int64_t t) const {
    const int64_t p = floor_mod(t, duration_s_) * kTicksPerSecond;
    return entry_at_or_before(p);
}

int64_t Archive::content_time(int64_t offset) const {
    auto it = std::upper_bound(index_.begin(), index_.end(), offset,
                               [](int64_t off, const Entry& e) { return off < e.offset; });
    if (it == index_.begin()) return 0;  // before the first PCR: the start of the content
    const Entry& e = *(it - 1);
    const Entry& a = (it == index_.end()) ? *(it - 2) : e;  // interval used for the rate
    const Entry& b = (it == index_.end()) ? *(it - 1) : *it;
    const int64_t c = e.c + (offset - e.offset) * (b.c - a.c) / (b.offset - a.offset);
    return std::clamp<int64_t>(c, 0, duration_ticks());
}

}  // namespace srv
