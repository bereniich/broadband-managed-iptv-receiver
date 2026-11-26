// Fake pal::Platform of the AL unit tests: the fakes of step 3 behind the factory interface of
// Section 3.9.2, so that the composition root of AL-MAIN can be tested. Used from the test thread.
#pragma once

#include <string>
#include <utility>

#include "fakes/call_log.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_demux.hpp"
#include "fakes/fake_hls_store.hpp"
#include "fakes/fake_http_client.hpp"
#include "fakes/fake_logger.hpp"
#include "fakes/fake_tuner.hpp"
#include "fakes/fake_ui_channel.hpp"
#include "pal/platform.hpp"

namespace fakes {

class FakePlatform : public pal::Platform {
public:
    explicit FakePlatform(std::string mw_base_url = FakeHttpClient::kDefaultMwBaseUrl,
                          std::string stream_control_url = FakeHttpClient::kDefaultStreamControlUrl)
        : http_(clock_, &calls, std::move(mw_base_url), std::move(stream_control_url)) {}

    pal::ITuner& tuner() override { return tuner_; }
    pal::IDemux& demux() override { return demux_; }
    pal::IHttpClient& http() override { return http_; }
    pal::IUiChannel& ui() override { return ui_; }
    pal::IHlsStore& hls() override { return hls_; }
    pal::IClock& clock() override { return clock_; }
    pal::ILogger& logger() override { return logger_; }

    bool start() override {
        record(&calls, "Platform::start");
        return start_result;
    }
    void stop() override { record(&calls, "Platform::stop"); }

    // ---- test side
    CallLog calls;
    bool start_result = true;
    FakeTuner& fake_tuner() { return tuner_; }
    FakeDemux& fake_demux() { return demux_; }
    FakeHttpClient& fake_http() { return http_; }
    FakeUiChannel& fake_ui() { return ui_; }
    FakeHlsStore& fake_hls() { return hls_; }
    FakeClock& fake_clock() { return clock_; }
    FakeLogger& fake_logger() { return logger_; }

private:
    FakeClock clock_;
    FakeLogger logger_;
    FakeTuner tuner_{&calls};
    FakeDemux demux_{&calls};
    FakeHttpClient http_;
    FakeUiChannel ui_;
    FakeHlsStore hls_{&calls};
};

}  // namespace fakes
