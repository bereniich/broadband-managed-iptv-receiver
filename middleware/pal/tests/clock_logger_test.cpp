// Unit tests of PAL-CLK, PAL-LOG and the PAL event loop (Section 5.3.3): TST-225; the event-loop
// part of TST-226 (the platform part follows with create_platform).
#include <gtest/gtest.h>

#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mongoose.h"
#include "pal_clock.hpp"
#include "pal_event_loop.hpp"
#include "pal_logger.hpp"
#include "test_util.hpp"

namespace {

using pal::Clock;
using pal::EventLoop;
using pal::LogLevel;
using std::chrono::milliseconds;
using std::chrono::steady_clock;
using testutil::RecordingLogger;
using testutil::TempDir;
using testutil::wait_until;

double ms_between(steady_clock::time_point a, steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

// ---------------------------------------------------------------------------------------- TST-225

// Step 1: timers of 50, 100 and 1 000 ms fire once, not earlier, at most 20 ms late; a cancelled one
// never fires. All run on the event-loop thread, which has SIGINT and SIGTERM blocked.
TEST(Tst225, TimersFireOnceNotEarlyAtMost20msLateAndTheCancelledOneNever) {
    RecordingLogger log;
    EventLoop loop(&log);
    ASSERT_TRUE(loop.start());
    Clock clock(loop);

    struct Fired {
        std::atomic<int> count{0};
        steady_clock::time_point at{};
        std::thread::id thread{};
        bool signals_blocked = false;
    };
    std::map<int, Fired> fired;
    for (int delay : {50, 100, 1000, 200}) fired[delay];
    std::map<int, steady_clock::time_point> started;
    std::map<int, pal::IClock::TimerId> ids;
    for (int delay : {50, 100, 1000, 200}) {
        Fired& f = fired[delay];
        started[delay] = steady_clock::now();
        ids[delay] = clock.start_timer(delay, [&f] {
            f.at = steady_clock::now();
            f.thread = std::this_thread::get_id();
            f.signals_blocked = testutil::stop_signals_blocked();
            ++f.count;
        });
        EXPECT_NE(ids[delay], 0u);
    }
    clock.cancel_timer(ids[200]);
    std::this_thread::sleep_for(milliseconds(1300));

    for (int delay : {50, 100, 1000}) {
        const Fired& f = fired[delay];
        ASSERT_EQ(f.count.load(), 1) << delay << " ms";
        const double elapsed = ms_between(started[delay], f.at);
        std::printf("TST-225 measured: %d ms timer fired after %.2f ms (late by %.2f ms, limit 0 - 20 ms)\n", delay,
                    elapsed, elapsed - delay);
        EXPECT_GE(elapsed, delay) << delay << " ms timer fired early";
        EXPECT_LE(elapsed, delay + 20) << delay << " ms timer fired late";
        EXPECT_NE(f.thread, std::this_thread::get_id());
        EXPECT_EQ(f.thread, fired[50].thread);  // one event-loop thread
        EXPECT_TRUE(f.signals_blocked);
    }
    EXPECT_EQ(fired[200].count.load(), 0);
    EXPECT_EQ(clock.start_timer(10, nullptr), 0u);
    loop.stop();
}

// Rule 4 of Section 3.9.1 for timers: cancel_timer() from another thread returns only after a callback
// that is already running has finished; from a callback it does not wait (no deadlock); a callback can
// cancel other timers and start new ones.
TEST(Tst225, CancelWaitsForARunningCallbackAndWorksFromACallback) {
    RecordingLogger log;
    EventLoop loop(&log);
    ASSERT_TRUE(loop.start());
    Clock clock(loop);

    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    const auto slow = clock.start_timer(10, [&] {
        entered = true;
        std::this_thread::sleep_for(milliseconds(150));
        finished = true;
    });
    ASSERT_TRUE(wait_until([&] { return entered.load(); }));
    clock.cancel_timer(slow);
    EXPECT_TRUE(finished.load()) << "cancel_timer returned while the callback was running";

    std::atomic<int> other_fired{0};
    std::atomic<int> rearmed{0};
    std::atomic<int> self_fired{0};
    const auto other = clock.start_timer(100, [&] { ++other_fired; });
    pal::IClock::TimerId self = 0;
    std::atomic<bool> self_set{false};
    self = clock.start_timer(20, [&] {
        while (!self_set) std::this_thread::yield();
        ++self_fired;
        clock.cancel_timer(self);   // its own id, from the loop thread: no wait
        clock.cancel_timer(other);  // a pending one: never fires
        clock.start_timer(10, [&] { ++rearmed; });
    });
    self_set = true;
    std::this_thread::sleep_for(milliseconds(250));
    EXPECT_EQ(self_fired.load(), 1);
    EXPECT_EQ(other_fired.load(), 0);
    EXPECT_EQ(rearmed.load(), 1);
    loop.stop();
}

TEST(Tst225, ClockReadsUtcWallTimeAndMonotonicMilliseconds) {
    RecordingLogger log;
    EventLoop loop(&log);
    Clock clock(loop);
    const std::int64_t wall = static_cast<std::int64_t>(std::time(nullptr));
    EXPECT_LE(std::llabs(clock.unix_time_s() - wall), 1);
    const std::int64_t m0 = clock.monotonic_ms();
    std::this_thread::sleep_for(milliseconds(50));
    const std::int64_t m1 = clock.monotonic_ms();
    EXPECT_GE(m1 - m0, 50);
    EXPECT_LT(m1 - m0, 1000);
}

// Step 2: 4 threads x 1 000 lines give 4 000 complete lines in the NFR-13 format, in the file and on
// the console; no line is mixed with another.
TEST(Tst225, FourThreadsWrite4000CompleteLines) {
    TempDir dir;
    const auto file = dir.path() / "logs" / "middleware.log";
    std::ostringstream console;
    {
        pal::Logger logger(file.string(), &console);
        ASSERT_TRUE(logger.ok()) << logger.error();
        const LogLevel levels[] = {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&logger, &levels, t] {
                const std::string padding(150, static_cast<char>('a' + t));
                for (int i = 0; i < 1000; ++i) {
                    logger.log(levels[(t + i) % 4], t % 2 == 0 ? "AL-SES" : "PAL-WS",
                               "thread=" + std::to_string(t) + " line=" + std::to_string(i) + " " + padding + " end");
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    const std::regex format(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3} (DEBUG|INFO|WARN|ERROR) \[[^\]]+\] )");
    const std::regex whole(R"(thread=(\d) line=(\d+) ([a-d])\3{149} end$)");
    for (const std::vector<std::string>& lines :
         {read_lines(file), [&] {
              std::istringstream in(console.str());
              std::vector<std::string> v;
              for (std::string l; std::getline(in, l);) v.push_back(l);
              return v;
          }()}) {
        ASSERT_EQ(lines.size(), 4000u);
        std::set<std::pair<int, int>> seen;
        for (const std::string& line : lines) {
            ASSERT_TRUE(std::regex_search(line, format)) << line;
            std::smatch m;
            ASSERT_TRUE(std::regex_search(line, m, whole)) << line;
            EXPECT_EQ(m[3].str()[0], 'a' + std::stoi(m[1].str()));
            seen.insert({std::stoi(m[1].str()), std::stoi(m[2].str())});
        }
        EXPECT_EQ(seen.size(), 4000u);
    }
}

// NFR-13 details: local time with milliseconds (the example of Section 5.2.4 in UTC), one call gives
// one line (control characters become '?'), the parent directory is created, lines are appended, and a
// log file that cannot be opened is reported while the console still gets the lines.
TEST(Tst225, LoggerFormatAppendsCreatesItsDirectoryAndReportsAnUnopenableFile) {
    const char* old_tz = std::getenv("TZ");
    const std::string saved = old_tz != nullptr ? old_tz : "";
    setenv("TZ", "UTC", 1);
    tzset();

    TempDir dir;
    const auto file = dir.path() / "a" / "b" / "middleware.log";
    const auto fixed = [] {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(1795000000317LL));
    };
    {
        pal::Logger logger(file.string(), nullptr, fixed);
        ASSERT_TRUE(logger.ok());
        logger.log(LogLevel::Info, "AL-API", "recv command id=7\nname=play\tx");
    }
    {
        pal::Logger logger(file.string(), nullptr, fixed);
        logger.log(LogLevel::Warn, nullptr, "second");
    }
    const auto lines = read_lines(file);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "2026-11-18T11:06:40.317 INFO [AL-API] recv command id=7?name=play?x");
    EXPECT_EQ(lines[1], "2026-11-18T11:06:40.317 WARN [?] second");

    std::ofstream(dir.path() / "plain-file") << "x";
    std::ostringstream console;
    pal::Logger bad((dir.path() / "plain-file" / "middleware.log").string(), &console, fixed);
    EXPECT_FALSE(bad.ok());
    EXPECT_NE(bad.error().find("plain-file/middleware.log"), std::string::npos) << bad.error();
    bad.log(LogLevel::Error, "PAL-LOG", "still on the console");
    EXPECT_EQ(console.str(), "2026-11-18T11:06:40.317 ERROR [PAL-LOG] still on the console\n");

    if (old_tz != nullptr) {
        setenv("TZ", saved.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
}

// ---------------------------------------------------------------------------------------- TST-226

// The event loop runs commands in the order of posting (also those posted before start()), only Mongoose
// errors are logged, and after stop() returns no command or timer callback runs any more.
TEST(Tst226, EventLoopRunsNoCommandOrTimerAfterStop) {
    RecordingLogger log;
    EventLoop loop(&log);
    std::vector<int> order;  // touched only on the loop thread
    std::atomic<int> executed{0};
    loop.post([&](mg_mgr&) {
        order.push_back(-1);
        ++executed;
    });
    ASSERT_TRUE(loop.start());
    EXPECT_EQ(mg_log_level, MG_LL_ERROR);

    std::vector<std::thread> posters;
    std::map<int, std::vector<int>> per_thread;  // filled on the loop thread
    for (int t = 0; t < 4; ++t) {
        posters.emplace_back([&, t] {
            for (int i = 0; i < 250; ++i) {
                loop.post([&, t, i](mg_mgr&) {
                    per_thread[t].push_back(i);
                    ++executed;
                });
            }
        });
    }
    for (auto& p : posters) p.join();
    ASSERT_TRUE(wait_until([&] { return executed.load() == 1001; }));

    std::atomic<bool> stopped{false};
    std::atomic<int> after_stop{0};
    Clock clock(loop);
    std::function<void()> rearm = [&] {
        if (stopped) ++after_stop;
        clock.start_timer(1, rearm);
    };
    clock.start_timer(1, rearm);
    for (int d = 5; d <= 300; d += 5) {
        clock.start_timer(d, [&] {
            if (stopped) ++after_stop;
        });
    }
    std::atomic<bool> posting{true};
    std::thread poster([&] {
        while (posting) {
            loop.post([&](mg_mgr&) {
                if (stopped) ++after_stop;
            });
            std::this_thread::sleep_for(milliseconds(1));
        }
    });
    std::this_thread::sleep_for(milliseconds(50));
    // A command that is still running when stop() is called: stop() returns only after it has ended.
    std::atomic<bool> long_entered{false};
    loop.post([&](mg_mgr&) {
        long_entered = true;
        std::this_thread::sleep_for(milliseconds(100));
        if (stopped) ++after_stop;
    });
    EXPECT_TRUE(wait_until([&] { return long_entered.load(); }));
    loop.stop();
    stopped = true;
    std::this_thread::sleep_for(milliseconds(350));
    posting = false;
    poster.join();

    EXPECT_EQ(after_stop.load(), 0);
    EXPECT_EQ(order, std::vector<int>{-1});
    for (int t = 0; t < 4; ++t) {
        ASSERT_EQ(per_thread[t].size(), 250u);
        for (int i = 0; i < 250; ++i) EXPECT_EQ(per_thread[t][i], i);
    }
    EXPECT_FALSE(loop.running());
    loop.stop();  // idempotent
}

}  // namespace
