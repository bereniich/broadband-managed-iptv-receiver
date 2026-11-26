// AL-MAIN configuration of the middleware process (Section 4.2.5; NFR-16, ER-37). Every key has the default
// of Section 4.2.5, which is also the value in middleware/main/middleware.json; missing keys take the
// default, unknown keys are ignored, a key with a wrong type or value makes the file invalid.
#pragma once

#include <cstdint>
#include <string>

namespace al {

struct MiddlewareConfig {
    std::string mw_base_url = "http://127.0.0.1:8080/api/v1";
    std::uint16_t rtp_port = 5004;
    std::string client_host = "127.0.0.1";
    std::uint16_t ui_http_port = 8090;
    std::string webui_dir = "webui";
    std::string hls_dir = "/tmp/m15-hls";
    std::string log_file = "logs/middleware.log";
    int mw_timeout_ms = 800;
    int headend_timeout_ms = 2000;
    int mw_retry_s = 5;
    int keepalive_interval_s = 5;
    int keepalive_loss_s = 15;
    int no_data_timeout_s = 5;
    int stream_deadline_s = 3;
};

struct ConfigResult {
    bool ok = false;
    MiddlewareConfig config;
    std::string error;   // names the file (and the key), e.g. "invalid JSON in middleware.json"
};

ConfigResult read_config(const std::string& path);

}  // namespace al
