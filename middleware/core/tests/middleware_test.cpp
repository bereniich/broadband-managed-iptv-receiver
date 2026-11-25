// AL-MAIN tests (step 7e): configuration reader (TST-158), composition root and orderly stop (TST-146,
// T-31). Expected values: the key table of Section 4.2.5, IN-11 of Section 5.2.5, TST-146 (/stop
// answered after 100 ms / not answered: completion at the response / at 2000 ms).
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <json.hpp>

#include "fakes/fake_platform.hpp"
#include "middleware.hpp"
#include "middleware_config.hpp"
#include "stream_gen.hpp"

#ifndef M15_MIDDLEWARE_JSON
#error "M15_MIDDLEWARE_JSON must be defined by the build (path of middleware/main/middleware.json)"
#endif

namespace {

using fakes::FakeHttpClient;
using nlohmann::json;
using testutil::Bytes;

const std::string kHe = "http://127.0.0.1:8081/api/v1/stream";

// A temporary directory removed at the end of the test.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::random_device device;
        std::ostringstream name;
        name << "m15-al-config-" << std::hex << device() << device();
        path = std::filesystem::temp_directory_path() / name.str();
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string write(const std::string& name, const std::string& text) const {
        const std::filesystem::path file = path / name;
        std::ofstream(file) << text;
        return file.string();
    }
};

void expect_defaults_of_section_4_2_5(const al::MiddlewareConfig& c, bool skip_rtp_port = false) {
    EXPECT_EQ(c.mw_base_url, "http://127.0.0.1:8080/api/v1");
    if (!skip_rtp_port) {
        EXPECT_EQ(c.rtp_port, 5004);
    }
    EXPECT_EQ(c.client_host, "127.0.0.1");
    EXPECT_EQ(c.ui_http_port, 8090);
    EXPECT_EQ(c.webui_dir, "webui");
    EXPECT_EQ(c.hls_dir, "/tmp/m15-hls");
    EXPECT_EQ(c.log_file, "logs/middleware.log");
    EXPECT_EQ(c.mw_timeout_ms, 800);
    EXPECT_EQ(c.headend_timeout_ms, 2000);
    EXPECT_EQ(c.mw_retry_s, 5);
    EXPECT_EQ(c.keepalive_interval_s, 5);
    EXPECT_EQ(c.keepalive_loss_s, 15);
    EXPECT_EQ(c.no_data_timeout_s, 5);
    EXPECT_EQ(c.stream_deadline_s, 3);
}

// ---------------------------------------------------------------- TST-158

TEST(Tst158, ShippedFileHasTheDefaults) {
    const al::ConfigResult r = al::read_config(M15_MIDDLEWARE_JSON);
    ASSERT_TRUE(r.ok) << r.error;
    expect_defaults_of_section_4_2_5(r.config);
    // Every key of Section 4.2.5 is present in the file itself (not only defaulted).
    std::ifstream file(M15_MIDDLEWARE_JSON);
    const json j = json::parse(file);
    EXPECT_EQ(j.size(), 14u);
}

TEST(Tst158, MissingKeysUseDefaults) {
    TempDir dir;
    const al::ConfigResult empty = al::read_config(dir.write("empty.json", "{}"));
    ASSERT_TRUE(empty.ok) << empty.error;
    expect_defaults_of_section_4_2_5(empty.config);

    const al::ConfigResult one = al::read_config(dir.write("one.json", R"({"rtp_port": 6004, "unknown_key": true})"));
    ASSERT_TRUE(one.ok) << one.error;
    EXPECT_EQ(one.config.rtp_port, 6004);
    expect_defaults_of_section_4_2_5(one.config, true);
}

TEST(Tst158, InvalidFiles) {
    TempDir dir;
    const std::string missing = (dir.path / "missing.json").string();
    const al::ConfigResult r1 = al::read_config(missing);   // IN-11: file missing
    EXPECT_FALSE(r1.ok);
    EXPECT_NE(r1.error.find(missing), std::string::npos) << r1.error;

    const std::string broken = dir.write("broken.json", R"({"rtp_port": 5004,)");   // IN-11: invalid JSON
    const al::ConfigResult r2 = al::read_config(broken);
    EXPECT_FALSE(r2.ok);
    EXPECT_NE(r2.error.find(broken), std::string::npos) << r2.error;
    EXPECT_NE(r2.error.find("invalid JSON"), std::string::npos);

    const struct {
        const char* text;
        const char* key;
    } bad[] = {{R"({"rtp_port": "abc"})", "rtp_port"},
               {R"({"rtp_port": 70000})", "rtp_port"},
               {R"({"mw_timeout_ms": 0})", "mw_timeout_ms"},
               {R"({"mw_base_url": ""})", "mw_base_url"},
               {R"([1, 2])", nullptr}};
    for (const auto& b : bad) {
        const std::string file = dir.write("bad.json", b.text);
        const al::ConfigResult r = al::read_config(file);
        EXPECT_FALSE(r.ok) << b.text;
        EXPECT_NE(r.error.find(file), std::string::npos) << r.error;
        if (b.key) {
            EXPECT_NE(r.error.find(b.key), std::string::npos) << r.error;
        }
    }
}

// ---------------------------------------------------------------- composition root

struct Harness {
    fakes::FakePlatform platform;
    al::MiddlewareConfig config;
    std::unique_ptr<al::Middleware> mw;
    testutil::StreamGen gen;

