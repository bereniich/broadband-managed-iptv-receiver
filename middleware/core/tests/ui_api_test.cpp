// AL-API module tests (step 7d): TST-157. The UI is played by the fake UI channel; AL-SVC, AL-HEC,
// AL-PSI, AL-DSC, AL-OUT and AL-SES are real, the platform is the fakes of step 3. Expected values: the
// envelopes, commands and events of Section 3.6 (JSON examples copied from the specification) and the
// invalid inputs IN-07 of Section 5.2.5.
#include <gtest/gtest.h>

#include <cctype>
#include <set>
#include <string>
#include <vector>

#include <json.hpp>

#include "fakes/call_log.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_demux.hpp"
#include "fakes/fake_hls_store.hpp"
#include "fakes/fake_http_client.hpp"
#include "fakes/fake_logger.hpp"
#include "fakes/fake_tuner.hpp"
#include "fakes/fake_ui_channel.hpp"
#include "headend_client.hpp"
#include "hls_packager.hpp"
#include "key_manager.hpp"
#include "psi_parser.hpp"
#include "service_manager.hpp"
#include "session_controller.hpp"
#include "stream_gen.hpp"
#include "task_queue.hpp"
#include "ui_api.hpp"

namespace {

using fakes::FakeHttpClient;
using nlohmann::json;
using testutil::Bytes;

const std::string kMw = "http://127.0.0.1:8080/api/v1";
const std::string kK3 = "3A7F1C9E5B2D8046A1E3C5079B4D6F28";   // Section 5.2.2

struct Harness {
    fakes::CallLog calls;
    fakes::FakeClock clock;
    fakes::FakeLogger logger;
    al::TaskQueue queue{clock, logger};
    fakes::FakeTuner tuner{&calls};
    fakes::FakeDemux demux{&calls};
    FakeHttpClient http{clock, &calls};
    fakes::FakeHlsStore store{&calls};
    fakes::FakeUiChannel ui;
    al::ServiceManager svc{http, clock, logger, queue, al::ServiceManager::Config{}};
    al::HeadendClient hec{http, logger, queue};
    al::PsiParser psi{demux, logger, queue};
    al::KeyManager dsc{demux, logger, queue};
    al::HlsPackager out{store, logger};
    al::UiApi api{ui, logger, queue, svc, dsc};
    al::SessionController ses{tuner, demux, clock, logger, queue, svc, hec, psi, dsc, out,
                              al::SessionController::Config{}, api.session_listener(), 42};
    testutil::StreamGen gen;

    Harness() {
        api.bind(ses);   // as AL-MAIN does
        svc.set_status_handler([this](bool reachable) { api.on_mw_status(reachable); });
    }

    void settle() {
        for (int i = 0; i < 100000; ++i) {
            clock.advance(0);
            if (queue.run_pending() == 0) return;
        }
        ADD_FAILURE() << "queue does not settle";
    }
    void load_services() {
        svc.load_services();
        settle();
    }
    void connect(pal::IUiChannel::ClientId client) {
        ui.connect(client);
        settle();
    }
    void command(pal::IUiChannel::ClientId client, const json& message) { raw(client, message.dump()); }
    void raw(pal::IUiChannel::ClientId client, const std::string& text) {
        ui.receive(client, text);
        settle();
    }
    json cmd(std::int64_t id, const std::string& name, const json& params = json::object()) const {
        return json{{"type", "command"}, {"id", id}, {"name", name}, {"params", params}};
    }
    json response(std::int64_t id) const {
        const auto r = ui.responses(id);
        EXPECT_EQ(r.size(), 1u) << "responses with id " << id;
        return r.empty() ? json() : r[0];
    }
    void play_until_playing() {
        command(1, cmd(100, "play", {{"service_id", "svc-1"}, {"start", "live"}}));
        tuner.emit(pal::TunerStatus::Locked);
        settle();
        for (const Bytes& p : gen.frames(51)) {
            demux.feed(p);
            settle();
        }
    }
    std::size_t frame_index(const std::function<bool(const json&)>& match) const {
        const auto frames = ui.json_frames();
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (match(frames[i])) return i;
        }
        return frames.size();
    }
};

// ---------------------------------------------------------------- TST-157

