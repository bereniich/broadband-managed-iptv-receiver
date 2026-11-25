#include "middleware.hpp"

#include <utility>

namespace al {

namespace {

constexpr const char* kModule = "AL-MAIN";

ServiceManager::Config svc_config(const MiddlewareConfig& c) {
    ServiceManager::Config s;
    s.mw_base_url = c.mw_base_url;
    s.timeout_ms = c.mw_timeout_ms;
    s.retry_s = c.mw_retry_s;
    return s;
}

SessionController::Config ses_config(const MiddlewareConfig& c) {
    SessionController::Config s;
    s.rtp_port = c.rtp_port;
    s.client_host = c.client_host;
    s.keepalive_interval_s = c.keepalive_interval_s;
    s.no_data_timeout_s = c.no_data_timeout_s;
    s.stream_deadline_s = c.stream_deadline_s;
    return s;
}

}  // namespace

Middleware::Middleware(pal::Platform& platform, const MiddlewareConfig& config)
    : platform_(platform),
      queue_(platform.clock(), platform.logger()),
      services_(platform.http(), platform.clock(), platform.logger(), queue_, svc_config(config)),
      headend_(platform.http(), platform.logger(), queue_, config.headend_timeout_ms),
      psi_(platform.demux(), platform.logger(), queue_),
      keys_(platform.demux(), platform.logger(), queue_),
      output_(platform.hls(), platform.logger()),
      api_(platform.ui(), platform.logger(), queue_, services_, keys_),
      session_(platform.tuner(), platform.clock(), platform.logger(), queue_, services_, headend_, psi_, keys_,
               output_, ses_config(config), api_.session_listener()) {
    api_.bind(session_);
    services_.set_status_handler([this](bool reachable) { api_.on_mw_status(reachable); });
}

Middleware::~Middleware() { finish(); }

void Middleware::start_worker() {
    if (worker_.joinable()) return;
    worker_ = std::thread([this] { queue_.run(); });
}

void Middleware::load_services() {
    queue_.post(TaskKind::Control, [this] { services_.load_services(); });
}

void Middleware::request_shutdown(std::function<void()> done) {
    platform_.logger().log(pal::LogLevel::Info, kModule, "shutdown requested");
    queue_.post(TaskKind::Control, [this, done = std::move(done)] { session_.shutdown(done); });
}

bool Middleware::shutdown(std::chrono::milliseconds timeout) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_done_ = false;
    }
    request_shutdown([this] {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_done_ = true;
        cv_.notify_all();
    });
    std::unique_lock<std::mutex> lock(mutex_);
    const bool done = cv_.wait_for(lock, timeout, [this] { return shutdown_done_; });
    platform_.logger().log(done ? pal::LogLevel::Info : pal::LogLevel::Warn, kModule,
                           done ? "shutdown complete" : "shutdown not complete within the timeout");
    return done;
}

void Middleware::finish() {
    if (finished_) return;
    finished_ = true;
    queue_.close();
    if (worker_.joinable()) worker_.join();
    keys_.clear_all();   // key bytes overwritten with zeros (Section 4.2.5)
}

}  // namespace al
