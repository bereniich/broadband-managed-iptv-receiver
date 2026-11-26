#include "log.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace srv {

const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

std::string format_log_time(std::chrono::system_clock::time_point t) {
    using namespace std::chrono;
    const auto since_epoch = t.time_since_epoch();
    const auto secs = floor<seconds>(since_epoch);
    const auto millis = duration_cast<milliseconds>(since_epoch - secs).count();
    const std::time_t tt = static_cast<std::time_t>(secs.count());
    std::tm local{};
    localtime_r(&tt, &local);
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local);
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03d", date, static_cast<int>(millis));
    return out;
}

Logger::Logger(const std::string& file_path, std::ostream* console, WallClock clock)
    : file_path_(file_path), console_(console), clock_(std::move(clock)) {
    if (file_path_.empty()) return;
    const std::filesystem::path parent = std::filesystem::path(file_path_).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error("cannot create log directory " + parent.string() + " for " +
                                     file_path_ + ": " + ec.message());
        }
    }
    file_.open(file_path_, std::ios::out | std::ios::app);
    if (!file_.is_open()) throw std::runtime_error("cannot open log file " + file_path_);
}

void Logger::log(LogLevel level, std::string_view module, std::string_view message) {
    std::string line = format_log_time(clock_());
    line += ' ';
    line += log_level_name(level);
    line += " [";
    line += module;
    line += "] ";
    for (const char ch : message) {
        const auto c = static_cast<unsigned char>(ch);
        line += (c < 0x20 || c == 0x7F) ? '?' : ch;
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
}

}  // namespace srv
