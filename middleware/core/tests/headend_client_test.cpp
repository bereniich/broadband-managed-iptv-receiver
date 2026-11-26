// AL-HEC module tests (step 7b): TST-156. Request bodies and responses are the JSON examples of
// Section 3.3 (fake HTTP client, step 3b); the error mapping is that of Section 4.2.4 (AL-HEC).
#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <json.hpp>

#include "fakes/call_log.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_http_client.hpp"
#include "fakes/fake_logger.hpp"
#include "headend_client.hpp"
#include "task_queue.hpp"

namespace {

using fakes::FakeHttpClient;
using nlohmann::json;

const std::string kHe = "http://127.0.0.1:8081/api/v1/stream";   // stream_control_url of Section 3.2.2

struct Harness {
    fakes::FakeClock clock;
    fakes::CallLog calls;
    FakeHttpClient http{clock, &calls};
    fakes::FakeLogger logger;
    al::TaskQueue queue{clock, logger};
    al::HeadendClient hec{http, logger, queue};

    void settle() {
        for (int i = 0; i < 1000; ++i) {
            clock.advance(0);
            if (queue.run_pending() == 0) return;
        }
        ADD_FAILURE() << "queue does not settle";
    }
    void advance(std::int64_t ms) {
        clock.advance(ms);
        settle();
    }

    struct StartOutcome {
        bool called = false;
        al::HeadendError error;
        std::optional<al::StartResult> result;
    };
    StartOutcome start(const al::StartRequest& request) {
        StartOutcome out;
        hec.start(1, kHe, request, [&out](const al::HeadendError& e, const al::StartResult* r) {
            out.called = true;
            out.error = e;
            if (r) out.result = *r;
        });
        settle();
        return out;
    }
    std::optional<al::HeadendError> keepalive() {
        std::optional<al::HeadendError> out;
        hec.keepalive(kHe, "hs-000017", [&out](const al::HeadendError& e) { out = e; });
        settle();
        return out;
    }
    std::optional<al::HeadendError> stop() {
        std::optional<al::HeadendError> out;
        hec.stop(kHe, "hs-000017", [&out](const al::HeadendError& e) { out = e; });
        settle();
        return out;
    }
};

// The two requests of Section 3.3.1.
al::StartRequest archive_request() {
    al::StartRequest r;
    r.service_id = "svc-3";
    r.start = 1794999600;
    r.client_host = "127.0.0.1";
    r.client_port = 5004;
    r.ssrc = 3054235519u;
    return r;
}

al::StartRequest live_request() {
    al::StartRequest r;
    r.service_id = "svc-1";
    r.client_host = "127.0.0.1";
    r.client_port = 5004;
    r.ssrc = 1180331842u;
    return r;
}

// ---------------------------------------------------------------- TST-156

TEST(Tst156, RequestBodies) {
    Harness h;
    h.start(archive_request());
    h.start(live_request());
    h.keepalive();
    h.stop();

    const auto& requests = h.http.requests();
    ASSERT_EQ(requests.size(), 4u);
    // Section 3.3.1, archive and live.
    EXPECT_EQ(requests[0].url, kHe + "/start");
    const json archive = json::parse(requests[0].body);
    EXPECT_EQ(archive, json::parse(R"({ "service_id": "svc-3", "start": 1794999600, "client_host": "127.0.0.1", "client_port": 5004, "ssrc": 3054235519 })"));
    EXPECT_TRUE(archive["start"].is_number_integer());
    EXPECT_TRUE(archive["ssrc"].is_number_unsigned());   // above INT32_MAX: an unsigned 32-bit integer
    EXPECT_GT(archive["ssrc"].get<std::uint64_t>(), static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()));
    const json live = json::parse(requests[1].body);
    EXPECT_EQ(live, json::parse(R"({ "service_id": "svc-1", "start": "live", "client_host": "127.0.0.1", "client_port": 5004, "ssrc": 1180331842 })"));
    // Sections 3.3.3 and 3.3.2.
    EXPECT_EQ(requests[2].url, kHe + "/keepalive");
    EXPECT_EQ(json::parse(requests[2].body), json::parse(R"({ "session_id": "hs-000017" })"));
    EXPECT_EQ(requests[3].url, kHe + "/stop");
    EXPECT_EQ(json::parse(requests[3].body), json::parse(R"({ "session_id": "hs-000017" })"));
    for (const auto& r : requests) {
        EXPECT_EQ(r.method, "POST");
        EXPECT_EQ(r.timeout_ms, 2000);   // headend_timeout_ms (Section 4.2.5)
    }
}

