#include "pal_event_loop.hpp"

#include <algorithm>
#include <string>

#include "pal_mongoose_log.hpp"
#include "pal_signals.hpp"

namespace pal {

namespace {

constexpr const char* kModule = "PAL-EVL";
// mg_wakeup needs a connection id > 0; no connection has this one, so the wake-up only ends the poll.
constexpr unsigned long kWakeId = ~0UL;

}  // namespace

EventLoop::EventLoop(ILogger* logger) : logger_(logger) {}

EventLoop::~EventLoop() { stop(); }

void EventLoop::log(LogLevel level, const std::string& message) const {
    if (logger_ != nullptr) logger_->log(level, kModule, message);
}

bool EventLoop::start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (running_) return true;
    if (stop_requested_) return false;  // a stopped loop is not restarted
    route_mongoose_log(logger_);  // before mg_mgr_init, which already logs
    mg_mgr_init(&mgr_);
    if (!mg_wakeup_init(&mgr_)) {
        log(LogLevel::Error, "cannot create the wakeup socket pair of the event loop");
        mg_mgr_free(&mgr_);
        unroute_mongoose_log(logger_);
        return false;
    }
    running_ = true;
    BlockStopSignals block;  // the loop thread inherits the blocked SIGINT / SIGTERM
    thread_ = std::thread([this] { run(); });
    thread_id_.store(thread_.get_id());
    return true;
}

bool EventLoop::running() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return running_ && !stop_requested_;
}

void EventLoop::stop() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stop_requested_ && !thread_.joinable()) return;
        stop_requested_ = true;
        if (running_) mg_wakeup(&mgr_, kWakeId, "", 0);
    }
    if (in_loop_thread()) {
        // Cannot join itself: the loop ends after the running callback; the owner joins later.
        log(LogLevel::Error, "stop() called on the event-loop thread; the loop ends after this callback");
        return;
    }
    if (thread_.joinable()) thread_.join();
    thread_id_.store(std::thread::id{});  // a later thread may get the same id
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (running_) {
        mg_mgr_free(&mgr_);
        unroute_mongoose_log(logger_);
        running_ = false;
    }
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    commands_.clear();
}

void EventLoop::wake() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (running_ && !stop_requested_) mg_wakeup(&mgr_, kWakeId, "", 0);
}

void EventLoop::post(Command command) {
    if (!command) return;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (stop_requested_) return;
        std::lock_guard<std::mutex> lock(command_mutex_);
        commands_.push_back(std::move(command));
        if (running_) mg_wakeup(&mgr_, kWakeId, "", 0);
    }
}

EventLoop::TimerId EventLoop::add_timer(int delay_ms, TimerCallback on_expired) {
    if (!on_expired) return 0;
    TimerId id;
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        id = next_timer_id_++;
        timers_[id] = Timer{Clock::now() + std::chrono::milliseconds(std::max(delay_ms, 0)), std::move(on_expired)};
    }
    wake();  // the poll timeout may have to become shorter
    return id;
}

void EventLoop::cancel_timer(TimerId id) {
    std::unique_lock<std::mutex> lock(timer_mutex_);
    timers_.erase(id);
    if (!in_loop_thread()) timer_done_.wait(lock, [this, id] { return firing_ != id; });
}

void EventLoop::run() {
    thread_id_.store(std::this_thread::get_id());  // also visible before start() has stored it
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (stop_requested_) break;
        }
        run_commands();
        run_due_timers();
        mg_mgr_poll(&mgr_, poll_timeout_ms());
    }
}

void EventLoop::run_commands() {
    std::deque<Command> batch;
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        batch.swap(commands_);
    }
    for (Command& command : batch) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (stop_requested_) return;
        }
        try {
            command(mgr_);
        } catch (...) {
            log(LogLevel::Error, "a command of the event loop threw an exception");
        }
    }
}

void EventLoop::run_due_timers() {
    for (;;) {
        TimerCallback callback;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            if (stop_requested_) return;
        }
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            const auto now = Clock::now();
            auto next = timers_.end();
            for (auto it = timers_.begin(); it != timers_.end(); ++it) {
                if (it->second.due <= now && (next == timers_.end() || it->second.due < next->second.due)) next = it;
            }
            if (next == timers_.end()) return;
            callback = std::move(next->second.on_expired);
            firing_ = next->first;
            timers_.erase(next);
        }
        try {
            callback();
        } catch (...) {
            log(LogLevel::Error, "a timer callback threw an exception");
        }
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            firing_ = 0;
        }
        timer_done_.notify_all();
    }
}

int EventLoop::poll_timeout_ms() {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    const auto now = Clock::now();
    auto timeout = std::chrono::milliseconds(kPollPeriodMs);
    for (const auto& entry : timers_) {
        // Rounded up, so that a timer never fires before its time.
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(entry.second.due - now);
        timeout = std::min(timeout, std::max(left, std::chrono::milliseconds(0)));
    }
    return static_cast<int>(timeout.count());
}

}  // namespace pal
