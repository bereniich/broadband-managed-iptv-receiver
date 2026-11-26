#include "pal_platform.hpp"

#include <cstdio>
#include <iostream>

namespace pal {

namespace {

constexpr const char* kModule = "PAL";

}  // namespace

LinuxPlatform::LinuxPlatform(const PlatformConfig& config, std::unique_ptr<Logger> logger)
    : config_(config),
      logger_(std::move(logger)),
      loop_(logger_.get()),
      clock_(loop_),
      hls_(config.hls_dir, logger_.get()),
      http_(loop_, logger_.get()),
      ui_(loop_, logger_.get(), config.webui_dir, config.hls_dir),
      demux_(logger_.get()),
      tuner_(demux_, logger_.get()) {}

LinuxPlatform::~LinuxPlatform() { stop(); }

bool LinuxPlatform::start() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (stopped_) return false;  // a stopped platform is not started again
    if (started_) return true;
    if (!hls_.reset()) return false;  // logged by PAL-FS
    if (!loop_.start()) return false;
    if (!ui_.listen(config_.ui_http_port)) {
        loop_.stop();
        stopped_ = true;
        return false;
    }
    started_ = true;
    return true;
}

void LinuxPlatform::stop() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (stopped_) return;
    stopped_ = true;
    tuner_.shutdown();          // no tuner status or demux data after this
    http_.disable_callbacks();  // connections closed by the stop are not reported
    ui_.disable_callbacks();
    loop_.stop();               // no timer, HTTP or WebSocket callback after this
    if (started_) logger_->log(LogLevel::Info, kModule, "platform stopped");
}

std::unique_ptr<Platform> create_platform(const PlatformConfig& config) {
    try {
        auto logger = std::make_unique<Logger>(config.log_file, &std::cerr);
        if (!logger->ok()) {
            // Without its log the process has no evidence (NFR-13, HLR-23): AL-MAIN exits with code 1.
            std::fprintf(stderr, "pal: %s\n", logger->error().c_str());
            return nullptr;
        }
        return std::make_unique<LinuxPlatform>(config, std::move(logger));
    } catch (...) {
        return nullptr;  // Section 3.9.1, rule 5
    }
}

}  // namespace pal
