// AL-MAIN: entry point of the DTV middleware process P2 (Sections 4.1.1 and 4.2.5; REQ-70, NFR-16).
//   build/bin/dtv-middleware --config middleware/main/middleware.json
// Start-up: configuration -> create_platform() -> AL objects (handlers registered) -> AL worker ->
// Platform::start() -> "load services". Orderly stop on SIGINT / SIGTERM: "shutdown" (POST /stop) ->
// Platform::stop() -> queue closed, worker joined, keys zeroed -> exit code 0, within 3 s.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "middleware.hpp"
#include "middleware_config.hpp"
#include "pal/platform.hpp"

namespace {

constexpr const char* kModule = "AL-MAIN";
constexpr std::chrono::milliseconds kShutdownWait{2500};   // /stop timeout 2000 ms + margin, total <= 3 s

std::atomic<bool> g_stop_requested{false};

extern "C" void on_stop_signal(int) { g_stop_requested.store(true); }   // only sets the flag

int usage() {
    std::fprintf(stderr, "usage: dtv-middleware --config <file>\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::strcmp(argv[1], "--config") != 0) return usage();

    const al::ConfigResult read = al::read_config(argv[2]);
    if (!read.ok) {
        std::fprintf(stderr, "dtv-middleware: %s\n", read.error.c_str());   // ER-37
        return 1;
    }
    const al::MiddlewareConfig& config = read.config;

    // The log directory (logs/ is not in the repository): created before the platform opens the log.
    const std::filesystem::path log_dir = std::filesystem::path(config.log_file).parent_path();
    if (!log_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);
    }

    pal::PlatformConfig platform_config;
    platform_config.ui_http_port = config.ui_http_port;
    platform_config.webui_dir = config.webui_dir;
    platform_config.hls_dir = config.hls_dir;
    platform_config.log_file = config.log_file;
    std::unique_ptr<pal::Platform> platform = pal::create_platform(platform_config);
    if (!platform) {
        std::fprintf(stderr, "dtv-middleware: cannot create the platform\n");
        return 1;
    }

    struct sigaction action {};
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    al::Middleware middleware(*platform, config);   // handlers registered before Platform::start()
    middleware.start_worker();
    if (!platform->start()) {
        platform->logger().log(pal::LogLevel::Error, kModule,
                               "Platform::start failed: port " + std::to_string(config.ui_http_port) +
                                   " or hls_dir " + config.hls_dir);
        middleware.finish();
        return 1;
    }
    platform->logger().log(pal::LogLevel::Info, kModule, "started with " + std::string(argv[2]));
    middleware.load_services();

    while (!g_stop_requested.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));

    platform->logger().log(pal::LogLevel::Info, kModule, "stop signal received");
    middleware.shutdown(kShutdownWait);
    platform->stop();   // no callback after it returns
    middleware.finish();
    return 0;
}
