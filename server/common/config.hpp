// Configuration of the head-end and the OTT MW server: server/config.json (Section 4.7.1).
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <json.hpp>

namespace srv {

// Missing or invalid configuration or content-configuration file (ER-37). The message starts
// with the path of the file, so that the process can exit naming it.
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Reads a JSON file. Throws ConfigError "<path>: cannot open file" or "<path>: invalid JSON: …".
nlohmann::json read_json_file(const std::string& path);

// Integer value of a JSON number without fraction that fits int64_t; nullopt otherwise.
std::optional<int64_t> json_int64(const nlohmann::json& value);

// The values below are only defaults (NFR-16); a key missing in the file keeps its default,
// unknown keys are ignored (Section 3.1).
struct ServerConfig {
    int mw_port = 8080;
    int headend_port = 8081;
    std::string headend_url = "http://127.0.0.1:8081/api/v1/stream";
    std::string services_file = "content/services.json";
    std::string content_dir = "content";
    std::string log_dir = "logs";
};

// Reads server/config.json or server/config.test.json. A key with a wrong type or value (port
// outside 1 – 65535, empty string) throws ConfigError naming the file and the key.
ServerConfig load_server_config(const std::string& path);

}  // namespace srv
