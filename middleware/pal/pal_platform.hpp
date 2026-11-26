// The Linux platform of IF-INT-04 (Section 3.9.2): all PAL modules over the software HAL, created by
// create_platform(). The concrete class is visible to the PAL tests and tools (bound ports).
#pragma once

#include <memory>
#include <mutex>

#include "pal/platform.hpp"
#include "pal_clock.hpp"
#include "pal_event_loop.hpp"
#include "pal_hls_store.hpp"
#include "pal_http_client.hpp"
#include "pal_logger.hpp"
#include "pal_sdk.hpp"
#include "pal_ui_server.hpp"

namespace pal {

class LinuxPlatform : public Platform {
public:
    LinuxPlatform(const PlatformConfig& config, std::unique_ptr<Logger> logger);
    ~LinuxPlatform() override;

    ITuner& tuner() override { return tuner_; }
    IDemux& demux() override { return demux_; }
    IHttpClient& http() override { return http_; }
    IUiChannel& ui() override { return ui_; }
    IHlsStore& hls() override { return hls_; }
    IClock& clock() override { return clock_; }
    ILogger& logger() override { return *logger_; }

    // Empties hls_dir, starts the event loop and listens on 127.0.0.1:<ui_http_port>. A platform is
    // started once; start() after stop() returns false.
    bool start() override;
    // Untunes (and refuses later tunings), stops the event loop: no callback of any interface after
    // the return. Idempotent; also called by the destructor.
    void stop() override;

    std::uint16_t ui_port() const { return ui_.port(); }
    std::uint16_t tuner_port() const { return tuner_.bound_port(); }

private:
    PlatformConfig config_;
    std::unique_ptr<Logger> logger_;  // first: used by all others, also after stop()
    EventLoop loop_;
    Clock clock_;
    HlsStore hls_;
    HttpClient http_;
    UiServer ui_;
    SdkDemux demux_;
    SdkTuner tuner_;  // after the demux it feeds

    std::mutex control_mutex_;
    bool started_ = false;
    bool stopped_ = false;
};

}  // namespace pal
