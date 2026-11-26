// TST-148: bounded AL task queue (Section 4.2.2; B4 and ER-27 of Sections 4.5.4 and 4.8.2;
// rules T3 and T5 of Section 4.2.1).
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "fakes/fake_clock.hpp"
#include "fakes/fake_logger.hpp"
#include "task_queue.hpp"

namespace {

using al::TaskKind;
using al::TaskQueue;

// Control tasks are recorded as negative numbers -1 … -5, data tasks as their index 0 … n − 1.
TEST(Tst148, BoundedDataTasksAndRateLimitedWarning) {
    fakes::FakeClock clock;
    fakes::FakeLogger logger;
    TaskQueue queue(clock, logger);
    std::vector<int> executed;

    auto post_control = [&](int number) {
        EXPECT_TRUE(queue.post(TaskKind::Control, [&executed, number] { executed.push_back(-number); }));
    };
    auto post_data = [&](int index) {
        return queue.post(TaskKind::Data, [&executed, index] { executed.push_back(index); });
    };

    // Step 1: 10 000 data tasks and 5 control tasks, three of them after the data bound is reached.
    post_control(1);
    for (int i = 0; i < 10000; ++i) {
        if (i == 4000) post_control(2);
        if (i == 8192) post_control(3);
        if (i == 9000) post_control(4);
        EXPECT_EQ(post_data(i), i < 8192) << "data task " << i;
    }
    post_control(5);

    EXPECT_EQ(queue.queued_data_tasks(), 8192u);
    EXPECT_EQ(queue.queued_control_tasks(), 5u);
    EXPECT_EQ(queue.dropped_data_tasks(), 1808u);
    ASSERT_EQ(logger.count(pal::LogLevel::Warn), 1u);

    // Step 2: 3 000 more data tasks while the clock advances 3 s (one post per millisecond).
    for (int i = 0; i < 3000; ++i) {
        clock.advance(1);
        EXPECT_FALSE(post_data(10000 + i));
    }
    EXPECT_EQ(queue.dropped_data_tasks(), 4808u);

    const std::vector<fakes::FakeLogger::Line> lines = logger.lines();
    ASSERT_EQ(lines.size(), 4u);   // 1 of step 1, 3 of step 2 (at 1 000, 2 000 and 3 000 ms)
    const char* totals[] = {"total=1", "total=2808", "total=3808", "total=4808"};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i].level, pal::LogLevel::Warn);
        EXPECT_EQ(lines[i].module, "AL-MAIN");
        EXPECT_NE(lines[i].message.find("task queue full"), std::string::npos) << lines[i].message;
        EXPECT_NE(lines[i].message.find(totals[i]), std::string::npos) << lines[i].message;
    }

    // Step 3: run the queue. Post order without the dropped tasks; no dropped task is ever executed.
    EXPECT_EQ(queue.run_pending(), 8197u);
    std::vector<int> expected;
    expected.push_back(-1);
    for (int i = 0; i < 8192; ++i) {
        if (i == 4000) expected.push_back(-2);
        expected.push_back(i);
    }
    for (int c = 3; c <= 5; ++c) expected.push_back(-c);
    EXPECT_EQ(executed, expected);

    EXPECT_EQ(queue.queued_data_tasks(), 0u);
    EXPECT_EQ(queue.queued_control_tasks(), 0u);
    EXPECT_EQ(queue.dropped_data_tasks(), 4808u);
    EXPECT_EQ(logger.count(pal::LogLevel::Warn), 4u);

    // The bound applies to waiting tasks only: after the queue drained, data tasks are queued again.
    EXPECT_TRUE(post_data(20000));
    EXPECT_EQ(queue.run_pending(), 1u);
    EXPECT_EQ(executed.back(), 20000);
}

TEST(Tst148, RunPendingRunsTasksPostedByTasks) {
    fakes::FakeClock clock;
    fakes::FakeLogger logger;
    TaskQueue queue(clock, logger);
    std::vector<std::string> executed;

    queue.post(TaskKind::Control, [&] {
        executed.push_back("first");
        queue.post(TaskKind::Data, [&] { executed.push_back("posted by first"); });
    });
    queue.post(TaskKind::Data, [&] { executed.push_back("second"); });

    EXPECT_EQ(queue.run_pending(), 3u);
    EXPECT_EQ(executed, (std::vector<std::string>{"first", "second", "posted by first"}));
    EXPECT_EQ(queue.run_pending(), 0u);
}

TEST(Tst148, WorkerThreadRunsInPostOrderUntilClose) {
    fakes::FakeClock clock;
    fakes::FakeLogger logger;
    TaskQueue queue(clock, logger);
    std::vector<int> executed;   // written only by the worker
    std::promise<void> done;

    std::thread worker([&] { queue.run(); });
    std::thread poster([&] {
        for (int i = 0; i < 1000; ++i) {
            if (i % 100 == 0) {
                queue.post(TaskKind::Control, [&executed, i] { executed.push_back(-i - 1); });
            }
            queue.post(TaskKind::Data, [&executed, i] { executed.push_back(i); });
        }
        queue.post(TaskKind::Control, [&done] { done.set_value(); });
    });

    poster.join();
    ASSERT_EQ(done.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
    queue.close();
    worker.join();   // run() has returned

    std::vector<int> expected;
    for (int i = 0; i < 1000; ++i) {
        if (i % 100 == 0) expected.push_back(-i - 1);
        expected.push_back(i);
    }
    EXPECT_EQ(executed, expected);
    EXPECT_EQ(queue.dropped_data_tasks(), 0u);
}

TEST(Tst148, CloseDiscardsQueuedTasksAndLaterPosts) {
    fakes::FakeClock clock;
    fakes::FakeLogger logger;
    TaskQueue queue(clock, logger);
    int executed = 0;

    EXPECT_TRUE(queue.post(TaskKind::Control, [&] { ++executed; }));
    EXPECT_TRUE(queue.post(TaskKind::Data, [&] { ++executed; }));
    queue.close();

    EXPECT_EQ(queue.queued_data_tasks(), 0u);
    EXPECT_EQ(queue.queued_control_tasks(), 0u);
    EXPECT_FALSE(queue.post(TaskKind::Control, [&] { ++executed; }));
    EXPECT_FALSE(queue.post(TaskKind::Data, [&] { ++executed; }));
    EXPECT_EQ(queue.run_pending(), 0u);
    queue.run();   // returns at once after close()
    EXPECT_EQ(executed, 0);
    EXPECT_EQ(queue.dropped_data_tasks(), 0u);   // posts after close() are not counted as drops
}

}  // namespace
