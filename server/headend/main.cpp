// Process P3, head-end (SRV-HEC, SRV-ARC, SRV-RTP; Sections 3.3, 3.4, 4.7.4 – 4.7.6).
//
// Usage: build/bin/headend [--config <file>]      (default server/config.json)
// Run from the repository root (Section 4.1.1). Exits with code 1 and a message naming the file if
// the configuration or services.json is missing or invalid (ER-37), and naming the service and the
// file if a content file is missing or its duration differs from services.json (ER-38). Stops on
// SIGINT / SIGTERM.
#include <csignal>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "archive.hpp"
#include "config.hpp"
#include "head_end.hpp"
#include "http.hpp"
#include "log.hpp"
#include "services.hpp"

namespace {

constexpr const char* kModule = "SRV-HEC";

int usage() {
    std::fprintf(stderr, "usage: headend [--config <file>]   (default server/config.json)\n");
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

    // SIGINT and SIGTERM are taken by sigwait() below; blocked before any thread is created.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    srv::ServerConfig config;
    try {
        config = srv::load_server_config(config_path);
    } catch (const srv::ConfigError& e) {
        std::fprintf(stderr, "headend: %s\n", e.what());
        return 1;
    }

    std::unique_ptr<srv::Logger> logger;
    try {
        logger = std::make_unique<srv::Logger>(config.log_dir + "/headend.log", &std::cerr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "headend: %s\n", e.what());
        return 1;
    }

    srv::ServiceCatalog catalog;
    try {
        catalog = srv::load_services(config.services_file);
    } catch (const srv::ConfigError& e) {
        logger->error(kModule, e.what());
        return 1;
    }

    // SRV-ARC: PCR index of every content file (Section 4.7.4, ER-38).
    std::map<std::string, std::shared_ptr<const srv::Archive>> archives;
    for (const srv::Service& s : catalog.services) {
        const std::string path = config.content_dir + "/" + s.file;
        try {
            auto archive = srv::Archive::load(s, path);
            char text[64];
            std::snprintf(text, sizeof(text), "%.3f", archive->measured_duration_s());
            logger->info("SRV-ARC", "loaded service=" + s.service_id + " file=" + path + " pmt_pid=" +
                                        std::to_string(archive->pmt_pid()) + " pcr_pid=" +
                                        std::to_string(archive->pcr_pid()) + " pcr_entries=" +
                                        std::to_string(archive->index().size()) + " duration=" + text + " s");
            archives[s.service_id] = std::move(archive);
        } catch (const srv::ArchiveError& e) {
            logger->error("SRV-ARC", e.what());
            return 1;
        }
    }

    std::unique_ptr<srv::HeadEnd> head_end;
    try {
        head_end = std::make_unique<srv::HeadEnd>(std::move(catalog), std::move(archives), srv::HeadEndOptions{},
                                                  logger.get());
    } catch (const std::exception& e) {
        logger->error(kModule, e.what());
        return 1;
    }

    srv::route_mongoose_log(logger.get(), kModule);
    srv::HttpServer server([&head_end](const srv::HttpRequest& r) { return head_end->handle(r); }, logger.get(),
                           kModule);
    std::string error;
    if (!server.start(config.headend_port, &error)) {
        logger->error(kModule, error);
        return 1;
    }
    logger->info(kModule, "listening on 127.0.0.1:" + std::to_string(server.port()) + " config=" + config_path +
                              " services_file=" + config.services_file);

    int signal_number = 0;
    sigwait(&signals, &signal_number);
    server.stop();
    head_end->shutdown();
    logger->info(kModule, std::string("stopped by ") + (signal_number == SIGINT ? "SIGINT" : "SIGTERM"));
    srv::route_mongoose_log(nullptr, kModule);
    return 0;
}
