// Logging of the operator-side programs in the line format of NFR-13:
//   <ISO 8601 local time with milliseconds> <LEVEL> [<module>] <message>
// e.g. "2026-12-03T10:15:42.317 INFO [SRV-MW] listening on 127.0.0.1:8080".
#pragma once

#include <chrono>
#include <fstream>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

#include "clock.hpp"

namespace srv {

enum class LogLevel { Debug, Info, Warn, Error };

const char* log_level_name(LogLevel level);

// Time stamp of NFR-13: local time with milliseconds, e.g. "2026-12-03T10:15:42.317".
std::string format_log_time(std::chrono::system_clock::time_point t);

// Thread-safe logger. Every line is written whole and flushed, to the log file and to an
// optional console stream. Control characters in the message (for example a line break taken
// from a request) are replaced by '?', so that one call always gives exactly one line.
class Logger {
public:
    // Opens `file_path` for appending and creates its parent directory if needed; an empty
    // path writes to the console only. Throws std::runtime_error naming the file on failure.
    explicit Logger(const std::string& file_path, std::ostream* console = nullptr,
                    WallClock clock = system_wall_clock());

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, std::string_view module, std::string_view message);
    void debug(std::string_view module, std::string_view message) { log(LogLevel::Debug, module, message); }
    void info(std::string_view module, std::string_view message) { log(LogLevel::Info, module, message); }
    void warn(std::string_view module, std::string_view message) { log(LogLevel::Warn, module, message); }
    void error(std::string_view module, std::string_view message) { log(LogLevel::Error, module, message); }

    const std::string& file_path() const { return file_path_; }

private:
    std::string file_path_;
    std::ostream* console_;
    WallClock clock_;
    std::mutex mutex_;
    std::ofstream file_;
};

}  // namespace srv
