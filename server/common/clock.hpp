// Injected clock functions of the operator side (Section 4.7).
//
// Every operator-side module reads the time only through clock functions passed to it at
// construction, so that its unit tests can use a fixed or advancing time (Section 5.3.1).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace srv {

// Wall-clock time: UNIX times of the interfaces (Section 3.1) and log time stamps.
using WallClock = std::function<std::chrono::system_clock::time_point()>;
// Monotonic time: pacing and timeouts.
using MonoClock = std::function<std::chrono::steady_clock::time_point()>;

inline WallClock system_wall_clock() {
    return [] { return std::chrono::system_clock::now(); };
}

inline MonoClock system_mono_clock() {
    return [] { return std::chrono::steady_clock::now(); };
}

// UNIX time in whole seconds (Section 3.1), rounded down.
inline int64_t unix_seconds(std::chrono::system_clock::time_point t) {
    return std::chrono::floor<std::chrono::seconds>(t.time_since_epoch()).count();
}

inline std::chrono::system_clock::time_point from_unix_seconds(int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

}  // namespace srv