TEST(Tst157, EveryCommandOneResponse) {
    Harness h;
    h.connect(1);
    h.command(1, h.cmd(1, "get_status"));
    EXPECT_EQ(h.response(1), json::parse(R"({"type": "response", "id": 1, "ok": true, "result": {"state": "idle"}})"));

    h.load_services();
    h.command(1, h.cmd(2, "get_services"));
    // Section 3.2.1 / 3.6.2: the same objects as GET /services.
    EXPECT_EQ(h.response(2)["result"], json::parse(R"({
      "services": [
        { "service_id": "svc-1", "number": 1, "name": "Demo One",   "protected": false },
        { "service_id": "svc-2", "number": 2, "name": "Demo Two",   "protected": false },
        { "service_id": "svc-3", "number": 3, "name": "Premium One", "protected": true },
        { "service_id": "svc-4", "number": 4, "name": "Premium Two", "protected": true }
      ]})"));

    h.command(1, h.cmd(3, "get_epg", {{"from", 1794993000}, {"to", 1795003000}}));
    const json epg = h.response(3);
    EXPECT_EQ(epg["ok"], true);
    EXPECT_EQ(epg["result"]["server_time"], 1795000000);
    EXPECT_EQ(epg["result"]["services"][0]["events"][0]["event_id"], "svc-3-1794999600");

    h.command(1, h.cmd(4, "play", {{"service_id", "svc-1"}, {"start", "live"}}));
    EXPECT_EQ(h.response(4), json::parse(R"({"type": "response", "id": 4, "ok": true, "result": {"session": 1}})"));
    // The response to play comes before the first event of the new session (T-01, Section 4.4.4).
    const std::size_t i_response = h.frame_index([](const json& j) { return j.value("type", "") == "response" && j["id"] == 4; });
    const std::size_t i_starting = h.frame_index([](const json& j) {
        return j.value("name", "") == "session_state" && j["data"]["session"] == 1 && j["data"]["state"] == "starting";
    });
    EXPECT_LT(i_response, i_starting);

    h.command(1, h.cmd(5, "set_key", {{"service_id", "svc-3"}, {"key", kK3}}));
    EXPECT_EQ(h.response(5)["result"], json::object());
    EXPECT_TRUE(h.dsc.has_key("svc-3"));
    h.command(1, h.cmd(6, "stop"));
    EXPECT_EQ(h.response(6)["result"], json::object());
    EXPECT_EQ(h.ses.state().state, al::State::Stopped);

    // Responses go to the sending client only, events to all clients.
    for (const auto& f : h.ui.frames()) {
        const json j = json::parse(f.text);
        EXPECT_EQ(f.broadcast, j["type"] == "event") << f.text;
    }
}

TEST(Tst157, InvalidInput) {
    Harness h;
    h.load_services();
    h.connect(1);
    h.ui.clear();
    // IN-07.
    h.raw(1, "not json");
    h.command(1, json::parse(R"({"type": "command", "id": 5, "name": "fly", "params": {}})"));
    h.command(1, h.cmd(6, "play", {{"start", "live"}}));                                  // without service_id
    h.command(1, h.cmd(7, "play", {{"service_id", "svc-1"}, {"start", -5.5}}));          // start -5.5
    std::string random(1000, '\0');
    for (std::size_t i = 0; i < random.size(); ++i) random[i] = static_cast<char>((i * 151 + 7) & 0xFF);
    h.raw(1, random);                                                                      // 1 000 random bytes
    // Further envelope errors with a readable id.
    h.command(1, json::parse(R"({"type": "query", "id": 8, "name": "get_status", "params": {}})"));
    h.command(1, json::parse(R"({"type": "command", "id": 9, "name": "get_status"})"));
    h.command(1, json::parse(R"({"type": "command", "id": "ten", "name": "get_status", "params": {}})"));   // id not readable

    for (std::int64_t id : {5, 6, 7, 8, 9}) {
        const json r = h.response(id);
        EXPECT_EQ(r["ok"], false);
        EXPECT_EQ(r["error"]["code"], "INVALID_COMMAND");
        EXPECT_TRUE(r["error"]["message"].is_string());
    }
    EXPECT_EQ(h.ui.frames().size(), 5u);   // "not json", the random bytes and the unreadable id: no response
    EXPECT_GE(h.logger.count_containing("ignored message"), 3u);
    EXPECT_FALSE(h.ses.has_session());     // nothing was started
}

TEST(Tst157, MwStatusToNewClients) {
    Harness h;
    h.connect(1);                    // before the first result: nothing on connection (decision M)
    EXPECT_TRUE(h.ui.frames().empty());
    h.load_services();               // first result: broadcast
    ASSERT_EQ(h.ui.frames().size(), 1u);
    EXPECT_TRUE(h.ui.frames()[0].broadcast);
    EXPECT_EQ(json::parse(h.ui.frames()[0].text),
              json::parse(R"({ "type": "event", "name": "mw_status", "data": { "reachable": true } })"));
    h.connect(2);                    // a new client receives mw_status on connection
    ASSERT_EQ(h.ui.frames().size(), 2u);
    EXPECT_FALSE(h.ui.frames()[1].broadcast);
    EXPECT_EQ(h.ui.frames()[1].client, 2u);
    EXPECT_EQ(json::parse(h.ui.frames()[1].text)["data"]["reachable"], true);
}

