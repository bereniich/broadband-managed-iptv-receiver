// Tests of the fakes of the platform services (step 3b). Each test carries the ID of the first test
// that relies on the fake (Section 5.3.1, "Names"). The expected JSON values are the examples of
// Section 3, copied into this file from the specification.
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include <cctype>
#include <string>
#include <vector>

#include <json.hpp>

#include "fakes/call_log.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_hls_store.hpp"
#include "fakes/fake_http_client.hpp"
#include "fakes/fake_ui_channel.hpp"

namespace {

using fakes::FakeHttpClient;
using nlohmann::json;

const std::string kMw = "http://127.0.0.1:8080/api/v1";
const std::string kHe = "http://127.0.0.1:8081/api/v1/stream";

// Runs a request and returns its response after advancing the clock by `advance_ms`.
struct Result {
    bool called = false;
    int calls = 0;
    pal::HttpResponse response{false, 0, "", ""};
};

pal::IHttpClient::Callback into(Result& r) {
    return [&r](const pal::HttpResponse& response) {
        r.called = true;
        ++r.calls;
        r.response = response;
    };
}

json get_json(FakeHttpClient& http, fakes::FakeClock& clock, const std::string& url, int* status) {
    Result r;
    http.get(url, 800, into(r));
    clock.advance(0);
    EXPECT_TRUE(r.called) << url;
    EXPECT_TRUE(r.response.transport_ok) << url;
    *status = r.response.status;
    return json::parse(r.response.body, nullptr, false);
}

json post_json(FakeHttpClient& http, fakes::FakeClock& clock, const std::string& url, const std::string& body,
               int* status) {
    Result r;
    http.post_json(url, body, 2000, into(r));
    clock.advance(0);
    EXPECT_TRUE(r.called) << url;
    *status = r.response.status;
    return json::parse(r.response.body, nullptr, false);
}

json service_info(const std::string& id, int number, const std::string& name, bool is_protected) {
    return json{{"service_id", id},
                {"number", number},
                {"name", name},
                {"protected", is_protected},
                {"description", is_protected ? "Protected demo service" : "Clear demo service"},
                {"stream_control_url", "http://127.0.0.1:8081/api/v1/stream"},
                {"archive_window_s", 7200}};
}

// ---------------------------------------------------------------- fake HTTP client

TEST(Tst151, FakeHttpMwResponses) {
    fakes::FakeClock clock;
    FakeHttpClient http(clock);
    int status = 0;

    // Section 3.2.1.
    const json services = json::parse(R"({
      "services": [
        { "service_id": "svc-1", "number": 1, "name": "Demo One",   "protected": false },
        { "service_id": "svc-2", "number": 2, "name": "Demo Two",   "protected": false },
        { "service_id": "svc-3", "number": 3, "name": "Premium One", "protected": true },
        { "service_id": "svc-4", "number": 4, "name": "Premium Two", "protected": true }
      ]
    })");
    EXPECT_EQ(get_json(http, clock, kMw + "/services", &status), services);
    EXPECT_EQ(status, 200);

    // Section 3.2.2 (svc-3, verbatim) and Section 5.2.3 (the other services, same fields).
    const json svc3 = json::parse(R"({
      "service_id": "svc-3",
      "number": 3,
      "name": "Premium One",
      "protected": true,
      "description": "Protected demo service",
      "stream_control_url": "http://127.0.0.1:8081/api/v1/stream",
      "archive_window_s": 7200
    })");
    EXPECT_EQ(get_json(http, clock, kMw + "/services/svc-3", &status), svc3);
    EXPECT_EQ(svc3, service_info("svc-3", 3, "Premium One", true));
    EXPECT_EQ(get_json(http, clock, kMw + "/services/svc-1", &status), service_info("svc-1", 1, "Demo One", false));
    EXPECT_EQ(get_json(http, clock, kMw + "/services/svc-2", &status), service_info("svc-2", 2, "Demo Two", false));
    EXPECT_EQ(get_json(http, clock, kMw + "/services/svc-4", &status), service_info("svc-4", 4, "Premium Two", true));
    EXPECT_EQ(status, 200);

    // Unknown service: 404 SERVICE_NOT_FOUND with the error object of Section 3.2.
    const json not_found = get_json(http, clock, kMw + "/services/svc-9", &status);
    EXPECT_EQ(status, 404);
    EXPECT_EQ(not_found["error"]["code"], "SERVICE_NOT_FOUND");
    EXPECT_TRUE(not_found["error"]["message"].is_string());

    // Section 3.2.3 (the query is not interpreted by the default route).
    const json epg = json::parse(R"({
      "server_time": 1795000000,
      "from": 1794993000,
      "to": 1795003000,
      "services": [
        {
          "service_id": "svc-3",
          "events": [
            {
              "event_id": "svc-3-1794999600",
              "start": 1794999600,
              "duration": 300,
              "title": "Premium Morning",
              "description": "First program of the protected demo service.",
              "in_archive": true
            }
          ]
        }
      ]
    })");
    EXPECT_EQ(get_json(http, clock, kMw + "/epg?from=1794993000&to=1795003000", &status), epg);
    EXPECT_EQ(status, 200);
}

