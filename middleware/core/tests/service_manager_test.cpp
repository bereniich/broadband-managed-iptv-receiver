// AL-SVC module tests (step 7a): TST-151 – TST-154 and the module part of TST-155. The responses are the
// JSON examples of Section 3.2 (fake HTTP client, step 3b) and the invalid inputs IN-05 / IN-06 of
// Section 5.2.5.
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include <json.hpp>

#include "fakes/call_log.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_http_client.hpp"
#include "fakes/fake_logger.hpp"
#include "service_manager.hpp"
#include "task_queue.hpp"

namespace {

using fakes::FakeHttpClient;
using nlohmann::json;

const std::string kMw = "http://127.0.0.1:8080/api/v1";

struct Harness {
    fakes::FakeClock clock;
    fakes::CallLog calls;
    FakeHttpClient http{clock, &calls};
    fakes::FakeLogger logger;
    al::TaskQueue queue{clock, logger};
    al::ServiceManager svc{http, clock, logger, queue, al::ServiceManager::Config{}};
    std::vector<bool> statuses;

    Harness() {
        svc.set_status_handler([this](bool reachable) { statuses.push_back(reachable); });
    }

    // Delivers due HTTP replies and timers and runs the worker until nothing is left.
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

    void load_ok() {
        svc.load_services();
        settle();
        ASSERT_TRUE(svc.loaded());
    }
};

// ---------------------------------------------------------------- TST-151

TEST(Tst151, RetryUntilLoaded) {
    Harness h;
    for (int i = 0; i < 3; ++i) h.http.script("GET", kMw + "/services", FakeHttpClient::Reply::transport_failure());
    h.svc.load_services();
    h.settle();
    EXPECT_EQ(h.http.count("GET", kMw + "/services"), 1u);   // at 0 s
    EXPECT_FALSE(h.svc.loaded());
    EXPECT_EQ(h.statuses, std::vector<bool>{false});          // after the first failure

    for (std::size_t expected = 2; expected <= 4; ++expected) {
        h.advance(4999);
        EXPECT_EQ(h.http.count("GET", kMw + "/services"), expected - 1) << "before " << 5 * (expected - 1) << " s";
        h.advance(1);
        EXPECT_EQ(h.http.count("GET", kMw + "/services"), expected) << "at " << 5 * (expected - 1) << " s";
    }
    // The 4th request (15 s) returns the example of Section 3.2.1.
    ASSERT_TRUE(h.svc.loaded());
    EXPECT_EQ(h.statuses, (std::vector<bool>{false, true}));   // false once, true once
    const auto& list = h.svc.services();
    ASSERT_EQ(list.size(), 4u);
    const char* ids[] = {"svc-1", "svc-2", "svc-3", "svc-4"};
    const char* names[] = {"Demo One", "Demo Two", "Premium One", "Premium Two"};
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(list[i].service_id, ids[i]);
        EXPECT_EQ(list[i].number, static_cast<int>(i + 1));
        EXPECT_EQ(list[i].name, names[i]);
        EXPECT_EQ(list[i].is_protected, i >= 2);
    }
    h.advance(60000);   // no further request after the success
    EXPECT_EQ(h.http.count("GET", kMw + "/services"), 4u);
    EXPECT_EQ(h.http.last("GET", kMw + "/services")->timeout_ms, 800);
}

// ---------------------------------------------------------------- TST-152

TEST(Tst152, InvalidEntries) {
    Harness h;
    // IN-05: empty service_id; number 0, 1000 and a duplicate number; name missing; protected "yes";
    // plus one valid entry (first, so that the duplicate is the later entry).
    const std::string body = R"({"services": [
        {"service_id": "svc-1", "number": 1, "name": "Demo One", "protected": false},
        {"service_id": "", "number": 2, "name": "Empty Id", "protected": false},
        {"service_id": "svc-2", "number": 0, "name": "Zero", "protected": false},
        {"service_id": "svc-3", "number": 1000, "name": "Thousand", "protected": true},
        {"service_id": "svc-4", "number": 1, "name": "Duplicate", "protected": true},
        {"service_id": "svc-5", "number": 5, "protected": false},
        {"service_id": "svc-6", "number": 6, "name": "Yes", "protected": "yes"}
    ]})";
    h.http.script("GET", kMw + "/services", FakeHttpClient::Reply::ok(200, body));
    h.load_ok();
    ASSERT_EQ(h.svc.services().size(), 1u);
    EXPECT_EQ(h.svc.services()[0].service_id, "svc-1");
    EXPECT_EQ(h.logger.count_containing("discarded service entry"), 6u);   // one line per discarded entry
    EXPECT_TRUE(h.logger.contains("discarded service entry 1: service_id missing or empty"));
    EXPECT_TRUE(h.logger.contains("discarded service entry 4: duplicate number"));
    EXPECT_TRUE(h.logger.contains("discarded service entry 6: protected missing or not a boolean"));
    EXPECT_EQ(h.svc.find("svc-4"), nullptr);
}

