// Test utilities of the PAL unit tests (Section 5.3.1): a logger that records lines, temporary
// directories, waiting for a condition.
#pragma once

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pal/platform.hpp"

namespace testutil {

struct LogLine {
    pal::LogLevel level;
    std::string module;
    std::string message;
};

class RecordingLogger : public pal::ILogger {
public:
    void log(pal::LogLevel level, const char* module, const std::string& message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back({level, module != nullptr ? module : "", message});
    }
    std::vector<LogLine> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }
    // Number of lines of `level` from `module` whose message contains `text`.
    int count(pal::LogLevel level, const std::string& module, const std::string& text) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const LogLine& l : lines_) {
            if (l.level == level && l.module == module && l.message.find(text) != std::string::npos) ++n;
        }
        return n;
    }

private:
    mutable std::mutex mutex_;
    std::vector<LogLine> lines_;
};

class TempDir {
public:
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() / "m15-pal-test-XXXXXX").string();
        if (mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = pattern;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all, ec);
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline bool wait_until(const std::function<bool()>& condition,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
}

// True if SIGINT and SIGTERM are blocked on the calling thread.
inline bool stop_signals_blocked() {
    sigset_t current;
    pthread_sigmask(SIG_BLOCK, nullptr, &current);
    return sigismember(&current, SIGINT) == 1 && sigismember(&current, SIGTERM) == 1;
}

}  // namespace testutil
