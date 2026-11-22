// middleware/include/pal/platform.hpp — IF-INT-04, frozen at I-1
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pal {

constexpr std::size_t kTsPacketSize = 188;

// ================================================================ DTV SDK

// ---------------------------------------------------------------- Tuner (HAL-TUN)
// IP tuner: receives MPEG-TS over RTP/UDP (IF-EXT-05) and feeds the demux.
struct TuneParams {
    std::uint16_t udp_port;   // receiver UDP port
    std::uint32_t ssrc;       // only RTP packets with this SSRC are accepted
};

enum class TunerStatus { Unlocked, Locked };

class ITuner {
public:
    struct Stats {
        std::uint64_t rtp_packets;   // accepted RTP packets
        std::uint64_t rtp_lost;      // gaps detected in sequence numbers
        std::uint64_t rtp_invalid;   // datagrams dropped (bad header, PT != 33, other SSRC, payload not 1-7 x 188 bytes)
    };
    using StatusCallback = std::function<void(TunerStatus status)>;

    virtual ~ITuner() = default;
    // Must be called before Platform::start().
    virtual void set_status_handler(StatusCallback on_status) = 0;
    // Starts reception; replaces any previous tuning. The status is Unlocked after tune();
    // it becomes Locked at the first accepted packet, Unlocked after 3 s without accepted packets,
    // and Locked again when they return. Returns false if the port cannot be opened.
    virtual bool tune(const TuneParams& params) = 0;
    // Stops reception; no status callback and no demux data after it returns.
    virtual void untune() = 0;
    virtual Stats stats() const = 0;
};

// ---------------------------------------------------------------- Demux + descrambler (HAL-DMX, HAL-DSC)
enum class FilterType {
    Section,   // delivers complete PSI sections (table_id … CRC_32; the CRC is not checked by the demux)
    Ts         // delivers whole TS packets of the PID, one packet per callback
};

struct FilterConfig {
    FilterType type;
    std::uint16_t pid;
    int table_id = -1;   // Section filters only: deliver only sections with this table_id; -1 = all
};
// Several filters (of any type) may be open on the same PID at the same time; each receives its own copy.

class IDemux {
public:
    using FilterId = std::uint32_t;   // 0 = invalid
    struct FilterData {
        FilterId filter;
        const std::uint8_t* data;   // section or one TS packet (188 bytes), valid only during the callback
        std::size_t len;
        bool scrambled;             // Ts filters: the packet was received with transport_scrambling_control != '00'
                                    // (if a key is set for the PID, `data` is already decrypted and its bits are '00')
    };
    using DataCallback = std::function<void(const FilterData& data)>;
    struct Stats {
        std::uint64_t ts_packets;    // packets received from the tuner
        std::uint64_t sync_errors;   // packets dropped because the sync byte is not 0x47
        std::uint64_t cc_errors;     // continuity counter discontinuities
    };

    virtual ~IDemux() = default;
    // Creates a stopped filter; returns 0 if it cannot be created.
    virtual FilterId open_filter(const FilterConfig& config, DataCallback on_data) = 0;
    virtual bool start_filter(FilterId id) = 0;
    virtual void stop_filter(FilterId id) = 0;
    // Destroys the filter; no callback for it after return.
    virtual void close_filter(FilterId id) = 0;
    // Descrambler: AES-128 key (16 bytes) for the given PIDs, applied according to D-04.
    virtual void set_key(const std::uint16_t* pids, std::size_t pid_count, const std::uint8_t key[16]) = 0;
    // Removes all descrambler keys.
    virtual void clear_keys() = 0;
    virtual Stats stats() const = 0;
};

// ================================================================ Other platform services

// ---------------------------------------------------------------- HTTP client
struct HttpResponse {
    bool transport_ok;     // false: connection failed or timeout (status is then 0)
    int status;            // HTTP status code
    std::string body;      // response body
    std::string error;     // transport error text when transport_ok == false
};

class IHttpClient {
public:
    using RequestId = std::uint64_t;
    using Callback = std::function<void(const HttpResponse&)>;

    virtual ~IHttpClient() = default;
    // Asynchronous requests; the callback is called exactly once, unless the request is cancelled.
    virtual RequestId get(const std::string& url, int timeout_ms, Callback on_done) = 0;
    virtual RequestId post_json(const std::string& url, const std::string& json_body,
                                int timeout_ms, Callback on_done) = 0;
    virtual void cancel(RequestId id) = 0;
};

// ---------------------------------------------------------------- UI control channel (IF-INT-01 transport)
class IUiChannel {
public:
    using ClientId = std::uint64_t;
    using MessageCallback = std::function<void(ClientId client, const std::string& text)>;
    using ConnectionCallback = std::function<void(ClientId client, bool connected)>;

    virtual ~IUiChannel() = default;
    // Must be called before Platform::start().
    virtual void set_handlers(MessageCallback on_message, ConnectionCallback on_connection) = 0;
    virtual void send(ClientId client, const std::string& text) = 0;  // one WebSocket text frame
    virtual void broadcast(const std::string& text) = 0;              // to all connected clients
};

// ---------------------------------------------------------------- HLS file store (IF-INT-02 storage)
// Files of one session live in <hls_dir>/<session>/ and are served under /hls/<session>/.
class IHlsStore {
public:
    using SessionId = std::uint64_t;

    virtual ~IHlsStore() = default;
    // Creates the empty directory of `session`. Returns false on a file-system error.
    virtual bool open_session(SessionId session) = 0;
    // Writes a complete file atomically (temporary name, then rename), replacing an existing one.
    // `name` is a plain file name such as "seg-17.ts" or "index.m3u8". Returns false on error.
    virtual bool write_file(SessionId session, const std::string& name,
                            const std::uint8_t* data, std::size_t len) = 0;
    // Removes one file of `session`; removing a missing file is not an error.
    virtual bool remove_file(SessionId session, const std::string& name) = 0;
    // Removes the directory of `session` with all its files.
    virtual void close_session(SessionId session) = 0;
};

// ---------------------------------------------------------------- Clock and timers
class IClock {
public:
    using TimerId = std::uint64_t;
    using TimerCallback = std::function<void()>;

    virtual ~IClock() = default;
    virtual std::int64_t unix_time_s() const = 0;    // wall clock, UTC
    virtual std::int64_t monotonic_ms() const = 0;   // for timeouts and durations
    virtual TimerId start_timer(int delay_ms, TimerCallback on_expired) = 0;  // one-shot
    virtual void cancel_timer(TimerId id) = 0;
};

// ---------------------------------------------------------------- Logger
enum class LogLevel { Debug, Info, Warn, Error };

class ILogger {
public:
    virtual ~ILogger() = default;
    // Thread-safe. Line format: "<ISO time> <LEVEL> [<module>] <message>"
    virtual void log(LogLevel level, const char* module, const std::string& message) = 0;
};

// ================================================================ Platform (factory)
struct PlatformConfig {
    std::uint16_t ui_http_port = 8090;      // IF-INT-01/02/03
    std::string webui_dir = "webui";        // IF-INT-03
    std::string hls_dir = "/tmp/m15-hls";   // IF-INT-02; served under /hls/, emptied at start()
    std::string log_file = "logs/middleware.log";
};

class Platform {
public:
    virtual ~Platform() = default;
    // DTV SDK
    virtual ITuner& tuner() = 0;
    virtual IDemux& demux() = 0;
    // Other platform services
    virtual IHttpClient& http() = 0;
    virtual IUiChannel& ui() = 0;
    virtual IHlsStore& hls() = 0;
    virtual IClock& clock() = 0;
    virtual ILogger& logger() = 0;
    // Empties hls_dir, starts the event-loop thread and the HTTP/WebSocket server.
    // Returns false if the port cannot be opened or hls_dir cannot be created.
    virtual bool start() = 0;
    // Stops all PAL and HAL threads; no callback is called after it returns.
    virtual void stop() = 0;
};

// Implemented by WP-PAL (Linux: sockets, OpenSSL, Mongoose; HAL in software).
std::unique_ptr<Platform> create_platform(const PlatformConfig& config);

}  // namespace pal
