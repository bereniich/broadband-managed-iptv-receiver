// PAL-CLK: IClock of IF-INT-04 (Section 3.9.2) over C++17 <chrono> and the PAL event loop.
#pragma once

#include "pal/platform.hpp"
#include "pal_event_loop.hpp"

namespace pal {

class Clock : public IClock {
public:
    explicit Clock(EventLoop& loop) : loop_(loop) {}

    std::int64_t unix_time_s() const override;   // std::chrono::system_clock, UTC
    std::int64_t monotonic_ms() const override;  // std::chrono::steady_clock
    // The callback runs on the event-loop thread (Section 3.9.1, rule 1); 0 for an empty callback.
    TimerId start_timer(int delay_ms, TimerCallback on_expired) override;
    void cancel_timer(TimerId id) override;

private:
    EventLoop& loop_;
};

}  // namespace pal
