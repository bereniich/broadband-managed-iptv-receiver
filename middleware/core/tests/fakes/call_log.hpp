// Chronological record of the interface calls of all fakes, so that a test can check the order of
// calls across interfaces (for example untune → tune → open_session → PAT filter → POST /start).
#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace fakes {

class CallLog {
public:
    void add(const std::string& entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(entry);
    }

    std::vector<std::string> entries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    // Index of the first entry that starts with `prefix`, or npos.
    std::size_t find(const std::string& prefix) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].compare(0, prefix.size(), prefix) == 0) return i;
        }
        return npos;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    mutable std::mutex mutex_;
    std::vector<std::string> entries_;
};

// Adds to `log` if it is set.
inline void record(CallLog* log, const std::string& entry) {
    if (log != nullptr) log->add(entry);
}

}  // namespace fakes
