// Test harness of the AL-SES module tests (session_controller_test.cpp, session_keys_test.cpp): all AL
// modules are real; the platform is the fakes of step 3, wired as AL-MAIN wires the real one.
// Not part of the code under test.
#pragma once

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "content.hpp"
#include "fakes/call_log.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_demux.hpp"
#include "fakes/fake_hls_store.hpp"
#include "fakes/fake_http_client.hpp"
#include "fakes/fake_logger.hpp"
#include "fakes/fake_tuner.hpp"
#include "headend_client.hpp"
#include "hls_packager.hpp"
#include "key_manager.hpp"
#include "psi_builder.hpp"
#include "psi_parser.hpp"
#include "service_manager.hpp"
#include "session_controller.hpp"
#include "stream_gen.hpp"
#include "task_queue.hpp"
#include "ts_builder.hpp"

namespace sestest {

using fakes::FakeHttpClient;
using nlohmann::json;
using testutil::Bytes;

inline const std::string kMw = "http://127.0.0.1:8080/api/v1";
inline const std::string kHe = "http://127.0.0.1:8081/api/v1/stream";
inline constexpr std::int64_t kT = 1795000000;   // worked example of Section 5.2.4 (FakeClock default)

struct Harness {
    fakes::CallLog calls;
    fakes::FakeClock clock;
    fakes::FakeLogger logger;
    al::TaskQueue queue{clock, logger};
    fakes::FakeTuner tuner{&calls};
    fakes::FakeDemux demux{&calls};
    FakeHttpClient http{clock, &calls};
    fakes::FakeHlsStore store{&calls};
    al::ServiceManager svc{http, clock, logger, queue, al::ServiceManager::Config{}};
    al::HeadendClient hec{http, logger, queue};
    al::PsiParser psi{demux, logger, queue};
    al::KeyManager dsc{demux, logger, queue};
    al::HlsPackager out{store, logger};
    std::vector<al::SessionState> states;
    std::vector<std::pair<al::SessionState, al::StreamDescription>> infos;
    al::SessionController ses{tuner, demux, clock, logger, queue, svc, hec, psi, dsc, out,
                              al::SessionController::Config{}, listener(), 42};
    testutil::StreamGen gen;

    al::SessionController::Listener listener() {
        al::SessionController::Listener l;
        l.on_session_state = [this](const al::SessionState& s) { states.push_back(s); };
        l.on_stream_info = [this](const al::SessionState& s, const al::StreamDescription& d) { infos.emplace_back(s, d); };
        return l;
    }

    void settle() {
        for (int i = 0; i < 100000; ++i) {
            clock.advance(0);
            if (queue.run_pending() == 0) return;
        }
        ADD_FAILURE() << "queue does not settle";
    }
    void advance(std::int64_t ms) {
        clock.advance(ms);
        settle();
    }

    // One packet at a time through the fake demux, as the tuner thread delivers them.
    void feed(const std::vector<Bytes>& packets) {
        for (const Bytes& p : packets) {
            demux.feed(p);
            settle();
        }
    }
    bool feed_content(const std::string& name, double seconds) {
        const std::string path = testutil::content_path(name);
        if (!testutil::content_exists(path)) return false;
        const std::size_t count = testutil::packets_for_seconds(seconds);
        for (std::size_t i = 0; i < count; ++i) {
            if (demux.feed_file(path, i, 1) != 1) break;
            settle();
        }
        return true;
    }

    // play -> waiting_stream -> Locked -> the stream until the first segment is listed (2 s).
    void play_until_playing(const std::string& service = "svc-1") {
        ses.play(service, {});
        settle();
        ASSERT_TRUE(tuner.emit(pal::TunerStatus::Locked));
        settle();
        feed(gen.frames(51));
    }

    std::vector<std::string> state_names(std::uint64_t session) const {
        std::vector<std::string> names;
        for (const auto& s : states) {
            if (s.session == session) names.emplace_back(al::to_string(s.state));
        }
        return names;
    }

    json last_start_body() const {
        const auto* r = http.last("POST", kHe + "/start");
        return r ? json::parse(r->body) : json();
    }
};

// Index of the first call-log entry starting with `prefix`, at or after `from`.
inline std::size_t index_of(const std::vector<std::string>& entries, const std::string& prefix, std::size_t from = 0) {
    for (std::size_t i = from; i < entries.size(); ++i) {
        if (entries[i].rfind(prefix, 0) == 0) return i;
    }
    return entries.size();
}

inline std::string error_code(const Harness& h, std::uint64_t session) {
    for (auto it = h.states.rbegin(); it != h.states.rend(); ++it) {
        if (it->session != session) continue;
        return it->state == al::State::Error && it->error ? it->error->code : "";
    }
    return "";
}

}  // namespace sestest
