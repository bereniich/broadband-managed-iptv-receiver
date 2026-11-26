// Unit tests of SRV-MW and of the operator-side configuration (Section 5.3.4):
//   TST-318 (OTT MW server part; the head-end part follows with the head-end),
//   TST-319, TST-320 (OTT MW server part and the common configuration readers).
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <json.hpp>

#include "config.hpp"
#include "http.hpp"
#include "mw_api.hpp"
#include "services.hpp"
#include "test_support.hpp"

namespace {

using nlohmann::json;

const std::string kRepo = M15_REPO_DIR;
constexpr int64_t kT = 1795000000;  // worked example of Section 5.2.4

// Section 5.2.3.
struct ExpectedService {
    const char* id;
    int number;
    const char* name;
    bool is_protected;
    const char* description;
    const char* program1;
    const char* program2;
};
const ExpectedService kExpected[] = {
    {"svc-1", 1, "Demo One", false, "Clear demo service", "News at Nine", "Weather Today"},
    {"svc-2", 2, "Demo Two", false, "Clear demo service", "Science Hour", "Nature Walk"},
    {"svc-3", 3, "Premium One", true, "Protected demo service", "Premium Morning", "Premium Evening"},
    {"svc-4", 4, "Premium Two", true, "Protected demo service", "Cinema Classic", "Late Movie"},
};
const char* kStreamControlUrl = "http://127.0.0.1:8081/api/v1/stream";

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "First program of the <description in lower case>." (Section 5.2.3).
std::string program_description(const ExpectedService& e, int program) {
    return std::string(program == 1 ? "First" : "Second") + " program of the " + lower(e.description) + ".";
}

// Checks the error object of Sections 3.1 / 3.2 and, if given, its code.
void expect_error(const test::HttpReply& reply, int status, const std::string& code = "") {
    ASSERT_TRUE(reply.answered) << "no HTTP response";
    EXPECT_EQ(reply.status, status) << reply.body;
    EXPECT_NE(reply.headers.find("content-type: application/json"), std::string::npos);
    const json body = json::parse(reply.body, nullptr, false);
    ASSERT_TRUE(body.is_object()) << reply.body;
    ASSERT_TRUE(body.contains("error") && body["error"].is_object()) << reply.body;
    ASSERT_TRUE(body["error"].contains("code") && body["error"]["code"].is_string()) << reply.body;
    ASSERT_TRUE(body["error"].contains("message") && body["error"]["message"].is_string()) << reply.body;
    if (!code.empty()) {
        EXPECT_EQ(body["error"]["code"], code) << reply.body;
    }
}

// OTT MW server in-process: the repository's server/config.json and content/services.json,
// a fixed clock (`now_`), Mongoose on an ephemeral port of 127.0.0.1. Usable as a fixture (TST-319)
// and as a plain object (TST-318, whose suite also has the head-end test without a fixture).
class MwServerUnderTest {
public:
    MwServerUnderTest() {
        const srv::ServerConfig config = srv::load_server_config(kRepo + "/server/config.json");
        api_ = std::make_unique<srv::MwApi>(srv::load_services(kRepo + "/" + config.services_file),
                                            config.headend_url,
                                            [this] { return srv::from_unix_seconds(now_.load()); });
        server_ = std::make_unique<srv::HttpServer>([this](const srv::HttpRequest& r) { return api_->handle(r); });
        std::string error;
        if (!server_->start(0, &error)) throw std::runtime_error(error);
    }

    ~MwServerUnderTest() { server_->stop(); }

    json get_ok(const std::string& target) {
        const test::HttpReply reply = test::http_get(server_->port(), target);
        EXPECT_TRUE(reply.answered) << target;
        EXPECT_EQ(reply.status, 200) << target << " " << reply.body;
        EXPECT_NE(reply.headers.find("content-type: application/json"), std::string::npos) << target;
        return json::parse(reply.body, nullptr, false);
    }

    int port() const { return server_->port(); }

