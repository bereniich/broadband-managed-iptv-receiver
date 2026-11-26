// AL-HEC: head-end client (Sections 2.2.4 and 4.2.4; IF-EXT-04 of Section 3.3; REQ-12, REQ-67, REQ-70).
// Builds the JSON bodies of POST /start, /keepalive and /stop and interprets the responses: 200 -> result,
// 4xx with an error object -> that error code, transport failure, timeout or an invalid body ->
// HEADEND_UNREACHABLE; SESSION_NOT_FOUND on /stop counts as success. Decisions (FUTURE_TIME retry,
// counting failed keepalives) are AL-SES. HTTP callbacks only post a control task (rule T1).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "pal/platform.hpp"
#include "task_queue.hpp"

namespace al {

struct StartRequest {   // body of POST /start (Section 3.3.1)
    std::string service_id;
    std::optional<std::int64_t> start;   // nullopt = "live"
    std::string client_host;
    std::uint16_t client_port = 0;
    std::uint32_t ssrc = 0;
};

struct StartResult {    // response 200 of POST /start
    std::string session_id;
    std::int64_t start_time = 0;
    std::uint32_t ssrc = 0;
};

// Error object of Section 3.1 ({"code", "message"}); an empty code means success.
struct HeadendError {
    std::string code;
    std::string message;
    bool ok() const { return code.empty(); }
};

constexpr const char* kHeadendUnreachable = "HEADEND_UNREACHABLE";
constexpr const char* kSessionNotFound = "SESSION_NOT_FOUND";

class HeadendClient {
public:
    using Token = std::uint64_t;
    using StartDone = std::function<void(const HeadendError&, const StartResult*)>;
    using Done = std::function<void(const HeadendError&)>;

    HeadendClient(pal::IHttpClient& http, pal::ILogger& logger, TaskQueue& queue, int timeout_ms = 2000);
    ~HeadendClient();

    HeadendClient(const HeadendClient&) = delete;
    HeadendClient& operator=(const HeadendClient&) = delete;

    // `url` is the stream_control_url of the service; `session` is only used in the log line.
    Token start(std::uint64_t session, const std::string& url, const StartRequest& request, StartDone done);
    // Result: success, SESSION_NOT_FOUND (T-28) or HEADEND_UNREACHABLE (T-29).
    Token keepalive(const std::string& url, const std::string& session_id, Done done);
    // Result: success (also for SESSION_NOT_FOUND, Section 3.3.2) or HEADEND_UNREACHABLE.
    Token stop(const std::string& url, const std::string& session_id, Done done);
    // The pending request is cancelled; `done` is never called.
    void cancel(Token token);

private:
    enum class Kind { Start, Keepalive, Stop };
    struct Pending {
        Kind kind;
        std::optional<pal::IHttpClient::RequestId> http;
        std::uint64_t session = 0;
        std::string session_id;
        StartDone start_done;
        Done done;
    };

    Token send(Kind kind, const std::string& url, const std::string& body, Pending pending);
    void on_response(Token token, const pal::HttpResponse& response);

    pal::IHttpClient& http_;
    pal::ILogger& logger_;
    TaskQueue& queue_;
    int timeout_ms_;
    Token next_token_ = 1;
    std::map<Token, Pending> pending_;
};

}  // namespace al