TEST(Tst156, StartResponses) {
    Harness h;
    // 200: the example of Section 3.3.1.
    Harness::StartOutcome ok = h.start(archive_request());
    ASSERT_TRUE(ok.called);
    EXPECT_TRUE(ok.error.ok());
    ASSERT_TRUE(ok.result.has_value());
    EXPECT_EQ(ok.result->session_id, "hs-000017");
    EXPECT_EQ(ok.result->start_time, 1794999600);
    EXPECT_EQ(ok.result->ssrc, 3054235519u);

    // 4xx with an error object: that code and its message.
    const struct {
        int status;
        const char* file;
        const char* code;
    } errors[] = {{400, "error_future_time.json", "FUTURE_TIME"},
                  {400, "error_out_of_archive.json", "OUT_OF_ARCHIVE"},
                  {400, "error_invalid_request.json", "INVALID_REQUEST"},
                  {404, "error_service_not_found.json", "SERVICE_NOT_FOUND"}};
    for (const auto& e : errors) {
        h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(e.status, e.file));
        const Harness::StartOutcome out = h.start(archive_request());
        EXPECT_EQ(out.error.code, e.code);
        EXPECT_FALSE(out.error.message.empty());
        EXPECT_FALSE(out.result.has_value());
    }
    EXPECT_EQ(h.logger.count(pal::LogLevel::Error), 1u);   // INVALID_REQUEST: programming error (ER-13)

    // Transport failure, invalid bodies, 5xx, 4xx without an error object: HEADEND_UNREACHABLE.
    const FakeHttpClient::Reply unreachable[] = {
        FakeHttpClient::Reply::transport_failure(),
        FakeHttpClient::Reply::ok(200, "not json"),
        FakeHttpClient::Reply::ok(200, R"({"service_id": "svc-3", "start_time": 1794999600, "ssrc": 1, "payload_type": 33})"),
        FakeHttpClient::Reply::ok(200, R"({"session_id": "hs-1", "service_id": "svc-3", "start_time": "now", "ssrc": 1, "payload_type": 33})"),
        FakeHttpClient::Reply::ok(500, "{}"),
        FakeHttpClient::Reply::ok(400, "{}")};
    for (const auto& reply : unreachable) {
        h.http.script("POST", kHe + "/start", reply);
        const Harness::StartOutcome out = h.start(archive_request());
        EXPECT_EQ(out.error.code, "HEADEND_UNREACHABLE") << reply.response.body;
        EXPECT_FALSE(out.result.has_value());
    }

    // No answer: HEADEND_UNREACHABLE at 2000 ms.
    h.http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json", 5000));
    Harness::StartOutcome late;
    h.hec.start(1, kHe, archive_request(), [&late](const al::HeadendError& e, const al::StartResult*) {
        late.called = true;
        late.error = e;
    });
    h.advance(1999);
    EXPECT_FALSE(late.called);
    h.advance(1);
    ASSERT_TRUE(late.called);
    EXPECT_EQ(late.error.code, "HEADEND_UNREACHABLE");
}

TEST(Tst156, KeepaliveAndStop) {
    Harness h;
    // Keepalive: 200 -> success; SESSION_NOT_FOUND (T-28); another failure -> HEADEND_UNREACHABLE (T-29).
    std::optional<al::HeadendError> ka = h.keepalive();
    ASSERT_TRUE(ka.has_value());
    EXPECT_TRUE(ka->ok());
    h.http.script("POST", kHe + "/keepalive", FakeHttpClient::Reply::file(404, "error_session_not_found.json"));
    EXPECT_EQ(h.keepalive()->code, "SESSION_NOT_FOUND");
    h.http.script("POST", kHe + "/keepalive", FakeHttpClient::Reply::transport_failure());
    EXPECT_EQ(h.keepalive()->code, "HEADEND_UNREACHABLE");
    h.http.script("POST", kHe + "/keepalive", FakeHttpClient::Reply::file(400, "error_invalid_request.json"));
    EXPECT_EQ(h.keepalive()->code, "HEADEND_UNREACHABLE");   // "no answer or another error" (REQ-69)

    // Stop: 200 -> success; SESSION_NOT_FOUND -> success (Section 3.3.2); failure -> HEADEND_UNREACHABLE.
    EXPECT_TRUE(h.stop()->ok());
    h.http.script("POST", kHe + "/stop", FakeHttpClient::Reply::file(404, "error_session_not_found.json"));
    EXPECT_TRUE(h.stop()->ok());
    h.http.script("POST", kHe + "/stop", FakeHttpClient::Reply::transport_failure());
    EXPECT_EQ(h.stop()->code, "HEADEND_UNREACHABLE");

    // A successful keepalive is logged at level Debug only.
    EXPECT_EQ(h.logger.count(pal::LogLevel::Debug), 1u);
}

TEST(Tst156, CancelAndLog) {
    Harness h;
    bool called = false;
    const auto token = h.hec.start(1, kHe, archive_request(), [&called](const al::HeadendError&, const al::StartResult*) {
        called = true;
    });
    h.hec.cancel(token);
    h.settle();
    EXPECT_FALSE(called);
    EXPECT_NE(h.calls.find("cancel "), fakes::CallLog::npos);

    h.start(archive_request());
    h.stop();
    EXPECT_TRUE(h.logger.contains("start ok session=1 headend_session=hs-000017 ssrc=3054235519 start_time=1794999600"));
    EXPECT_TRUE(h.logger.contains("stop headend_session=hs-000017"));
}

}  // namespace