    std::atomic<int64_t> now_{kT};
    std::unique_ptr<srv::MwApi> api_;
    std::unique_ptr<srv::HttpServer> server_;
};

class MwServerFixture : public ::testing::Test, public MwServerUnderTest {};

using Tst319 = MwServerFixture;

std::string epg_target(int64_t from, int64_t to, const std::string& extra = "") {
    return "/api/v1/epg?from=" + std::to_string(from) + "&to=" + std::to_string(to) + extra;
}

// Checks the events of one service in GET /epg?from=t-7200&to=t+7200 with the rules of
// Section 5.2.4, computed here independently of SRV-MW.
void check_epg_window(const json& entry, const ExpectedService& e, int64_t t) {
    ASSERT_EQ(entry["service_id"], e.id);
    const json& events = entry["events"];
    ASSERT_TRUE(events.is_array());
    const int64_t from = t - 7200;
    const int64_t to = t + 7200;
    const int64_t first = from - ((from % 300) + 300) % 300;  // event overlapping `from`
    const size_t expected_count = (t % 300 == 0) ? 48 : 49;
    ASSERT_EQ(events.size(), expected_count) << e.id;
    int in_archive = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        const json& ev = events[i];
        const int64_t start = first + 300 * static_cast<int64_t>(i);  // sorted, no overlap, no gap
        ASSERT_EQ(ev["start"], start) << e.id << " event " << i;
        EXPECT_EQ(start % 300, 0);
        EXPECT_EQ(ev["event_id"], std::string(e.id) + "-" + std::to_string(start));
        EXPECT_EQ(ev["duration"], 300);
        const bool program1 = ((start % 600) + 600) % 600 < 300;
        EXPECT_EQ(ev["title"], program1 ? e.program1 : e.program2) << e.id << " " << start;
        EXPECT_EQ(ev["description"], program_description(e, program1 ? 1 : 2));
        const bool expected_archive = t - 7200 <= start && start <= t;
        EXPECT_EQ(ev["in_archive"], expected_archive) << e.id << " " << start;
        in_archive += expected_archive ? 1 : 0;
        EXPECT_LT(start, to);
        EXPECT_GT(start + 300, from);
    }
    // Starts t - 7200 ... t in steps of 300 with both bounds inclusive: 25 if t is a multiple of 300.
    EXPECT_EQ(in_archive, (t % 300 == 0) ? 25 : 24);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// TST-319 — IF-EXT-03 with a fixed clock at t = 1795000000 (REQ-64, ER-05)
// ---------------------------------------------------------------------------------------------

TEST_F(Tst319, ServicesMatchSection523) {
    const json list = get_ok("/api/v1/services");
    ASSERT_TRUE(list.is_object() && list["services"].is_array());
    ASSERT_EQ(list["services"].size(), 4u);
    for (size_t i = 0; i < 4; ++i) {
        const json& s = list["services"][i];
        const ExpectedService& e = kExpected[i];  // sorted by number
        EXPECT_EQ(s.size(), 4u) << s.dump();
        EXPECT_EQ(s["service_id"], e.id);
        EXPECT_EQ(s["number"], e.number);
        EXPECT_EQ(s["name"], e.name);
        EXPECT_EQ(s["protected"], e.is_protected);
    }
    for (const ExpectedService& e : kExpected) {
        const json s = get_ok(std::string("/api/v1/services/") + e.id);
        ASSERT_TRUE(s.is_object()) << e.id;
        EXPECT_EQ(s["service_id"], e.id);
        EXPECT_EQ(s["number"], e.number);
        EXPECT_EQ(s["name"], e.name);
        EXPECT_EQ(s["protected"], e.is_protected);
        EXPECT_EQ(s["description"], e.description);
        EXPECT_EQ(s["stream_control_url"], kStreamControlUrl);
        EXPECT_EQ(s["archive_window_s"], 7200);
    }
    expect_error(test::http_get(port(), "/api/v1/services/svc-9"), 404, "SERVICE_NOT_FOUND");
}

TEST_F(Tst319, EpgWorkedExampleOfSection524ForAllServices) {
    const int64_t t = kT;
    const json body = get_ok(epg_target(t - 7200, t + 7200));
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body["server_time"], t);
    EXPECT_EQ(body["from"], t - 7200);
    EXPECT_EQ(body["to"], t + 7200);
    ASSERT_EQ(body["services"].size(), 4u);
    for (size_t i = 0; i < 4; ++i) check_epg_window(body["services"][i], kExpected[i], t);

