#include "config.hpp"

#include <fstream>
#include <limits>
#include <sstream>

namespace srv {

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw ConfigError(path + ": cannot open file");
    std::ostringstream text;
    text << in.rdbuf();
    try {
        return nlohmann::json::parse(text.str());
    } catch (const nlohmann::json::exception& e) {
        throw ConfigError(path + ": invalid JSON: " + e.what());
    }
}

std::optional<int64_t> json_int64(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        const auto u = value.get<uint64_t>();
        if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
        return static_cast<int64_t>(u);
    }
    if (value.is_number_integer()) return value.get<int64_t>();
    return std::nullopt;
}

namespace {

void read_port(const nlohmann::json& root, const char* key, const std::string& path, int& out) {
    const auto it = root.find(key);
    if (it == root.end()) return;
    const auto value = json_int64(*it);
    if (!value || *value < 1 || *value > 65535) {
        throw ConfigError(path + ": \"" + key + "\" must be an integer port number 1 - 65535");
    }
    out = static_cast<int>(*value);
}

void read_string(const nlohmann::json& root, const char* key, const std::string& path,
                 std::string& out) {
    const auto it = root.find(key);
    if (it == root.end()) return;
    if (!it->is_string() || it->get<std::string>().empty()) {
        throw ConfigError(path + ": \"" + key + "\" must be a non-empty string");
    }
    out = it->get<std::string>();
}

}  // namespace

ServerConfig load_server_config(const std::string& path) {
    const nlohmann::json root = read_json_file(path);
    if (!root.is_object()) throw ConfigError(path + ": the top level must be a JSON object");
    ServerConfig config;
    read_port(root, "mw_port", path, config.mw_port);
    read_port(root, "headend_port", path, config.headend_port);
    read_string(root, "headend_url", path, config.headend_url);
    read_string(root, "services_file", path, config.services_file);
    read_string(root, "content_dir", path, config.content_dir);
    read_string(root, "log_dir", path, config.log_dir);
    return config;
}

}  // namespace srv