TEST(Tst152, InvalidWholeResponse) {
    Harness h;   // ER-04: not JSON, no services array, HTTP error -> failure and retry
    h.http.script("GET", kMw + "/services", FakeHttpClient::Reply::ok(200, "not json"));
    h.http.script("GET", kMw + "/services", FakeHttpClient::Reply::ok(200, R"({"list": []})"));
    h.http.script("GET", kMw + "/services", FakeHttpClient::Reply::file(404, "error_service_not_found.json"));
    h.svc.load_services();
    h.settle();
    h.advance(5000);
    h.advance(5000);
    EXPECT_FALSE(h.svc.loaded());
    EXPECT_EQ(h.statuses, std::vector<bool>{false});
    h.advance(5000);   // default route: the example of Section 3.2.1
    EXPECT_TRUE(h.svc.loaded());
    EXPECT_EQ(h.statuses, (std::vector<bool>{false, true}));
}

// ---------------------------------------------------------------- TST-153

TEST(Tst153, ListKeptWhenUnreachable) {
    Harness h;
    h.load_ok();
    const std::vector<al::ServiceEntry> before = h.svc.services();
    h.http.script("GET", kMw + "/epg?from=1795000000&to=1795003600", FakeHttpClient::Reply::transport_failure());
    std::optional<al::MwError> result;
    h.svc.get_epg(1795000000, 1795003600, [&](al::MwError e, const json*) { result = e; });
    h.settle();
    EXPECT_EQ(result, std::optional<al::MwError>(al::MwError::MwUnreachable));
    EXPECT_STREQ(al::to_code(*result), "MW_UNREACHABLE");
    EXPECT_EQ(h.statuses, (std::vector<bool>{true, false}));   // ER-02: mw_status false
    ASSERT_EQ(h.svc.services().size(), before.size());         // REQ-05: the last valid list
    EXPECT_EQ(h.svc.services()[3].service_id, before[3].service_id);
    EXPECT_TRUE(h.svc.loaded());
}

// ---------------------------------------------------------------- TST-154

