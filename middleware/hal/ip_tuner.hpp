// HAL-TUN: software IP tuner (REQ-13, IF-EXT-05, Section 4.5).
//
// Receives RTP datagrams on 127.0.0.1:<port> (Section 4.2.5) on its own receive thread, accepts only
// V = 2, no padding / extension / CSRC, PT = 33, the tuned SSRC and a payload of 1 – 7 whole TS
// packets, and passes the TS packets of each accepted datagram to the demux callback. Lock status:
// Locked at the first accepted datagram after tune(), Unlocked after 3 s without accepted datagrams,
// Locked again when they return.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "hal_log.hpp"

namespace hal {

constexpr std::size_t kTsPacketSize = 188;

class IpTuner {
public:
    struct Stats {
        std::uint64_t rtp_packets = 0;  // accepted datagrams
        std::uint64_t rtp_lost = 0;     // sequence-number gaps (forward), wrap 65535 -> 0 is no gap
        std::uint64_t rtp_invalid = 0;  // dropped datagrams
    };
    // `ts` points into the datagram buffer B2 (writable, so that the descrambler can work in place);
    // valid only during the call. Called on the receive thread.
    using PacketsCallback = std::function<void(std::uint8_t* ts, std::size_t packet_count)>;
    // Called on the receive thread when the lock status changes (true = Locked).
    using LockCallback = std::function<void(bool locked)>;

    struct Options {
        std::chrono::milliseconds unlock_after{3000};  // REQ-13
        std::chrono::milliseconds poll_timeout{100};   // Section 4.5.2: receive with a 100 ms timeout
        int receive_buffer_bytes = 1 << 20;            // B1: SO_RCVBUF requested (1 MiB)
    };

    IpTuner(PacketsCallback on_packets, LockCallback on_lock, LogFn log = nullptr);
    IpTuner(PacketsCallback on_packets, LockCallback on_lock, LogFn log, Options options);
    ~IpTuner();

    IpTuner(const IpTuner&) = delete;
    IpTuner& operator=(const IpTuner&) = delete;

    // Replaces any previous tuning: binds 127.0.0.1:<udp_port> (0 = an ephemeral port, for tests),
    // resets the statistics and the lock status (Unlocked, without a callback) and starts the receive
    // thread. Returns false, logged, if the port cannot be opened; the tuner is then untuned.
    bool tune(std::uint16_t udp_port, std::uint32_t ssrc);

    // Stops reception; returns after the receive thread has ended, so no callback follows.
    // tune() and untune() called from the tuner's own callbacks (the receive thread, which cannot
    // join itself) are refused: logged as an error, tune() returns false, untune() has no effect.
    void untune();

    Stats stats() const;
    // True on the receive thread, i.e. inside the tuner's own callbacks.
    bool on_receive_thread() const { return std::this_thread::get_id() == receive_thread_id_.load(); }
    std::uint16_t bound_port() const { return bound_port_; }
    // SO_RCVBUF reported by the kernel after the request of Options::receive_buffer_bytes (B1).
    int effective_receive_buffer() const { return effective_rcvbuf_; }

private:
    void run(int socket_fd, int wake_fd, std::uint32_t ssrc);
    void log(LogLevel level, const std::string& message) const;

    PacketsCallback on_packets_;
    LockCallback on_lock_;
    LogFn log_;
    Options options_;

    std::mutex control_mutex_;  // serializes tune() / untune()
    std::thread thread_;
    int socket_fd_ = -1;
    int wake_fd_ = -1;  // eventfd that wakes the receive thread on untune()
    std::atomic<bool> stop_{false};
    std::atomic<std::thread::id> receive_thread_id_{};
    std::atomic<std::uint16_t> bound_port_{0};
    std::atomic<int> effective_rcvbuf_{0};

    std::atomic<std::uint64_t> rtp_packets_{0};
    std::atomic<std::uint64_t> rtp_lost_{0};
    std::atomic<std::uint64_t> rtp_invalid_{0};
};

}  // namespace hal
