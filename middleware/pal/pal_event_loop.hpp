// PAL event loop (Section 4.5.2): one thread runs mg_mgr_poll and is the only user of the Mongoose
// manager. Other threads hand work to it through a mutex-protected command queue and wake it with
// mg_wakeup. The one-shot timers of PAL-CLK run on the same thread (Section 3.9.1, rule 1).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "mongoose.h"
#include "pal/platform.hpp"

namespace pal {

class EventLoop {
public:
    using Command = std::function<void(mg_mgr& mgr)>;
    using TimerId = std::uint64_t;
    using TimerCallback = std::function<void()>;

    static constexpr int kPollPeriodMs = 10;  // mg_mgr_poll period (Section 4.5.2)

    explicit EventLoop(ILogger* logger);
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Initializes the Mongoose manager and starts the thread (with SIGINT / SIGTERM blocked).
    // Returns false if the manager or its wakeup socket pair cannot be created.
    bool start();
    // Ends the thread; afterwards no command, timer or Mongoose callback runs. Idempotent.
    void stop();
    bool running() const;
    bool in_loop_thread() const { return std::this_thread::get_id() == thread_id_.load(); }

    // Executes `command` on the loop thread, in the order of posting. Commands posted before start()
    // run after it; commands posted after stop() are dropped.
    void post(Command command);

    // One-shot timer: `on_expired` runs once on the loop thread, not earlier than `delay_ms`.
    // Returns 0 for an empty callback.
    TimerId add_timer(int delay_ms, TimerCallback on_expired);
    // After it returns, the callback is never called (it waits for a callback that is running on
    // another thread; from the loop thread itself it does not wait).
    void cancel_timer(TimerId id);

private:
    using Clock = std::chrono::steady_clock;
    struct Timer {
        Clock::time_point due;
        TimerCallback on_expired;
    };

    void run();
    void run_commands();
    void run_due_timers();
    int poll_timeout_ms();
    void wake();
    void log(LogLevel level, const std::string& message) const;

    ILogger* logger_;
    mg_mgr mgr_{};

    mutable std::mutex state_mutex_;  // running_, mgr_ initialized, wake-ups
    bool running_ = false;
    bool stop_requested_ = false;
    std::thread thread_;
    std::atomic<std::thread::id> thread_id_{};

    std::mutex command_mutex_;
    std::deque<Command> commands_;

    std::mutex timer_mutex_;
    std::condition_variable timer_done_;
    std::map<TimerId, Timer> timers_;
    TimerId next_timer_id_ = 1;
    TimerId firing_ = 0;  // timer whose callback is running (0 = none)
};

}  // namespace pal