    // The explicit values of the worked example (svc-3).
    const json& events = body["services"][2]["events"];
    auto find = [&events](int64_t start) {
        for (const json& ev : events) {
            if (ev["start"] == start) return ev;
        }
        return json();
    };
    const json running = find(1794999900);  // t - (t mod 300)
    ASSERT_TRUE(running.is_object());
    EXPECT_EQ(running["event_id"], "svc-3-1794999900");
    EXPECT_EQ(running["duration"], 300);
    EXPECT_EQ(running["title"], "Premium Evening");  // t mod 600 = 400 >= 300: Program 2
    EXPECT_EQ(running["in_archive"], true);
    const json previous = find(1794999600);
    EXPECT_EQ(previous["event_id"], "svc-3-1794999600");
    EXPECT_EQ(previous["title"], "Premium Morning");
    EXPECT_EQ(previous["description"], "First program of the protected demo service.");
    EXPECT_EQ(find(1794993000)["in_archive"], true);   // oldest available event
    EXPECT_EQ(find(1794992700)["in_archive"], false);  // overlaps the range, outside the archive
    EXPECT_EQ(events.front()["start"], 1794992700);
    EXPECT_EQ(events.back()["start"], 1795007100);

    // One service only.
    const json one = get_ok(epg_target(t - 7200, t + 7200, "&service_id=svc-3"));
    ASSERT_EQ(one["services"].size(), 1u);
    check_epg_window(one["services"][0], kExpected[2], t);
}

TEST_F(Tst319, EpgAtMultipleOf300Has48EventsAndInclusiveArchiveBounds) {
    const int64_t t = 1794999900;  // a multiple of 300
    now_ = t;
    const json body = get_ok(epg_target(t - 7200, t + 7200));
    EXPECT_EQ(body["server_time"], t);
    ASSERT_EQ(body["services"].size(), 4u);
    for (size_t i = 0; i < 4; ++i) check_epg_window(body["services"][i], kExpected[i], t);
    const json& events = body["services"][0]["events"];
    EXPECT_EQ(events.front()["start"], t - 7200);  // start = server_time - 7200: in the archive
    EXPECT_EQ(events.front()["in_archive"], true);
    EXPECT_EQ(events[24]["start"], t);  // start = server_time: in the archive
    EXPECT_EQ(events[24]["in_archive"], true);
    EXPECT_EQ(events[25]["in_archive"], false);
}

TEST_F(Tst319, EpgInvalidInputsIn04) {
    const int64_t t = kT;
    // IN-04: without `to`; from = to; to - from = 86 401; from = abc -> 400 INVALID_RANGE.
    expect_error(test::http_get(port(), "/api/v1/epg?from=" + std::to_string(t)), 400, "INVALID_RANGE");
    expect_error(test::http_get(port(), epg_target(t, t)), 400, "INVALID_RANGE");
    expect_error(test::http_get(port(), epg_target(t, t + 86401)), 400, "INVALID_RANGE");
    expect_error(test::http_get(port(), "/api/v1/epg?from=abc&to=" + std::to_string(t)), 400, "INVALID_RANGE");
    // IN-04: service_id = svc-9 -> 404 SERVICE_NOT_FOUND.
    expect_error(test::http_get(port(), epg_target(t - 7200, t + 7200, "&service_id=svc-9")), 404,
                 "SERVICE_NOT_FOUND");

    // Further invalid ranges: missing both, from > to, non-integers, out of int64_t range.
    expect_error(test::http_get(port(), "/api/v1/epg"), 400, "INVALID_RANGE");
    expect_error(test::http_get(port(), epg_target(t + 1, t)), 400, "INVALID_RANGE");
    expect_error(test::http_get(port(), "/api/v1/epg?from=1.5&to=600"), 400, "INVALID_RANGE");
    expect_error(test::http_get(port(), "/api/v1/epg?from=%2B5&to=600"), 400, "INVALID_RANGE");
    expect_error(test::http_get(port(), "/api/v1/epg?from=0&to=99999999999999999999"), 400, "INVALID_RANGE");
    expect_error(test::http_get(port(), "/api/v1/epg?from=0&to="), 400, "INVALID_RANGE");
    // A range error is reported before an unknown service.
    expect_error(test::http_get(port(), epg_target(t, t, "&service_id=svc-9")), 400, "INVALID_RANGE");

    // The largest range is valid.
    const json body = get_ok(epg_target(t, t + 86400, "&service_id=svc-1"));
    ASSERT_EQ(body["services"].size(), 1u);
    EXPECT_EQ(body["services"][0]["events"].size(), 289u);  // t mod 300 = 100: 288 + 1
}

