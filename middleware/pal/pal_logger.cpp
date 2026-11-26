#include "pal_logger.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace pal {

const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

std::string format_log_time(std::chrono::system_clock::time_point time) {
    using namespace std::chrono;
    const auto since_epoch = time.time_since_epoch();
    auto seconds = duration_cast<std::chrono::seconds>(since_epoch);
    auto millis = duration_cast<milliseconds>(since_epoch - seconds).count();
    if (millis < 0) {  // times before 1970: floor, not truncation
        millis += 1000;
        seconds -= std::chrono::seconds(1);
    }
    const std::time_t t = static_cast<std::time_t>(seconds.count());
    std::tm local{};
    localtime_r(&t, &local);
    char text[40];
    const std::size_t n = std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
    std::snprintf(text + n, sizeof(text) - n, ".%03d", static_cast<int>(millis));
    return text;
}

Logger::Logger(const std::string& file_path, std::ostream* console, WallClock clock)
    : console_(console), clock_(clock ? std::move(clock) : WallClock(&std::chrono::system_clock::now)) {
    const std::filesystem::path path(file_path);
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    file_.open(file_path, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        error_ = file_path + ": cannot open the log file: " +
                 (ec ? ec.message() : std::string(std::strerror(errno != 0 ? errno : EACCES)));
    }
}

void Logger::log(LogLevel level, const char* module, const std::string& message) {
    try {
        std::string line = format_log_time(clock_());
        line += ' ';
        line += log_level_name(level);
        line += " [";
        line += module != nullptr ? module : "?";
        line += "] ";
        line += message;
        for (char& c : line) {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';  // one call, one line
        }
        line += '\n';
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_ << line;
            file_.flush();
        }
        if (console_ != nullptr) {
            *console_ << line;
            console_->flush();
        }
    } catch (...) {
        // Section 3.9.1, rule 5: no exception leaves the interface; a line that cannot be built is lost.
    }
}

}  // namespace pal