TEST(Tst154, EpgValidation) {
    Harness h;
    h.load_ok();
    const std::string url = kMw + "/epg?from=1794993000&to=1795003000";

    // IN-06: an event without title, two overlapping events and unsorted events.
    const std::string in06 = R"({"server_time": 1795000000, "services": [{"service_id": "svc-3", "events": [
        {"event_id": "svc-3-1794999000", "start": 1794999000, "duration": 300, "title": "A", "description": "a", "in_archive": true},
        {"event_id": "svc-3-1794999300", "start": 1794999300, "duration": 300, "description": "no title", "in_archive": true},
        {"event_id": "svc-3-1794999600", "start": 1794999600, "duration": 300, "title": "C", "description": "c", "in_archive": true},
        {"event_id": "svc-3-1794999700", "start": 1794999700, "duration": 300, "title": "D", "description": "overlaps C", "in_archive": true},
        {"event_id": "svc-3-1794998700", "start": 1794998700, "duration": 300, "title": "E", "description": "unsorted", "in_archive": true},
        {"event_id": "svc-3-1794999900", "start": 1794999900, "duration": 300, "title": "F", "description": "f", "in_archive": false}
    ]}]})";
    h.http.script("GET", url, FakeHttpClient::Reply::ok(200, in06));
    std::optional<json> epg;
    h.svc.get_epg(1794993000, 1795003000, [&](al::MwError e, const json* r) {
        EXPECT_EQ(e, al::MwError::None);
        if (r) epg = *r;
    });
    h.settle();
    ASSERT_TRUE(epg.has_value());
    EXPECT_EQ((*epg)["server_time"], 1795000000);
    const json& events = (*epg)["services"][0]["events"];
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0]["event_id"], "svc-3-1794999000");
    EXPECT_EQ(events[1]["event_id"], "svc-3-1794999600");
    EXPECT_EQ(events[2]["event_id"], "svc-3-1794999900");
    EXPECT_EQ(h.logger.count_containing("discarded EPG event"), 3u);
    EXPECT_TRUE(h.logger.contains("discarded EPG event svc-3-1794999300: title missing"));

    // A response that is not JSON -> MW_UNREACHABLE (ER-04).
    h.http.script("GET", url, FakeHttpClient::Reply::ok(200, "<html>"));
    std::optional<al::MwError> not_json;
    h.svc.get_epg(1794993000, 1795003000, [&](al::MwError e, const json*) { not_json = e; });
    h.settle();
    EXPECT_EQ(not_json, std::optional<al::MwError>(al::MwError::MwUnreachable));

    // A response delayed by 900 ms -> MW_UNREACHABLE at 800 ms (timeout of Section 4.2.4).
    h.http.script("GET", url, FakeHttpClient::Reply::file(200, "mw_epg.json", 900));
    std::optional<al::MwError> late;
    h.svc.get_epg(1794993000, 1795003000, [&](al::MwError e, const json*) { late = e; });
    h.advance(799);
    EXPECT_FALSE(late.has_value());
    h.advance(1);
    EXPECT_EQ(late, std::optional<al::MwError>(al::MwError::MwUnreachable));

    // INVALID_RANGE from the server is passed on.
    h.http.script("GET", url, FakeHttpClient::Reply::file(400, "error_invalid_range.json"));
    std::optional<al::MwError> range;
    h.svc.get_epg(1794993000, 1795003000, [&](al::MwError e, const json*) { range = e; });
    h.settle();
    EXPECT_EQ(range, std::optional<al::MwError>(al::MwError::InvalidRange));

    // Two valid requests: two GET /epg (no cache); the result is the example of Section 3.2.3.
    const std::size_t before = h.http.count("GET", kMw + "/epg");
    std::vector<json> results;
    for (int i = 0; i < 2; ++i) {
        h.svc.get_epg(1794993000, 1795003000, [&](al::MwError, const json* r) { results.push_back(*r); });
        h.settle();
    }
    EXPECT_EQ(h.http.count("GET", kMw + "/epg"), before + 2);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0]["server_time"], 1795000000);
    EXPECT_EQ(results[0]["services"][0]["events"][0]["title"], "Premium Morning");
    EXPECT_EQ(h.statuses.back(), true);   // reachable again after the valid responses
}

// ---------------------------------------------------------------- TST-155 (module part)

TEST(Tst155, ServiceInfoCached) {
    Harness h;
    h.load_ok();
    std::vector<std::string> urls;
    for (int i = 0; i < 2; ++i) {
        h.svc.get_service_info("svc-1", [&](al::MwError e, const al::ServiceInfo* info) {
            EXPECT_EQ(e, al::MwError::None);
            ASSERT_NE(info, nullptr);
            urls.push_back(info->stream_control_url);
            EXPECT_EQ(info->archive_window_s, 7200);
        });
        h.settle();
    }
    EXPECT_EQ(h.http.count("GET", kMw + "/services/svc-1"), 1u);   // REQ-12: cached after the first success
    EXPECT_EQ(urls, (std::vector<std::string>{"http://127.0.0.1:8081/api/v1/stream",
                                              "http://127.0.0.1:8081/api/v1/stream"}));

    std::optional<al::MwError> unknown;   // 404 SERVICE_NOT_FOUND (T-05): the server is reachable
    h.svc.get_service_info("svc-9", [&](al::MwError e, const al::ServiceInfo*) { unknown = e; });
    h.settle();
    EXPECT_EQ(unknown, std::optional<al::MwError>(al::MwError::ServiceNotFound));
    EXPECT_EQ(h.statuses, std::vector<bool>{true});

    h.http.script("GET", kMw + "/services/svc-2", FakeHttpClient::Reply::transport_failure());
    std::optional<al::MwError> failed;
    h.svc.get_service_info("svc-2", [&](al::MwError e, const al::ServiceInfo*) { failed = e; });
    h.settle();
    EXPECT_EQ(failed, std::optional<al::MwError>(al::MwError::MwUnreachable));

    // cancel(): no callback, neither for a request nor for a cache hit.
    bool called = false;
    const auto request = h.svc.get_service_info("svc-3", [&](al::MwError, const al::ServiceInfo*) { called = true; });
    const auto cached = h.svc.get_service_info("svc-1", [&](al::MwError, const al::ServiceInfo*) { called = true; });
    h.svc.cancel(request);
    h.svc.cancel(cached);
    h.settle();
    EXPECT_FALSE(called);
    EXPECT_NE(h.calls.find("cancel "), fakes::CallLog::npos);
}

}  // namespace