TEST_F(Tst319, EpgCycleFormulaOfSection477) {  // fixture unused; one fixture per suite
    srv::Service s;
    s.service_id = "svc-7";
    s.programs = {{"A", "a", 100}, {"B", "b", 200}, {"C", "c", 300}};  // D = 600, o = 0, 100, 300
    auto starts = [&s](int64_t from, int64_t to) {
        std::vector<std::pair<int64_t, std::string>> out;
        for (const srv::EpgEvent& e : srv::epg_events(s, from, to)) {
            EXPECT_EQ(e.duration, e.program->duration);
            out.emplace_back(e.start, e.program->title);
        }
        return out;
    };
    using V = std::vector<std::pair<int64_t, std::string>>;
    EXPECT_EQ(starts(0, 600), (V{{0, "A"}, {100, "B"}, {300, "C"}}));
    EXPECT_EQ(starts(50, 150), (V{{0, "A"}, {100, "B"}}));     // overlapping at both ends
    EXPECT_EQ(starts(100, 101), (V{{100, "B"}}));              // A ends at 100: not overlapping
    EXPECT_EQ(starts(599, 601), (V{{300, "C"}, {600, "A"}}));  // across the cycle boundary
    EXPECT_EQ(starts(-650, -500), (V{{-900, "C"}, {-600, "A"}}));  // negative times, floor division
    EXPECT_TRUE(starts(10, 10).empty());

    // Near the int64_t limits: no overflow, every event overlaps and the events are contiguous.
    const int64_t max = std::numeric_limits<int64_t>::max();
    const int64_t min = std::numeric_limits<int64_t>::min();
    for (const auto& [from, to] : {std::pair<int64_t, int64_t>{max - 1000, max}, {min, min + 1000}}) {
        const auto events = srv::epg_events(s, from, to);
        ASSERT_FALSE(events.empty());
        for (size_t i = 0; i < events.size(); ++i) {
            EXPECT_LT(events[i].start, to);
            if (i + 1 < events.size()) {
                EXPECT_EQ(events[i].start + events[i].duration, events[i + 1].start);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// TST-318 — invalid requests to the OTT MW server (REQ-58, ER-18); head-end part in step 3
// ---------------------------------------------------------------------------------------------

TEST(Tst318, MwServerAnswersIn01WithErrorObjectsAndKeepsRunning) {
    MwServerUnderTest mw;
    std::mt19937 rng(318);
    std::string random_bytes(4096, '\0');
    for (auto& c : random_bytes) c = static_cast<char>(rng() & 0xFF);
    auto post = [](const std::string& path, const std::string& body) {
        return "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    };
    auto expect_400_or_404 = [](const test::HttpReply& reply, const char* what) {
        SCOPED_TRACE(what);
        ASSERT_TRUE(reply.answered);
        EXPECT_TRUE(reply.status == 400 || reply.status == 404) << reply.status;
        expect_error(reply, reply.status);
    };

    // IN-01: truncated JSON body; body of 4 096 random bytes; unknown path; method DELETE.
    expect_400_or_404(test::http_exchange(mw.port(), post("/api/v1/services", "{\"service_id\": ")), "truncated JSON");
    expect_400_or_404(test::http_exchange(mw.port(), post("/api/v1/epg", random_bytes)), "random body");
    expect_400_or_404(test::http_get(mw.port(), "/api/v1/xyz"), "unknown path");
    expect_400_or_404(test::http_exchange(mw.port(), "DELETE /api/v1/services HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"),
                      "DELETE");

    // Not HTTP at all: Mongoose drops the connection; the server keeps running.
    test::http_exchange(mw.port(), random_bytes);
    // Unknown paths outside /api/v1 and an invalid percent-encoding.
    expect_400_or_404(test::http_get(mw.port(), "/"), "root");
    expect_400_or_404(test::http_get(mw.port(), "/api/v1/services%zz"), "bad encoding");

    // The server still answers a valid request.
    const json list = mw.get_ok("/api/v1/services");
    ASSERT_TRUE(list.is_object());
    EXPECT_EQ(list["services"].size(), 4u);
}

// ---------------------------------------------------------------------------------------------
// TST-320 — missing or invalid configuration (ER-37), OTT MW server part and common readers
// ---------------------------------------------------------------------------------------------

namespace {

// Message of the ConfigError thrown by `f`, or "" if none.
template <typename F>
std::string config_error(F f) {
    try {
        f();
    } catch (const srv::ConfigError& e) {
        return e.what();
    }
    return "";
}

const char* kValidServices = R"({"archive_window_s": 7200, "services": [
  {"service_id": "svc-1", "number": 1, "name": "One", "description": "d", "protected": false,
   "file": "svc-1.ts", "programs": [{"title": "P1", "description": "p", "duration": 300},
                                    {"title": "P2", "description": "p", "duration": 300}]}]})";

}  // namespace

TEST(Tst320, ConfigReadersReadValuesDefaultsAndRejectInvalidFiles) {
    // The repository files (Section 4.7.1).
    const srv::ServerConfig c = srv::load_server_config(kRepo + "/server/config.json");
    EXPECT_EQ(c.mw_port, 8080);
    EXPECT_EQ(c.headend_port, 8081);
    EXPECT_EQ(c.headend_url, kStreamControlUrl);
    EXPECT_EQ(c.services_file, "content/services.json");
    EXPECT_EQ(c.content_dir, "content");
    EXPECT_EQ(c.log_dir, "logs");
    const srv::ServerConfig t = srv::load_server_config(kRepo + "/server/config.test.json");
    EXPECT_EQ(t.services_file, "content/test/services.test.json");
    EXPECT_EQ(t.mw_port, c.mw_port);
    EXPECT_EQ(t.headend_port, c.headend_port);
    EXPECT_EQ(t.headend_url, c.headend_url);
    EXPECT_EQ(t.content_dir, c.content_dir);
    EXPECT_EQ(t.log_dir, c.log_dir);
    const srv::ServiceCatalog cat = srv::load_services(kRepo + "/content/services.json");
    EXPECT_EQ(cat.archive_window_s, 7200);
    ASSERT_EQ(cat.services.size(), 4u);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(cat.services[i].service_id, kExpected[i].id);
        EXPECT_EQ(cat.services[i].cycle_duration(), 600);
    }
    EXPECT_EQ(srv::load_services(kRepo + "/content/test/services.test.json").services.size(), 4u);

    test::TempDir dir;
    // Missing keys keep the defaults (NFR-16); unknown keys are ignored (Section 3.1).
    test::write_text(dir.file("empty.json"), R"({"unknown_key": [1, 2]})");
    const srv::ServerConfig d = srv::load_server_config(dir.file("empty.json"));
    EXPECT_EQ(d.mw_port, 8080);
    EXPECT_EQ(d.headend_port, 8081);
    EXPECT_EQ(d.headend_url, kStreamControlUrl);
    EXPECT_EQ(d.services_file, "content/services.json");
    EXPECT_EQ(d.content_dir, "content");
    EXPECT_EQ(d.log_dir, "logs");

    // Invalid configuration files: the message names the file.
    const std::vector<std::pair<std::string, std::string>> bad_configs = {
        {"truncated.json", R"({"mw_port": )"},
        {"array.json", "[1, 2]"},
        {"port-string.json", R"({"mw_port": "8080"})"},
        {"port-range.json", R"({"headend_port": 70000})"},
        {"port-float.json", R"({"mw_port": 8080.5})"},
        {"empty-dir.json", R"({"log_dir": ""})"},
    };
    for (const auto& [name, text] : bad_configs) {
        test::write_text(dir.file(name), text);
        const std::string msg = config_error([&] { srv::load_server_config(dir.file(name)); });
        EXPECT_NE(msg.find(dir.file(name)), std::string::npos) << name << ": " << msg;
    }
    const std::string missing = config_error([&] { srv::load_server_config(dir.file("missing.json")); });
    EXPECT_NE(missing.find(dir.file("missing.json")), std::string::npos) << missing;

    // Invalid services.json files: the message names the file.
    json valid = json::parse(kValidServices);
    test::write_text(dir.file("valid-services.json"), valid.dump());
    EXPECT_EQ(srv::load_services(dir.file("valid-services.json")).services.size(), 1u);
    auto variant = [&valid](const std::function<void(json&)>& change) {
        json j = valid;
        change(j);
        return j.dump();
    };
    const std::vector<std::pair<std::string, std::string>> bad_services = {
        {"s-truncated.json", R"({"services": [)"},
        {"s-no-window.json", variant([](json& j) { j.erase("archive_window_s"); })},
        {"s-no-services.json", variant([](json& j) { j["services"] = json::array(); })},
        {"s-no-name.json", variant([](json& j) { j["services"][0].erase("name"); })},
        {"s-bad-id.json", variant([](json& j) { j["services"][0]["service_id"] = "one"; })},
        {"s-number-0.json", variant([](json& j) { j["services"][0]["number"] = 0; })},
        {"s-number-1000.json", variant([](json& j) { j["services"][0]["number"] = 1000; })},
        {"s-dup-number.json", variant([](json& j) {
             json second = j["services"][0];
             second["service_id"] = "svc-2";
             j["services"].push_back(second);
         })},
        {"s-dup-id.json", variant([](json& j) {
             json second = j["services"][0];
             second["number"] = 2;
             j["services"].push_back(second);
         })},
        {"s-protected-yes.json", variant([](json& j) { j["services"][0]["protected"] = "yes"; })},
        {"s-no-file.json", variant([](json& j) { j["services"][0].erase("file"); })},
        {"s-no-programs.json", variant([](json& j) { j["services"][0]["programs"] = json::array(); })},
        {"s-duration-0.json", variant([](json& j) { j["services"][0]["programs"][0]["duration"] = 0; })},
        {"s-no-title.json", variant([](json& j) { j["services"][0]["programs"][1].erase("title"); })},
    };
    for (const auto& [name, text] : bad_services) {
        test::write_text(dir.file(name), text);
        const std::string msg = config_error([&] { srv::load_services(dir.file(name)); });
        EXPECT_NE(msg.find(dir.file(name)), std::string::npos) << name << ": " << msg;
    }
}

namespace {

const std::string kMwServer = M15_MW_SERVER_BIN;

// Runs mw_server with `config` and returns its exit code and output.
std::pair<int, std::string> run_mw_server(const test::TempDir& dir, const std::string& config) {
    const std::string out = dir.file("output.txt");
    test::Process p({kMwServer, "--config", config}, out);
    EXPECT_TRUE(p.started());
    const int code = p.wait(std::chrono::seconds(10));
    return {code, test::read_text(out)};
}

std::string server_config(const test::TempDir& dir, const std::string& services_file, int port) {
    json c;
    c["mw_port"] = port;
    c["services_file"] = services_file;
    c["log_dir"] = dir.file("logs");
    return c.dump();
}

}  // namespace

TEST(Tst320, MwServerExitsWithCode1NamingTheFile) {
    test::TempDir dir;
    const int port = test::free_tcp_port();
    struct Case {
        const char* what;
        std::string config;         // path passed with --config
        std::string expected_name;  // file the message must name
    };
    std::vector<Case> cases;

    // server/config.json missing, and with invalid JSON.
    cases.push_back({"config missing", dir.file("missing-config.json"), dir.file("missing-config.json")});
    test::write_text(dir.file("bad-config.json"), R"({"mw_port": 8080, )");
    cases.push_back({"config invalid JSON", dir.file("bad-config.json"), dir.file("bad-config.json")});

    // services.json missing, with invalid JSON and with an invalid entry.
    test::write_text(dir.file("cfg-missing-services.json"), server_config(dir, dir.file("missing-services.json"), port));
    cases.push_back({"services missing", dir.file("cfg-missing-services.json"), dir.file("missing-services.json")});
    test::write_text(dir.file("bad-services.json"), R"({"archive_window_s": 7200, "services": [ {)");
    test::write_text(dir.file("cfg-bad-services.json"), server_config(dir, dir.file("bad-services.json"), port));
    cases.push_back({"services invalid JSON", dir.file("cfg-bad-services.json"), dir.file("bad-services.json")});
    json no_name = json::parse(kValidServices);
    no_name["services"][0].erase("name");
    test::write_text(dir.file("noname-services.json"), no_name.dump());
    test::write_text(dir.file("cfg-noname-services.json"), server_config(dir, dir.file("noname-services.json"), port));
    cases.push_back({"services invalid entry", dir.file("cfg-noname-services.json"), dir.file("noname-services.json")});

    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        const auto [code, output] = run_mw_server(dir, c.config);
        EXPECT_EQ(code, 1) << output;
        EXPECT_NE(output.find(c.expected_name), std::string::npos) << output;
    }
}

