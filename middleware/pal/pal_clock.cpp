#include "pal_clock.hpp"

#include <chrono>

namespace pal {

std::int64_t Clock::unix_time_s() const {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::int64_t Clock::monotonic_ms() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

IClock::TimerId Clock::start_timer(int delay_ms, TimerCallback on_expired) {
    return loop_.add_timer(delay_ms, std::move(on_expired));
}

void Clock::cancel_timer(TimerId id) { loop_.cancel_timer(id); }

}  // namespace pal