TEST(Tst156, FakeHttpHeadendResponses) {
    fakes::FakeClock clock;
    fakes::CallLog log;
    FakeHttpClient http(clock, &log);
    int status = 0;

    // Section 3.3.1.
    const json start = json::parse(R"({
      "session_id": "hs-000017",
      "service_id": "svc-3",
      "start_time": 1794999600,
      "ssrc": 3054235519,
      "payload_type": 33
    })");
    const std::string body =
        R"({ "service_id": "svc-3", "start": 1794999600, "client_host": "127.0.0.1", "client_port": 5004, "ssrc": 3054235519 })";
    EXPECT_EQ(post_json(http, clock, kHe + "/start", body, &status), start);
    EXPECT_EQ(status, 200);
    ASSERT_NE(http.last("POST", kHe + "/start"), nullptr);
    EXPECT_EQ(http.last("POST", kHe + "/start")->body, body);
    EXPECT_EQ(http.last("POST", kHe + "/start")->timeout_ms, 2000);

    // Sections 3.3.3 and 3.3.2.
    const std::string session = R"({ "session_id": "hs-000017" })";
    EXPECT_EQ(post_json(http, clock, kHe + "/keepalive", session, &status), json::parse(R"({ "expires_in_s": 15 })"));
    EXPECT_EQ(post_json(http, clock, kHe + "/stop", session, &status), json::parse(R"({ "stopped": true })"));
    EXPECT_EQ(log.entries(), (std::vector<std::string>{"POST " + kHe + "/start", "POST " + kHe + "/keepalive",
                                                       "POST " + kHe + "/stop"}));

    // Error files: {"error": {"code": <code>, "message": <text>}} (Sections 3.2, 3.3, 3.10).
    for (const std::string code : {"SERVICE_NOT_FOUND", "INVALID_RANGE", "INVALID_REQUEST", "OUT_OF_ARCHIVE",
                                   "FUTURE_TIME", "SESSION_NOT_FOUND"}) {
        std::string file = "error_" + code + ".json";
        for (char& c : file) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const json error = json::parse(FakeHttpClient::read_response_file(file), nullptr, false);
        ASSERT_TRUE(error.is_object()) << file;
        EXPECT_EQ(error.size(), 1u) << file;
        EXPECT_EQ(error["error"]["code"], code) << file;
        EXPECT_TRUE(error["error"]["message"].is_string()) << file;
        EXPECT_EQ(error["error"].size(), 2u) << file;
    }
}

