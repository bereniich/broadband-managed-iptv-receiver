// Fake HTTP client of the AL unit tests (Section 5.3.1, "AL fakes"): a scripted response per request,
// by default the JSON examples of Section 3 from the files in fakes/responses/ (also checked by
// TST-533). Responses are delivered through FakeClock timers, as the PAL delivers them on its own
// thread, never inside get() / post_json(). Used from the test thread only.
#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "fakes/call_log.hpp"
#include "fakes/fake_clock.hpp"
#include "pal/platform.hpp"

namespace fakes {

class FakeHttpClient : public pal::IHttpClient {
public:
    struct Reply {
        pal::HttpResponse response{true, 200, "", ""};
        int delay_ms = 0;    // delivered after min(delay_ms, timeout_ms); a later one is a timeout
        bool hold = false;   // no timer and no timeout: delivered only by release() or respond()

        static Reply ok(int status, std::string body, int delay_ms = 0);
        static Reply file(int status, const std::string& name, int delay_ms = 0);   // body from responses/
        static Reply transport_failure(int delay_ms = 0);
        Reply held() const;
    };

    enum class State { Pending, Delivered, Cancelled };

    struct Request {
        RequestId id = 0;
        std::string method;   // "GET" or "POST"
        std::string url;
        std::string body;
        int timeout_ms = 0;
        State state = State::Pending;
        Reply reply;
    };

    static constexpr const char* kDefaultMwBaseUrl = "http://127.0.0.1:8080/api/v1";               // 4.2.5
    static constexpr const char* kDefaultStreamControlUrl = "http://127.0.0.1:8081/api/v1/stream"; // 3.2.2

    explicit FakeHttpClient(FakeClock& clock, CallLog* log = nullptr,
                            std::string mw_base_url = kDefaultMwBaseUrl,
                            std::string stream_control_url = kDefaultStreamControlUrl);
    ~FakeHttpClient() override;

    RequestId get(const std::string& url, int timeout_ms, Callback on_done) override;
    RequestId post_json(const std::string& url, const std::string& json_body, int timeout_ms,
                        Callback on_done) override;
    void cancel(RequestId id) override;

    // ---- test side
    // Queues a reply for the next request with this method and URL (FIFO per method and URL);
    // without a queued reply the default route is used.
    void script(const std::string& method, const std::string& url, Reply reply);
    // Delivers a held request with its scripted reply / with `reply`. False if it is not pending.
    bool release(RequestId id);
    bool respond(RequestId id, const Reply& reply);

    const std::vector<Request>& requests() const { return requests_; }
    std::size_t count(const std::string& method, const std::string& url_prefix) const;
    const Request* last(const std::string& method, const std::string& url_prefix) const;

    // Body of a file in fakes/responses/ (a missing file is a test failure).
    static std::string read_response_file(const std::string& name);

private:
    RequestId start_request(const std::string& method, const std::string& url, const std::string& body,
                            int timeout_ms, Callback on_done);
    Reply default_reply(const std::string& method, const std::string& url) const;
    void deliver(RequestId id, const pal::HttpResponse& response);
    Request* find(RequestId id);

    FakeClock& clock_;
    CallLog* log_;
    std::string mw_base_url_;
    std::string stream_control_url_;
    RequestId next_id_ = 1;
    std::vector<Request> requests_;
    std::map<RequestId, Callback> callbacks_;
    std::map<RequestId, pal::IClock::TimerId> timers_;
    std::map<std::pair<std::string, std::string>, std::deque<Reply>> scripts_;
};

}  // namespace fakes
