#include "task_queue.hpp"

#include <string>
#include <utility>

namespace al {

namespace {
constexpr const char* kModule = "AL-MAIN";
}

TaskQueue::TaskQueue(pal::IClock& clock, pal::ILogger& logger) : clock_(clock), logger_(logger) {}

bool TaskQueue::post(TaskKind kind, Task task) {
    std::uint64_t dropped_total = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        if (kind == TaskKind::Data && data_count_ >= kMaxDataTasks) {
            ++dropped_;
            // The clock is read only on a drop, so a normal post costs no clock call.
            const std::int64_t now_ms = clock_.monotonic_ms();
            if (warned_ && now_ms - last_warning_ms_ < kDropWarningIntervalMs) {
                return false;
            }
            warned_ = true;
            last_warning_ms_ = now_ms;
            dropped_total = dropped_;
        } else {
            entries_.push_back(Entry{kind, std::move(task)});
            if (kind == TaskKind::Data) {
                ++data_count_;
            }
            ready_.notify_one();
            return true;
        }
    }
    // Logged without holding the mutex, so that the worker is not blocked by the log write.
    logger_.log(pal::LogLevel::Warn, kModule,
                "task queue full: dropped data tasks total=" + std::to_string(dropped_total));
    return false;
}

void TaskQueue::run() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return closed_ || !entries_.empty(); });
            if (closed_) {
                return;
            }
            task = pop_locked();
        }
        task();
    }
}

std::size_t TaskQueue::run_pending() {
    std::size_t executed = 0;
    for (;;) {
        Task task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || entries_.empty()) {
                return executed;
            }
            task = pop_locked();
        }
        task();
        ++executed;
    }
}

void TaskQueue::close() {
    std::deque<Entry> discarded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        discarded.swap(entries_);
        data_count_ = 0;
        ready_.notify_all();
    }
    // `discarded` is destroyed here, outside the mutex (the tasks may own large captures).
}

std::uint64_t TaskQueue::dropped_data_tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

std::size_t TaskQueue::queued_data_tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_count_;
}

std::size_t TaskQueue::queued_control_tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size() - data_count_;
}

TaskQueue::Task TaskQueue::pop_locked() {
    Entry entry = std::move(entries_.front());
    entries_.pop_front();
    if (entry.kind == TaskKind::Data) {
        --data_count_;
    }
    return std::move(entry.task);
}

}  // namespace al
