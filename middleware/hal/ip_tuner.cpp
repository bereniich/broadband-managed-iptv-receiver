#include "ip_tuner.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace hal {

namespace {

constexpr const char* kModule = "HAL-TUN";
constexpr std::size_t kRtpHeaderSize = 12;
constexpr std::size_t kDatagramBufferSize = 2048;  // B2
constexpr int kPayloadTypeMp2t = 33;

}  // namespace

IpTuner::IpTuner(PacketsCallback on_packets, LockCallback on_lock, LogFn log)
    : IpTuner(std::move(on_packets), std::move(on_lock), std::move(log), Options{}) {}

IpTuner::IpTuner(PacketsCallback on_packets, LockCallback on_lock, LogFn log, Options options)
    : on_packets_(std::move(on_packets)), on_lock_(std::move(on_lock)), log_(std::move(log)), options_(options) {}

IpTuner::~IpTuner() { untune(); }

void IpTuner::log(LogLevel level, const std::string& message) const {
    if (log_) log_(level, kModule, message);
}

IpTuner::Stats IpTuner::stats() const {
    Stats s;
    s.rtp_packets = rtp_packets_.load();
    s.rtp_lost = rtp_lost_.load();
    s.rtp_invalid = rtp_invalid_.load();
    return s;
}

bool IpTuner::tune(std::uint16_t udp_port, std::uint32_t ssrc) {
    untune();
    std::lock_guard<std::mutex> lock(control_mutex_);
    rtp_packets_ = 0;
    rtp_lost_ = 0;
    rtp_invalid_ = 0;

    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log(LogLevel::Error, std::string("cannot create the UDP socket: ") + std::strerror(errno));
        return false;
    }
    // B1: request 1 MiB; the kernel limits it by net.core.rmem_max, which is not changed.
    const int requested = options_.receive_buffer_bytes;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &requested, sizeof(requested));
    int effective = 0;
    socklen_t length = sizeof(effective);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &effective, &length);
    effective_rcvbuf_ = effective;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(udp_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only (Section 4.2.5)
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log(LogLevel::Error, "cannot open UDP port 127.0.0.1:" + std::to_string(udp_port) + ": " +
                                 std::strerror(errno));
        ::close(fd);
        return false;
    }
    socklen_t addr_length = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addr_length);
    bound_port_ = ntohs(addr.sin_port);

    const int wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake < 0) {
        log(LogLevel::Error, std::string("cannot create the eventfd: ") + std::strerror(errno));
        ::close(fd);
        return false;
    }
    socket_fd_ = fd;
    wake_fd_ = wake;
    stop_ = false;
    log(LogLevel::Info, "tuned 127.0.0.1:" + std::to_string(bound_port_.load()) + " ssrc=" + std::to_string(ssrc) +
                            " SO_RCVBUF requested " + std::to_string(requested) + " effective " +
                            std::to_string(effective));
    thread_ = std::thread([this, fd, wake, ssrc] { run(fd, wake, ssrc); });
    return true;
}

void IpTuner::untune() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!thread_.joinable()) return;
    stop_ = true;
    const std::uint64_t one = 1;
    if (::write(wake_fd_, &one, sizeof(one)) < 0) {
        // cannot happen for an eventfd counter far below its maximum; the 100 ms poll timeout ends the wait anyway
    }
    thread_.join();
    ::close(socket_fd_);
    ::close(wake_fd_);
    socket_fd_ = -1;
    wake_fd_ = -1;
    bound_port_ = 0;
}

void IpTuner::run(int socket_fd, int wake_fd, std::uint32_t ssrc) {
    using clock = std::chrono::steady_clock;
    alignas(16) std::uint8_t buffer[kDatagramBufferSize];  // B2, reused for every datagram
    bool locked = false;
    bool have_sequence = false;
    std::uint16_t last_sequence = 0;
    clock::time_point last_accepted{};
    pollfd fds[2] = {{socket_fd, POLLIN, 0}, {wake_fd, POLLIN, 0}};
    const int timeout_ms = static_cast<int>(options_.poll_timeout.count());

    while (!stop_) {
        const int ready = ::poll(fds, 2, timeout_ms);
        if (stop_) break;
        if (ready > 0 && (fds[0].revents & POLLIN)) {
            // MSG_TRUNC returns the real length, so that a datagram larger than B2 is rejected.
            const ssize_t n = ::recv(socket_fd, buffer, sizeof(buffer), MSG_TRUNC | MSG_DONTWAIT);
            if (n >= 0) {
                const std::size_t size = static_cast<std::size_t>(n);
                const std::size_t payload = size >= kRtpHeaderSize ? size - kRtpHeaderSize : 0;
                const bool header_ok = size >= kRtpHeaderSize && size <= sizeof(buffer) &&
                                       buffer[0] == 0x80 &&                       // V = 2, P = 0, X = 0, CC = 0
                                       (buffer[1] & 0x7F) == kPayloadTypeMp2t;   // M not checked
                const std::uint32_t packet_ssrc = header_ok ? (std::uint32_t{buffer[8]} << 24) |
                                                                  (std::uint32_t{buffer[9]} << 16) |
                                                                  (std::uint32_t{buffer[10]} << 8) | buffer[11]
                                                            : 0;
                const bool payload_ok = payload >= kTsPacketSize && payload <= 7 * kTsPacketSize &&
                                        payload % kTsPacketSize == 0;
                if (!header_ok || packet_ssrc != ssrc || !payload_ok) {
                    ++rtp_invalid_;
                } else {
                    ++rtp_packets_;
                    const std::uint16_t sequence = static_cast<std::uint16_t>((buffer[2] << 8) | buffer[3]);
                    if (have_sequence) {
                        const std::uint16_t gap = static_cast<std::uint16_t>(sequence - last_sequence - 1);
                        if (gap < 0x8000) {  // forward: gap datagrams missing (0 = in order, also across the wrap)
                            rtp_lost_ += gap;
                            last_sequence = sequence;
                        }  // else: a late or duplicate datagram; passed on, sequence kept (no reordering)
                    } else {
                        last_sequence = sequence;
                        have_sequence = true;
                    }
                    last_accepted = clock::now();
                    if (!locked) {
                        locked = true;
                        if (on_lock_) on_lock_(true);
                    }
                    if (on_packets_) on_packets_(buffer + kRtpHeaderSize, payload / kTsPacketSize);
                }
            }
        }
        if (locked && clock::now() - last_accepted >= options_.unlock_after) {
            locked = false;
            if (on_lock_) on_lock_(false);
        }
    }
}

}  // namespace hal
