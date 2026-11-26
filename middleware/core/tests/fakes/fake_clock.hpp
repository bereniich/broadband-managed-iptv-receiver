// Controllable clock of the AL unit tests (Section 5.3.1, "AL fakes"): time moves only by advance().
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

#include "pal/platform.hpp"

namespace fakes {

class FakeClock : public pal::IClock {
public:
    // Default wall clock: the worked example of Section 5.2.4.
    explicit FakeClock(std::int64_t unix_time_s = 1795000000) : start_unix_s_(unix_time_s) {}

    std::int64_t unix_time_s() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_unix_s_ + now_ms_ / 1000;
    }

    std::int64_t monotonic_ms() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return now_ms_;
    }

    TimerId start_timer(int delay_ms, TimerCallback on_expired) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const TimerId id = next_id_++;
        timers_.emplace(Key{now_ms_ + (delay_ms > 0 ? delay_ms : 0), id}, std::move(on_expired));
        return id;
    }

    void cancel_timer(TimerId id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = timers_.begin(); it != timers_.end(); ++it) {
            if (it->first.second == id) {
                timers_.erase(it);
                return;
            }
        }
    }

    // Moves the time forward by `ms`. Every timer whose deadline is reached fires once, in the order
    // of its deadline (then of its start), on the calling thread, with the time set to its deadline.
    void advance(std::int64_t ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::int64_t target = now_ms_ + ms;
        while (!timers_.empty() && timers_.begin()->first.first <= target) {
            auto it = timers_.begin();
            now_ms_ = it->first.first;
            TimerCallback callback = std::move(it->second);
            timers_.erase(it);
            lock.unlock();
            callback();
            lock.lock();
        }
        now_ms_ = target;
    }

    std::size_t pending_timers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timers_.size();
    }

private:
    using Key = std::pair<std::int64_t, TimerId>;   // (deadline in ms, timer id)

    mutable std::mutex mutex_;
    std::int64_t start_unix_s_;
    std::int64_t now_ms_ = 0;
    TimerId next_id_ = 1;
    std::map<Key, TimerCallback> timers_;
};

}  // namespace fakes
