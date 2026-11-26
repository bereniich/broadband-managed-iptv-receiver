#include "middleware_config.hpp"

#include <fstream>
#include <sstream>

#include <json.hpp>

namespace al {

namespace {

using nlohmann::json;

// Reads `key` into `out` if present; false if the value has the wrong type or range.
bool read_string(const json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end()) return true;
    if (!it->is_string() || it->get<std::string>().empty()) return false;
    out = it->get<std::string>();
    return true;
}

bool read_port(const json& j, const char* key, std::uint16_t& out) {
    auto it = j.find(key);
    if (it == j.end()) return true;
    if (!it->is_number_integer()) return false;
    const std::int64_t v = it->get<std::int64_t>();
    if (v < 1 || v > 65535) return false;
    out = static_cast<std::uint16_t>(v);
    return true;
}

bool read_positive(const json& j, const char* key, int& out) {
    auto it = j.find(key);
    if (it == j.end()) return true;
    if (!it->is_number_integer()) return false;
    const std::int64_t v = it->get<std::int64_t>();
    if (v <= 0 || v > 3600000) return false;
    out = static_cast<int>(v);
    return true;
}

}  // namespace

ConfigResult read_config(const std::string& path) {
    ConfigResult result;
    std::ifstream file(path);
    if (!file) {
        result.error = "cannot read the configuration file " + path;
        return result;
    }
    std::stringstream text;
    text << file.rdbuf();
    const json j = json::parse(text.str(), nullptr, false);
    if (!j.is_object()) {
        result.error = "invalid JSON in the configuration file " + path;
        return result;
    }

    MiddlewareConfig& c = result.config;
    const auto invalid = [&](const char* key) {
        result.error = std::string("invalid value of ") + key + " in the configuration file " + path;
        return result;
    };
    if (!read_string(j, "mw_base_url", c.mw_base_url)) return invalid("mw_base_url");
    if (!read_port(j, "rtp_port", c.rtp_port)) return invalid("rtp_port");
    if (!read_string(j, "client_host", c.client_host)) return invalid("client_host");
    if (!read_port(j, "ui_http_port", c.ui_http_port)) return invalid("ui_http_port");
    if (!read_string(j, "webui_dir", c.webui_dir)) return invalid("webui_dir");
    if (!read_string(j, "hls_dir", c.hls_dir)) return invalid("hls_dir");
    if (!read_string(j, "log_file", c.log_file)) return invalid("log_file");
    if (!read_positive(j, "mw_timeout_ms", c.mw_timeout_ms)) return invalid("mw_timeout_ms");
    if (!read_positive(j, "headend_timeout_ms", c.headend_timeout_ms)) return invalid("headend_timeout_ms");
    if (!read_positive(j, "mw_retry_s", c.mw_retry_s)) return invalid("mw_retry_s");
    if (!read_positive(j, "keepalive_interval_s", c.keepalive_interval_s)) return invalid("keepalive_interval_s");
    if (!read_positive(j, "keepalive_loss_s", c.keepalive_loss_s)) return invalid("keepalive_loss_s");
    if (!read_positive(j, "no_data_timeout_s", c.no_data_timeout_s)) return invalid("no_data_timeout_s");
    if (!read_positive(j, "stream_deadline_s", c.stream_deadline_s)) return invalid("stream_deadline_s");
    result.ok = true;
    return result;
}

}  // namespace al