TEST(Tst157, ErrorsOfCommands) {
    Harness h;
    h.connect(1);
    h.command(1, h.cmd(1, "get_services"));   // list not loaded
    EXPECT_EQ(h.response(1)["error"]["code"], "MW_UNREACHABLE");
    h.load_services();

    h.command(1, h.cmd(2, "play", {{"service_id", "svc-1"}, {"start", "live"}}));
    const std::uint64_t generation = h.ses.generation();
    h.command(1, h.cmd(3, "play", {{"service_id", "svc-9"}, {"start", "live"}}));
    EXPECT_EQ(h.response(3)["error"]["code"], "SERVICE_NOT_FOUND");
    EXPECT_EQ(h.ses.state().session, 1u);     // ER-07: the current session continues unchanged
    EXPECT_EQ(h.ses.generation(), generation);

    for (const char* bad : {"3A7F1C9E5B2D8046A1E3C5079B4D6F2", "3A7F1C9E5B2D8046A1E3C5079B4D6F2G"}) {   // KF1, KF2
        h.command(1, h.cmd(10, "set_key", {{"service_id", "svc-3"}, {"key", bad}}));
        const auto r = h.ui.responses(10);
        ASSERT_FALSE(r.empty());
        EXPECT_EQ(r.back()["error"]["code"], "INVALID_KEY_FORMAT");
    }
    EXPECT_FALSE(h.dsc.has_key("svc-3"));     // ER-32: nothing stored
    h.command(1, h.cmd(11, "set_key", {{"service_id", "svc-9"}, {"key", kK3}}));
    EXPECT_EQ(h.response(11)["error"]["code"], "SERVICE_NOT_FOUND");

    h.command(1, h.cmd(12, "get_epg", {{"from", 1795000000}, {"to", 1795000000}}));
    EXPECT_EQ(h.response(12)["error"]["code"], "INVALID_RANGE");
    h.command(1, h.cmd(13, "get_epg", {{"from", 1795000000}, {"to", 1795000000 + 86401}}));
    EXPECT_EQ(h.response(13)["error"]["code"], "INVALID_RANGE");
    EXPECT_EQ(h.http.count("GET", kMw + "/epg"), 0u);   // decided without a request
    h.command(1, h.cmd(14, "get_epg", {{"from", "abc"}, {"to", 1795000000}}));
    EXPECT_EQ(h.response(14)["error"]["code"], "INVALID_COMMAND");
}

TEST(Tst157, EventFormats) {
    Harness h;
    h.load_services();
    h.connect(1);
    h.play_until_playing();

    const auto states = h.ui.events("session_state");
    ASSERT_GE(states.size(), 3u);
    const std::set<std::string> fields = {"session", "service_id", "state", "reason", "start", "start_time",
                                          "playing_since", "protected", "error"};
    for (const json& d : states) {
        std::set<std::string> keys;
        for (auto it = d.begin(); it != d.end(); ++it) keys.insert(it.key());
        EXPECT_EQ(keys, fields) << d.dump();
    }
    EXPECT_EQ(states[0], json::parse(R"({"session": 1, "service_id": "svc-1", "state": "starting", "reason": null,
        "start": "live", "start_time": null, "playing_since": null, "protected": null, "error": null})"));
    EXPECT_EQ(states.back(), json::parse(R"({"session": 1, "service_id": "svc-1", "state": "playing", "reason": null,
        "start": "live", "start_time": 1794999600, "playing_since": 1795000000, "protected": false, "error": null})"));

    // stream_info: the example of Section 3.6.3 apart from session and service_id.
    const auto infos = h.ui.events("stream_info");
    ASSERT_EQ(infos.size(), 1u);
    json expected = json::parse(R"({
        "session": 12, "service_id": "svc-3", "program_number": 1, "pmt_pid": 4096, "pcr_pid": 256,
        "video": { "pid": 256, "stream_type": 27, "codec": "H.264" },
        "audio": { "pid": 257, "stream_type": 15, "codec": "AAC" } })");
    expected["session"] = 1;
    expected["service_id"] = "svc-1";
    EXPECT_EQ(infos[0], expected);

    h.command(1, h.cmd(200, "get_status"));
    EXPECT_EQ(h.response(200)["result"], states.back());   // the last session_state data

    EXPECT_TRUE(h.logger.contains("recv command id=100 name=play service_id=svc-1 start=live"));
    EXPECT_TRUE(h.logger.contains("send event session_state session=1 state=playing reason=null start=live start_time=1794999600"));
    EXPECT_TRUE(h.logger.contains("send event stream_info session=1 program=1 pmt_pid=4096 pcr_pid=256 video=256 audio=257"));
}

TEST(Tst157, KeyNeverInFramesOrLogs) {
    Harness h;
    h.load_services();
    h.connect(1);
    h.command(1, h.cmd(1, "set_key", {{"service_id", "svc-3"}, {"key", kK3}}));
    EXPECT_EQ(h.response(1)["ok"], true);
    EXPECT_TRUE(h.logger.contains("name=set_key key=3A7F\xE2\x80\xA6 service_id=svc-3"));
    std::string lower = kK3;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& line : h.logger.lines()) {
        EXPECT_EQ(line.message.find(kK3), std::string::npos) << line.message;
        EXPECT_EQ(line.message.find(lower), std::string::npos) << line.message;
    }
    for (const auto& f : h.ui.frames()) EXPECT_EQ(f.text.find(kK3), std::string::npos) << f.text;   // REQ-55
}

}  // namespace
