// A Platform from create_platform with a test configuration (Section 5.3.3, TST-221 – TST-226):
// ephemeral UI port, temporary webui_dir, hls_dir and log file.
#pragma once

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "pal_platform.hpp"
#include "test_util.hpp"

namespace testutil {

class TestPlatform {
public:
    TestPlatform() {
        config.ui_http_port = 0;
        config.webui_dir = (dir.path() / "webui").string();
        config.hls_dir = (dir.path() / "hls").string();
        config.log_file = (dir.path() / "logs" / "middleware.log").string();
        std::filesystem::create_directories(config.webui_dir);
    }
    // Creates the platform (after the test has prepared the directories).
    pal::Platform& create() {
        platform = pal::create_platform(config);
        if (!platform) throw std::runtime_error("create_platform returned nullptr");
        return *platform;
    }
    pal::LinuxPlatform& linux_platform() { return dynamic_cast<pal::LinuxPlatform&>(*platform); }
    std::string log_text() const {
        std::ifstream in(config.log_file);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }
    void write_web_file(const std::string& relative, const std::string& text) const {
        const auto path = std::filesystem::path(config.webui_dir) / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    TempDir dir;
    pal::PlatformConfig config;
    std::unique_ptr<pal::Platform> platform;
};

}  // namespace testutil
