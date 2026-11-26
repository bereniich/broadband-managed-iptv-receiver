// Process P4, OTT MW server (SRV-MW, Sections 3.2 and 4.7.7).
//
// Usage: build/bin/mw_server [--config <file>]      (default server/config.json)
// Run from the repository root (Section 4.1.1). Exits with code 1 and a message naming the file
// if the configuration or services.json is missing or invalid (ER-37); stops on SIGINT / SIGTERM.
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include "config.hpp"
#include "http.hpp"
#include "log.hpp"
#include "mw_api.hpp"
#include "services.hpp"

namespace {

constexpr const char* kModule = "SRV-MW";

int usage() {
    std::fprintf(stderr, "usage: mw_server [--config <file>]   (default server/config.json)\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "server/config.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            return usage();
        }
    }

    // SIGINT and SIGTERM are taken by sigwait() below; blocked before any thread is created, so
    // that the server threads inherit the mask.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    srv::ServerConfig config;
    try {
        config = srv::load_server_config(config_path);
    } catch (const srv::ConfigError& e) {
        std::fprintf(stderr, "mw_server: %s\n", e.what());
        return 1;
    }

    std::unique_ptr<srv::Logger> logger;
    try {
        logger = std::make_unique<srv::Logger>(config.log_dir + "/mw_server.log", &std::cerr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mw_server: %s\n", e.what());
        return 1;
    }

    srv::ServiceCatalog catalog;
    try {
        catalog = srv::load_services(config.services_file);
    } catch (const srv::ConfigError& e) {
        logger->error(kModule, e.what());
        return 1;
    }
    const size_t service_count = catalog.services.size();

    srv::route_mongoose_log(logger.get(), kModule);
    const srv::MwApi api(std::move(catalog), config.headend_url, srv::system_wall_clock(), logger.get());
    srv::HttpServer server([&api](const srv::HttpRequest& r) { return api.handle(r); }, logger.get(), kModule);
    std::string error;
    if (!server.start(config.mw_port, &error)) {
        logger->error(kModule, error);
        return 1;
    }
    logger->info(kModule, "listening on 127.0.0.1:" + std::to_string(server.port()) + " config=" + config_path +
                              " services_file=" + config.services_file + " services=" +
                              std::to_string(service_count));

    int signal_number = 0;
    sigwait(&signals, &signal_number);
    server.stop();
    logger->info(kModule, std::string("stopped by ") + (signal_number == SIGINT ? "SIGINT" : "SIGTERM"));
    srv::route_mongoose_log(nullptr, kModule);
    return 0;
}
