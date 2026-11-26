// AL task queue (Section 4.2.2, buffer B4 of Section 4.5.4).
// All AL work runs on the AL worker thread; PAL callbacks only post tasks (rule T1). One FIFO
// keeps the order of all control and data tasks (rule T3). Data tasks are bounded; control tasks
// are never dropped. In test mode the worker is not started and the test calls run_pending() (T5).
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

#include "pal/platform.hpp"

namespace al {

enum class TaskKind {
    Control,   // UI messages, HTTP responses, timers, tuner status: never dropped
    Data       // one PSI section or one TS packet: bounded, dropped when the bound is reached
};

class TaskQueue {
public:
    using Task = std::function<void()>;

    static constexpr std::size_t kMaxDataTasks = 8192;            // B4
    static constexpr std::int64_t kDropWarningIntervalMs = 1000;  // B4, ER-27: at most once per second

    // The clock is used only to rate-limit the drop warning.
    TaskQueue(pal::IClock& clock, pal::ILogger& logger);

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // May be called from any thread. Returns false if the task was not queued: a data task while
    // kMaxDataTasks data tasks are waiting (counted as dropped), or any task after close().
    bool post(TaskKind kind, Task task);

    // AL worker loop: executes the tasks one after the other in FIFO order until close().
    void run();

    // Test mode (rule T5): executes tasks on the calling thread until the queue is empty,
    // including tasks posted by the executed tasks. Must not be used while run() is active.
    // Returns the number of executed tasks.
    std::size_t run_pending();

    // Makes run() return after the current task; queued tasks are destroyed without being
    // executed, later posts are discarded.
    void close();

    std::uint64_t dropped_data_tasks() const;
    std::size_t queued_data_tasks() const;
    std::size_t queued_control_tasks() const;

private:
    struct Entry {
        TaskKind kind;
        Task task;
    };

    // Removes the first entry; the mutex must be held and the queue must not be empty.
    Task pop_locked();

    pal::IClock& clock_;
    pal::ILogger& logger_;

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Entry> entries_;
    std::size_t data_count_ = 0;
    std::uint64_t dropped_ = 0;
    bool warned_ = false;
    std::int64_t last_warning_ms_ = 0;
    bool closed_ = false;
};

}  // namespace al
