// Logger of the AL unit tests (Section 5.3.1, "AL fakes"): records every line.
#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "pal/platform.hpp"

namespace fakes {

class FakeLogger : public pal::ILogger {
public:
    struct Line {
        pal::LogLevel level;
        std::string module;
        std::string message;
    };

    void log(pal::LogLevel level, const char* module, const std::string& message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(Line{level, module, message});
    }

    std::vector<Line> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    std::size_t count(pal::LogLevel level) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const Line& line : lines_) {
            if (line.level == level) {
                ++n;
            }
        }
        return n;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Line> lines_;
};

}  // namespace fakes