TEST(Tst154, FakeHttpTimingAndTimeout) {
    fakes::FakeClock clock;
    fakes::CallLog log;
    FakeHttpClient http(clock, &log);

    // Delay below the timeout: delivered exactly at the delay, never inside get().
    http.script("GET", kMw + "/services", FakeHttpClient::Reply::file(200, "mw_services.json", 300));
    Result delayed;
    http.get(kMw + "/services", 800, into(delayed));
    EXPECT_FALSE(delayed.called);
    clock.advance(299);
    EXPECT_FALSE(delayed.called);
    clock.advance(1);
    ASSERT_TRUE(delayed.called);
    EXPECT_TRUE(delayed.response.transport_ok);
    EXPECT_EQ(delayed.response.status, 200);

    // Delay 900 ms with a timeout of 800 ms (TST-154): transport failure at 800 ms, nothing later.
    http.script("GET", kMw + "/epg?from=1&to=2", FakeHttpClient::Reply::file(200, "mw_epg.json", 900));
    Result late;
    http.get(kMw + "/epg?from=1&to=2", 800, into(late));
    clock.advance(799);
    EXPECT_FALSE(late.called);
    clock.advance(1);
    ASSERT_TRUE(late.called);
    EXPECT_FALSE(late.response.transport_ok);
    EXPECT_EQ(late.response.status, 0);
    clock.advance(1000);
    EXPECT_EQ(late.calls, 1);   // exactly once

    // Held: no timeout, delivered by release().
    http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json").held());
    Result held;
    const auto held_id = http.post_json(kHe + "/start", "{}", 2000, into(held));
    clock.advance(10000);
    EXPECT_FALSE(held.called);
    EXPECT_TRUE(http.release(held_id));
    ASSERT_TRUE(held.called);
    EXPECT_EQ(held.response.status, 200);
    EXPECT_FALSE(http.release(held_id));   // already delivered

    // respond() replaces the reply of a held request.
    http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json").held());
    Result replaced;
    const auto replaced_id = http.post_json(kHe + "/start", "{}", 2000, into(replaced));
    EXPECT_TRUE(http.respond(replaced_id, FakeHttpClient::Reply::file(400, "error_future_time.json")));
    ASSERT_TRUE(replaced.called);
    EXPECT_EQ(replaced.response.status, 400);

    // cancel(): no callback afterwards (Section 3.9.1, rule 4), also for a held request.
    Result cancelled;
    const auto cancelled_id = http.get(kMw + "/services", 800, into(cancelled));
    http.cancel(cancelled_id);
    clock.advance(1000);
    EXPECT_FALSE(cancelled.called);
    http.script("POST", kHe + "/start", FakeHttpClient::Reply::file(200, "he_start.json").held());
    Result cancelled_held;
    const auto cancelled_held_id = http.post_json(kHe + "/start", "{}", 2000, into(cancelled_held));
    http.cancel(cancelled_held_id);
    EXPECT_FALSE(http.release(cancelled_held_id));
    EXPECT_FALSE(cancelled_held.called);

    // Scripted replies are used in order, then the default route.
    http.script("GET", kMw + "/services", FakeHttpClient::Reply::transport_failure());
    http.script("GET", kMw + "/services", FakeHttpClient::Reply::ok(500, "{}"));
    Result first, second, third;
    http.get(kMw + "/services", 800, into(first));
    http.get(kMw + "/services", 800, into(second));
    http.get(kMw + "/services", 800, into(third));
    clock.advance(0);
    EXPECT_FALSE(first.response.transport_ok);
    EXPECT_EQ(second.response.status, 500);
    EXPECT_EQ(third.response.status, 200);
    EXPECT_EQ(http.count("GET", kMw + "/services"), 5u);

    // A request without a script and without a default route is a test failure.
    Result unknown;
    EXPECT_NONFATAL_FAILURE(http.get(kMw + "/xyz", 800, into(unknown)), "no script and no default route");
    clock.advance(0);
    EXPECT_TRUE(unknown.called);
    EXPECT_FALSE(unknown.response.transport_ok);

    EXPECT_NE(log.find("cancel " + std::to_string(cancelled_id)), fakes::CallLog::npos);
}

// ---------------------------------------------------------------- fake UI channel

TEST(Tst157, FakeUiChannel) {
    fakes::FakeUiChannel ui;
    std::vector<std::string> seen;
    ui.set_handlers(
        [&](pal::IUiChannel::ClientId c, const std::string& text) { seen.push_back(std::to_string(c) + ":" + text); },
        [&](pal::IUiChannel::ClientId c, bool connected) {
            seen.push_back(std::to_string(c) + (connected ? " connected" : " disconnected"));
        });

    ui.connect(1);
    ui.receive(1, R"({"type": "command", "id": 7, "name": "get_status", "params": {}})");
    ui.send(1, R"({"type": "response", "id": 7, "ok": true, "result": {"state": "idle"}})");
    ui.broadcast(R"({"type": "event", "name": "mw_status", "data": {"reachable": false}})");
    ui.send(2, R"({"type": "event", "name": "mw_status", "data": {"reachable": true}})");
    ui.send(1, "not json");
    ui.disconnect(1);

    EXPECT_EQ(seen, (std::vector<std::string>{
                        "1 connected", R"(1:{"type": "command", "id": 7, "name": "get_status", "params": {}})",
                        "1 disconnected"}));
    const auto& frames = ui.frames();
    ASSERT_EQ(frames.size(), 4u);
    EXPECT_FALSE(frames[0].broadcast);
    EXPECT_EQ(frames[0].client, 1u);
    EXPECT_TRUE(frames[0].delivered);
    EXPECT_TRUE(frames[1].broadcast);
    EXPECT_FALSE(frames[2].delivered);   // client 2 is not connected

    ASSERT_EQ(ui.responses(7).size(), 1u);
    EXPECT_EQ(ui.responses(7)[0]["result"]["state"], "idle");
    ASSERT_EQ(ui.events("mw_status").size(), 2u);
    EXPECT_EQ(ui.events("mw_status")[0]["reachable"], false);
    EXPECT_TRUE(ui.json_frames()[3].is_discarded());
}

