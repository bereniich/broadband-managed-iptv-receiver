// SRV-RTP: RTP sender of one head-end session, paced by the PCR of the content (Section 4.7.5,
// IF-EXT-05 of Section 3.4).
#pragma once

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "archive.hpp"
#include "clock.hpp"
#include "log.hpp"

namespace srv {

// Sleeps until a time of the sender's monotonic clock. Injected, so that a test can make the clock
// jump (TST-315); the default is std::this_thread::sleep_until.
using SleepFn = std::function<void(std::chrono::steady_clock::time_point)>;
SleepFn real_sleep();

constexpr int kRtpHeaderSize = 12;
constexpr int kRtpPayloadType = 33;    // MP2T (RFC 3551)
constexpr int kTsPacketsPerBlock = 7;  // 7 x 188 = 1316 bytes per datagram
constexpr std::chrono::seconds kMaxLateness{1};  // Section 4.7.5 step 4

struct RtpSessionParams {
    std::string session_id;      // for the log lines
    std::string client_host;     // IPv4 literal
    uint16_t client_port = 0;
    uint32_t ssrc = 0;
    uint16_t first_sequence = 0;  // random per session (Section 3.4)
    uint32_t timestamp_base = 0;  // random per session (Section 4.7.5)
};

// One sender thread per session. The first datagram carries the cached PAT and PMT, then blocks of
// 7 TS packets from the start position, sent at monotonic time w0 + (C - c0) where C = L * D + c is
// the content time of the block (L = loop counter, c interpolated between PCR index entries). A
// block never crosses the end of the file; after the last block the file starts again at offset 0.
// More than 1 s late, the sender skips to the content of the current time and logs a warning.
class RtpSender {
public:
    // `udp_fd`: UDP socket used with sendto(); it may be shared with other senders.
    RtpSender(std::shared_ptr<const Archive> archive, int udp_fd, RtpSessionParams params,
              Archive::Position start, MonoClock clock = system_mono_clock(), SleepFn sleep = real_sleep(),
              Logger* logger = nullptr);
    ~RtpSender();

    RtpSender(const RtpSender&) = delete;
    RtpSender& operator=(const RtpSender&) = delete;

    void start();
    // Sets the stop flag (checked before every block) and returns after the thread has ended; no
    // datagram is sent after the return.
    void stop();

    uint64_t datagrams_sent() const { return datagrams_; }

private:
    void run();
    void send_datagram(const uint8_t* ts, int packets, int64_t content_ticks);

    std::shared_ptr<const Archive> archive_;
    int udp_fd_;
    RtpSessionParams params_;
    sockaddr_in destination_{};  // client_host:client_port, built once
    Archive::Position start_;
    MonoClock clock_;
    SleepFn sleep_;
    Logger* logger_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> datagrams_{0};
    uint16_t sequence_ = 0;
    std::thread thread_;
};

}  // namespace srv