    explicit Harness(al::MiddlewareConfig c = {}) : platform(c.mw_base_url), config(std::move(c)) {
        mw = std::make_unique<al::Middleware>(platform, config);
    }

    void settle() {
        for (int i = 0; i < 100000; ++i) {
            platform.fake_clock().advance(0);
            if (mw->queue().run_pending() == 0) return;
        }
        ADD_FAILURE() << "queue does not settle";
    }
    void advance(std::int64_t ms) {
        platform.fake_clock().advance(ms);
        settle();
    }
    void command(std::int64_t id, const std::string& name, const json& params) {
        platform.fake_ui().receive(1, json{{"type", "command"}, {"id", id}, {"name", name}, {"params", params}}.dump());
        settle();
    }
    void play_until_playing() {
        mw->load_services();
        settle();
        platform.fake_ui().connect(1);
        command(1, "play", {{"service_id", "svc-1"}, {"start", "live"}});
        platform.fake_tuner().emit(pal::TunerStatus::Locked);
        settle();
        for (const Bytes& p : gen.frames(51)) {
            platform.fake_demux().feed(p);
            settle();
        }
    }
};

TEST(Tst158, CompositionWiring) {
    al::MiddlewareConfig c;
    c.mw_base_url = "http://127.0.0.1:9080/api/v1";   // values of TST-718 style: not the defaults
    c.rtp_port = 6004;
    c.client_host = "127.0.0.2";
    Harness h(c);
    h.mw->load_services();
    h.settle();
    EXPECT_EQ(h.platform.fake_http().count("GET", "http://127.0.0.1:9080/api/v1/services"), 1u);
    EXPECT_EQ(h.platform.fake_http().last("GET", "http://127.0.0.1:9080/api/v1/services")->timeout_ms, 800);

    h.platform.fake_ui().connect(1);
    h.settle();
    EXPECT_EQ(h.platform.fake_ui().events("mw_status").size(), 2u);   // broadcast at the result, send on connect
    h.command(1, "play", {{"service_id", "svc-1"}, {"start", "live"}});
    const std::uint32_t ssrc = h.mw->session().ssrc();
    EXPECT_NE(h.platform.calls.find("tune 6004 " + std::to_string(ssrc)), fakes::CallLog::npos);
    const json body = json::parse(h.platform.fake_http().last("POST", kHe + "/start")->body);
    EXPECT_EQ(body["client_port"], 6004);
    EXPECT_EQ(body["client_host"], "127.0.0.2");
    EXPECT_EQ(h.platform.fake_http().last("POST", kHe + "/start")->timeout_ms, 2000);
    EXPECT_EQ(h.platform.fake_ui().responses(1).size(), 1u);
}

// ---------------------------------------------------------------- TST-146, T-31

TEST(Tst146, ShutdownSendsStop) {
    for (bool answered : {true, false}) {
        SCOPED_TRACE(answered ? "(a) /stop answered after 100 ms" : "(b) /stop not answered");
        Harness h;
        h.play_until_playing();
        ASSERT_EQ(h.mw->session().state().state, al::State::Playing);
        h.platform.fake_http().script("POST", kHe + "/stop",
                                      FakeHttpClient::Reply::file(200, "he_stop.json", answered ? 100 : 10000));
        bool done = false;
        h.mw->request_shutdown([&done] { done = true; });
        h.settle();
        EXPECT_FALSE(done);
        ASSERT_NE(h.platform.fake_http().last("POST", kHe + "/stop"), nullptr);
        EXPECT_EQ(json::parse(h.platform.fake_http().last("POST", kHe + "/stop")->body)["session_id"], "hs-000017");
        EXPECT_EQ(h.mw->session().state().state, al::State::Stopped);
        const std::int64_t at = answered ? 100 : 2000;   // the response / the timeout of 2000 ms
        h.advance(at - 1);
        EXPECT_FALSE(done);
        h.advance(1);
        EXPECT_TRUE(done);
    }
}

TEST(Tst146, ShutdownWithoutSession) {
    Harness h;
    bool done = false;
    h.mw->request_shutdown([&done] { done = true; });
    h.settle();
    EXPECT_TRUE(done);   // nothing to release, no /stop
    EXPECT_EQ(h.platform.fake_http().count("POST", kHe + "/stop"), 0u);
}

TEST(Tst146, KeysZeroedAtFinish) {
    Harness h;
    ASSERT_TRUE(h.mw->keys().store("svc-3", "3A7F1C9E5B2D8046A1E3C5079B4D6F28"));
    std::vector<al::Key> zeroed;
    h.mw->keys().key_store().set_zeroed_hook([&zeroed](const al::Key& k) { zeroed.push_back(k); });
    h.mw->finish();
    ASSERT_EQ(zeroed.size(), 1u);
    EXPECT_EQ(zeroed[0], al::Key{});
    EXPECT_FALSE(h.mw->keys().has_key("svc-3"));
}

TEST(Tst146, WorkerShutdownWithoutSession) {
    Harness h;   // the real worker thread: shutdown() returns when the worker has executed the task
    h.mw->start_worker();
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_TRUE(h.mw->shutdown(std::chrono::milliseconds(2500)));
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(1000));
    h.mw->finish();   // joins the worker
}

}  // namespace
