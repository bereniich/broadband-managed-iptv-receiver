#include "rtp_sender.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace srv {

SleepFn real_sleep() {
    return [](std::chrono::steady_clock::time_point t) { std::this_thread::sleep_until(t); };
}

namespace {

constexpr const char* kModule = "SRV-RTP";

std::chrono::nanoseconds ticks_to_ns(int64_t ticks) {
    return std::chrono::nanoseconds(ticks * 100000 / 9);  // 1 tick = 1/90 000 s
}

int64_t ns_to_ticks(std::chrono::nanoseconds ns) { return ns.count() * 9 / 100000; }

std::string seconds_text(double s) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", s);
    return text;
}

}  // namespace

RtpSender::RtpSender(std::shared_ptr<const Archive> archive, int udp_fd, RtpSessionParams params,
                     Archive::Position start, MonoClock clock, SleepFn sleep, Logger* logger)
    : archive_(std::move(archive)),
      udp_fd_(udp_fd),
      params_(std::move(params)),
      start_(start),
      clock_(std::move(clock)),
      sleep_(std::move(sleep)),
      logger_(logger),
      sequence_(params_.first_sequence) {
    destination_.sin_family = AF_INET;
    destination_.sin_port = htons(params_.client_port);
    inet_pton(AF_INET, params_.client_host.c_str(), &destination_.sin_addr);  // validated by SRV-HEC
}

RtpSender::~RtpSender() { stop(); }

void RtpSender::start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void RtpSender::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void RtpSender::send_datagram(const uint8_t* ts, int packets, int64_t content_ticks) {
    uint8_t datagram[kRtpHeaderSize + kTsPacketsPerBlock * kTsPacketSize];
    const uint32_t timestamp = params_.timestamp_base + static_cast<uint32_t>(content_ticks);  // mod 2^32
    datagram[0] = 0x80;  // V = 2, P = 0, X = 0, CC = 0
    datagram[1] = kRtpPayloadType;  // M = 0
    datagram[2] = static_cast<uint8_t>(sequence_ >> 8);
    datagram[3] = static_cast<uint8_t>(sequence_);
    datagram[4] = static_cast<uint8_t>(timestamp >> 24);
    datagram[5] = static_cast<uint8_t>(timestamp >> 16);
    datagram[6] = static_cast<uint8_t>(timestamp >> 8);
    datagram[7] = static_cast<uint8_t>(timestamp);
    datagram[8] = static_cast<uint8_t>(params_.ssrc >> 24);
    datagram[9] = static_cast<uint8_t>(params_.ssrc >> 16);
    datagram[10] = static_cast<uint8_t>(params_.ssrc >> 8);
    datagram[11] = static_cast<uint8_t>(params_.ssrc);
    const size_t length = kRtpHeaderSize + static_cast<size_t>(packets) * kTsPacketSize;
    std::memcpy(datagram + kRtpHeaderSize, ts, length - kRtpHeaderSize);

    // Errors (e.g. no receiver on loopback) are ignored: the stream is real time and the session
    // ends by stop or keepalive expiry (Section 4.7.6).
    (void)sendto(udp_fd_, datagram, length, 0, reinterpret_cast<const sockaddr*>(&destination_),
                 sizeof(destination_));
    ++sequence_;
    ++datagrams_;
}

void RtpSender::run() {
    const Archive& a = *archive_;
    const int64_t d_ticks = a.duration_ticks();
    const int64_t file_size = a.file_size();

    // 1. First datagram: the cached PAT and PMT (Section 3.4, "Start of session").
    uint8_t psi[2 * kTsPacketSize];
    std::memcpy(psi, a.pat().data(), kTsPacketSize);
    std::memcpy(psi + kTsPacketSize, a.pmt().data(), kTsPacketSize);
    const int64_t c0 = start_.c;
    const auto w0 = clock_();
    send_datagram(psi, 2, c0);
    if (logger_) {
        logger_->info(kModule, "send session_id=" + params_.session_id + " to " + params_.client_host + ":" +
                                   std::to_string(params_.client_port) + " from offset=" +
                                   std::to_string(start_.offset) + " content time " +
                                   seconds_text(static_cast<double>(c0) / kTicksPerSecond));
    }

    int64_t offset = start_.offset;
    int64_t loop = 0;  // L
    uint8_t block[kTsPacketsPerBlock * kTsPacketSize];
    while (!stop_) {
        // 3. Content time of the block: C = L * D + c.
        const int64_t content = loop * d_ticks + a.content_time(offset);
        const auto due = w0 + ticks_to_ns(content - c0);
        const auto now = clock_();
        if (now - due > kMaxLateness) {
            // 4. More than 1 s late: continue with the content of the current time.
            const int64_t c_now = c0 + ns_to_ticks(now - w0);
            loop = c_now / d_ticks;
            const Archive::Position p = a.entry_at_or_before(c_now % d_ticks);
            offset = p.offset;
            if (logger_) {
                const double late = std::chrono::duration<double>(now - due).count();
                logger_->warn(kModule, "late by " + seconds_text(late) + " s, skipping to content time " +
                                           seconds_text(static_cast<double>(p.c) / kTicksPerSecond) +
                                           " session_id=" + params_.session_id);
            }
            continue;
        }
        if (due > now) {
            sleep_(due);
            if (stop_) break;
        }
        // 2. A block of up to 7 TS packets that does not cross the end of the file.
        const int packets = static_cast<int>(
            std::min<int64_t>(kTsPacketsPerBlock, (file_size - offset) / kTsPacketSize));
        const size_t bytes = static_cast<size_t>(packets) * kTsPacketSize;
        size_t done = 0;
        while (done < bytes) {
            const ssize_t n = ::pread(a.fd(), block + done, bytes - done, static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        if (done != bytes) {
            if (logger_) {
                logger_->error(kModule, "cannot read " + a.path() + " at offset " + std::to_string(offset) +
                                            ", sender stopped session_id=" + params_.session_id);
            }
            break;
        }
        send_datagram(block, packets, content);
        offset += static_cast<int64_t>(bytes);
        if (offset >= file_size) {  // D-08: continue with the beginning of the file
            offset = 0;
            ++loop;
        }
    }
    if (logger_) {
        logger_->info(kModule, "sender ended session_id=" + params_.session_id +
                                   " datagrams=" + std::to_string(datagrams_.load()));
    }
}

}  // namespace srv
