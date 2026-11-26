// PAL-LOG: ILogger of IF-INT-04 (Section 3.9.2) with the line format of NFR-13.
#pragma once

#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>

#include "pal/platform.hpp"

namespace pal {

const char* log_level_name(LogLevel level);

// "<ISO 8601 local time with milliseconds>", e.g. 2026-12-03T10:15:42.317 (NFR-13).
std::string format_log_time(std::chrono::system_clock::time_point time);

class Logger : public ILogger {
public:
    using WallClock = std::function<std::chrono::system_clock::time_point()>;

    // Appends to `file_path` and creates its parent directory. Every line also goes to `console`
    // (nullptr: file only). If the file cannot be opened, ok() is false, error() names the file
    // and the lines still go to the console.
    explicit Logger(const std::string& file_path, std::ostream* console = nullptr, WallClock clock = nullptr);

    bool ok() const { return file_.is_open(); }
    const std::string& error() const { return error_; }

    // Thread-safe; one call writes exactly one complete line (control characters become '?').
    void log(LogLevel level, const char* module, const std::string& message) override;

private:
    std::mutex mutex_;
    std::ofstream file_;
    std::ostream* console_;
    WallClock clock_;
    std::string error_;
};

}  // namespace pal
