// AL-SVC: service and metadata manager (Sections 2.2.4, 4.2.3 and 4.2.4; REQ-01, REQ-02, REQ-05,
// REQ-12, REQ-32; IF-EXT-03 of Section 3.2). Loads and validates the service list with retries, keeps
// it when the OTT MW server becomes unreachable, caches the service info and passes the EPG through
// after validation, without a cache. Tracks the reachability of the server for mw_status. HTTP
// callbacks only post a control task (rule T1); everything else runs on the AL worker.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "pal/platform.hpp"
#include "task_queue.hpp"

namespace al {

struct ServiceEntry {   // one validated entry of GET /services (REQ-02)
    std::string service_id;
    int number = 0;
    std::string name;
    bool is_protected = false;
};

struct ServiceInfo {    // the fields of GET /services/{id} used by the AL (Section 3.2.2)
    std::string stream_control_url;
    int archive_window_s = 0;
};

enum class MwError { None, MwUnreachable, ServiceNotFound, InvalidRange };

// Error code of Section 3.10 ("" for None).
const char* to_code(MwError error);

class ServiceManager {
public:
    struct Config {   // Section 4.2.5
        std::string mw_base_url = "http://127.0.0.1:8080/api/v1";
        int timeout_ms = 800;   // mw_timeout_ms
        int retry_s = 5;        // mw_retry_s
    };

    using Token = std::uint64_t;
    using InfoDone = std::function<void(MwError, const ServiceInfo*)>;
    // The EPG result of Section 3.6.2: {"server_time": …, "services": [ … ]} with the valid events.
    using EpgDone = std::function<void(MwError, const nlohmann::json*)>;

    ServiceManager(pal::IHttpClient& http, pal::IClock& clock, pal::ILogger& logger, TaskQueue& queue,
                   Config config);
    ~ServiceManager();

    ServiceManager(const ServiceManager&) = delete;
    ServiceManager& operator=(const ServiceManager&) = delete;

    // Called on the worker when the reachability is first known and whenever it changes (mw_status).
    void set_status_handler(std::function<void(bool reachable)> handler) { status_handler_ = std::move(handler); }

    // REQ-01: GET /services; on failure repeated every retry_s seconds until it succeeds.
    void load_services();

    bool loaded() const { return loaded_; }
    const std::vector<ServiceEntry>& services() const { return services_; }   // sorted by number
    const ServiceEntry* find(const std::string& service_id) const;
    std::optional<bool> reachable() const { return reachable_; }               // nullopt: not known yet

    // REQ-12: from the cache, or GET /services/{id}. `done` is called once on the worker, unless
    // the token is cancelled.
    Token get_service_info(const std::string& service_id, InfoDone done);
    // REQ-32: GET /epg?from=&to=, validated, no cache.
    Token get_epg(std::int64_t from, std::int64_t to, EpgDone done);
    // Rule 4 of Section 4.3.1: the pending request is cancelled, `done` is never called.
    void cancel(Token token);

private:
    struct Pending {
        std::optional<pal::IHttpClient::RequestId> http;
        InfoDone info_done;
        EpgDone epg_done;
        std::string service_id;
    };

    void on_services(const pal::HttpResponse& response);
    void on_service_info(Token token, const pal::HttpResponse& response);
    void on_epg(Token token, const pal::HttpResponse& response);
    void retry_later(const std::string& why);
    void set_reachable(bool reachable);
    // True when the server answered with a valid body or a valid error object.
    void note_reachability(const pal::HttpResponse& response, bool valid_body);
    void log(pal::LogLevel level, const std::string& message);

    pal::IHttpClient& http_;
    pal::IClock& clock_;
    pal::ILogger& logger_;
    TaskQueue& queue_;
    Config config_;
    std::function<void(bool)> status_handler_;

    bool loaded_ = false;
    bool loading_ = false;
    std::optional<pal::IHttpClient::RequestId> services_request_;
    std::optional<pal::IClock::TimerId> retry_timer_;
    std::vector<ServiceEntry> services_;
    std::map<std::string, std::size_t> index_;
    std::map<std::string, ServiceInfo> info_cache_;
    std::optional<bool> reachable_;
    Token next_token_ = 1;
    std::map<Token, Pending> pending_;
};

}  // namespace al