// ---------------------------------------------------------------- fake HLS store

TEST(Tst123, FakeHlsStore) {
    fakes::CallLog log;
    std::filesystem::path root;
    {
        fakes::FakeHlsStore hls(&log);
        root = hls.root();
        ASSERT_TRUE(std::filesystem::is_directory(root));

        EXPECT_TRUE(hls.open_session(1));
        EXPECT_TRUE(hls.session_exists(1));
        EXPECT_TRUE(hls.files(1).empty());

        const std::vector<std::uint8_t> data(1000, 0x47);
        EXPECT_TRUE(hls.write_file(1, "seg-0.ts", data.data(), data.size()));
        EXPECT_EQ(hls.read(1, "seg-0.ts"), data);
        EXPECT_EQ(hls.files(1), (std::vector<std::string>{"seg-0.ts"}));   // no temporary file left

        const std::string playlist = "#EXTM3U\n";
        EXPECT_TRUE(hls.write_file(1, "index.m3u8", reinterpret_cast<const std::uint8_t*>(playlist.data()),
                                   playlist.size()));
        const std::vector<std::uint8_t> other(10, 0x01);
        EXPECT_TRUE(hls.write_file(1, "seg-0.ts", other.data(), other.size()));   // replaces the file
        EXPECT_EQ(hls.read(1, "seg-0.ts"), other);

        EXPECT_TRUE(hls.write_file(1, "seg-1.ts", data.data(), data.size()));
        EXPECT_EQ(hls.segment_files(1), 2u);
        EXPECT_TRUE(hls.remove_file(1, "seg-0.ts"));
        EXPECT_TRUE(hls.remove_file(1, "seg-0.ts"));   // missing file: not an error
        EXPECT_EQ(hls.segment_files(1), 1u);
        EXPECT_EQ(hls.max_segment_files(1), 2u);

        EXPECT_FALSE(hls.write_file(1, "../x.ts", data.data(), data.size()));
        EXPECT_FALSE(hls.write_file(1, "a/b.ts", data.data(), data.size()));
        EXPECT_FALSE(hls.write_file(2, "seg-0.ts", data.data(), data.size()));   // session not open

        hls.close_session(1);
        EXPECT_FALSE(hls.session_exists(1));
        EXPECT_TRUE(hls.open_session(1));   // a restart after set_key creates it again, empty
        EXPECT_TRUE(hls.files(1).empty());

        hls.fail_open(true);
        EXPECT_FALSE(hls.open_session(3));
    }
    EXPECT_FALSE(std::filesystem::exists(root));   // removed with the store
    EXPECT_EQ(log.find("open_session 1"), 0u);
    EXPECT_NE(log.find("write_file 1 seg-0.ts 1000"), fakes::CallLog::npos);
    EXPECT_NE(log.find("close_session 1"), fakes::CallLog::npos);
}

TEST(Tst127, FakeHlsStoreWriteFailure) {
    fakes::FakeHlsStore hls;
    hls.fail_writes_from(3);
    ASSERT_TRUE(hls.open_session(1));
    const std::uint8_t byte = 0x47;
    EXPECT_TRUE(hls.write_file(1, "seg-0.ts", &byte, 1));
    EXPECT_TRUE(hls.write_file(1, "index.m3u8", &byte, 1));
    EXPECT_FALSE(hls.write_file(1, "seg-1.ts", &byte, 1));
    EXPECT_FALSE(hls.write_file(1, "index.m3u8", &byte, 1));
    EXPECT_EQ(hls.write_calls(), 4u);
}

}  // namespace