TEST(Tst320, MwServerStartsWithValidConfigLogsAndStopsOnSigterm) {
    // Control case of the test above: the same program with valid files starts, answers, writes
    // log lines of NFR-13 into a log directory it creates, and exits with 0 on SIGTERM.
    test::TempDir dir;
    const int port = test::free_tcp_port();
    json c;
    c["mw_port"] = port;
    c["services_file"] = kRepo + "/content/services.json";
    c["log_dir"] = dir.file("logs/nested");  // does not exist yet
    test::write_text(dir.file("config.json"), c.dump());

    test::Process p({kMwServer, "--config", dir.file("config.json")}, dir.file("output.txt"));
    ASSERT_TRUE(p.started());
    test::HttpReply reply;
    for (int i = 0; i < 100 && !reply.answered; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        reply = test::http_get(port, "/api/v1/services");
    }
    ASSERT_TRUE(reply.answered) << test::read_text(dir.file("output.txt"));
    EXPECT_EQ(reply.status, 200);
    p.signal(SIGTERM);
    EXPECT_EQ(p.wait(std::chrono::seconds(5)), 0);

    const std::string log = test::read_text(dir.file("logs/nested/mw_server.log"));
    ASSERT_FALSE(log.empty());
    const std::regex line_format(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3} (DEBUG|INFO|WARN|ERROR) \[SRV-MW\] .+$)");
    std::istringstream lines(log);
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        EXPECT_TRUE(std::regex_match(line, line_format)) << line;
        ++count;
    }
    EXPECT_GE(count, 3);
    EXPECT_NE(log.find("INFO [SRV-MW] listening on 127.0.0.1:" + std::to_string(port)), std::string::npos) << log;
    EXPECT_NE(log.find("INFO [SRV-MW] GET /api/v1/services -> 200"), std::string::npos) << log;
    EXPECT_NE(log.find("INFO [SRV-MW] stopped by SIGTERM"), std::string::npos) << log;
}
