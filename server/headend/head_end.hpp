// SRV-HEC: stream control API IF-EXT-04 (Section 3.3) with the session table and the keepalive
// watchdog (Section 4.7.6).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#include "archive.hpp"
#include "clock.hpp"
#include "http.hpp"
#include "log.hpp"
#include "rtp_sender.hpp"
#include "services.hpp"

namespace srv {

struct HeadEndOptions {
    WallClock wall_clock = system_wall_clock();     // "live" start time and archive window
    MonoClock session_clock = system_mono_clock();  // last_seen and watchdog (REQ-68)
    MonoClock sender_clock = system_mono_clock();   // pacing of the senders (Section 4.7.5)
    SleepFn sender_sleep = real_sleep();
    // Session expiry of D-06 / REQ-68 and the watchdog period of Section 4.7.6. Section 4.7.1 lists
    // no configuration keys for them, so they are constants of the head-end (see the step report).
    std::chrono::seconds session_expiry{15};
    std::chrono::milliseconds watchdog_period{1000};
    bool start_watchdog = true;  // false in tests that call watchdog_tick() themselves
    uint64_t seed = 0;           // random sequence numbers and timestamp bases; 0 = std::random_device
};

class HeadEnd {
public:
    static constexpr const char* kBasePath = "/api/v1/stream";

    // `archives` must hold an archive for every service of `catalog`. Creates the UDP socket of the
    // senders and starts the watchdog thread; throws std::runtime_error if the socket fails.
    HeadEnd(ServiceCatalog catalog, std::map<std::string, std::shared_ptr<const Archive>> archives,
            HeadEndOptions options = {}, Logger* logger = nullptr);
    ~HeadEnd();

    HeadEnd(const HeadEnd&) = delete;
    HeadEnd& operator=(const HeadEnd&) = delete;

    // POST <base>/start, /stop, /keepalive; everything else gets 400 / 404 with an error object.
    HttpResponse handle(const HttpRequest& request);

    // One watchdog pass: stops every session with now - last_seen >= session_expiry.
    void watchdog_tick();

    // Ids of the running sessions (for tests and the log).
    std::vector<std::string> session_ids() const;

    // Stops the watchdog and every session; called by the destructor.
    void shutdown();

private:
    struct Session {
        std::string id;
        std::string endpoint;  // client_host:client_port
        std::chrono::steady_clock::time_point last_seen;
        std::unique_ptr<RtpSender> sender;
    };

    HttpResponse route(const HttpRequest& request);
    HttpResponse start(const nlohmann::json& body);
    HttpResponse keepalive(const nlohmann::json& body);
    HttpResponse stop(const nlohmann::json& body);
    // Stops the sender and removes the session; mutex_ must be held.
    void remove_locked(const std::string& session_id);

    ServiceCatalog catalog_;
    std::map<std::string, std::shared_ptr<const Archive>> archives_;
    HeadEndOptions options_;
    Logger* logger_;
    int udp_fd_ = -1;

    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;  // session_id -> session
    std::map<std::string, std::string> by_endpoint_;            // client_host:client_port -> session_id
    uint64_t next_id_ = 1;
    std::mt19937_64 rng_;

    std::mutex watchdog_mutex_;
    std::condition_variable watchdog_cv_;
    bool watchdog_stop_ = false;
    std::thread watchdog_;
};

}  // namespace srv
