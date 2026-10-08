#include "async_test_utils.hpp"

#include <oxwald/core/jobs.hpp>

#include <thread>

using namespace ox;
using oxtest::LiveCounter;

TEST(AsyncThreading, BackgroundThenMainThreadHop) {
    CoroutineScheduler s;
    const auto mainId = std::this_thread::get_id();
    std::thread::id bgId, backId;
    int result = 0;
    auto h = s.spawn([&]() -> Task<> {
        co_await backgroundThread();
        bgId = std::this_thread::get_id();
        int sum = 0;
        for (int i = 1; i <= 1000; ++i) {
            sum += i;
        }
        co_await mainThread();
        backId = std::this_thread::get_id();
        result = sum;
    });
    ASSERT_TRUE(oxtest::tickUntil(s, [&] { return h.isDone(); }));
    EXPECT_NE(bgId, mainId);
    EXPECT_EQ(backId, mainId);
    EXPECT_EQ(result, 500500);
    EXPECT_EQ(h.status(), CoroutineStatus::Completed);
}

TEST(AsyncThreading, FutureCompletedFromOtherThreadResumesOnTick) {
    CoroutineScheduler s;
    Promise<int> p;
    std::thread::id resumedOn;
    int got = 0;
    s.spawn([&, f = p.future()]() -> Task<> {
        got = co_await f;
        resumedOn = std::this_thread::get_id();
    });
    std::thread producer([&] { p.setValue(77); });
    producer.join();
    EXPECT_EQ(got, 0); // not resumed on the producer thread
    s.tick(0.016);
    EXPECT_EQ(got, 77);
    EXPECT_EQ(resumedOn, std::this_thread::get_id());
}

TEST(AsyncThreading, RunAsyncOnPoolAndJobSystem) {
    ThreadPoolExecutor pool(2);
    JobSystem jobs(3);
    JobSystemExecutor jobExec(jobs);
    CoroutineScheduler s(&jobExec);
    int a = 0, b = 0;
    std::thread::id jobThread;
    auto h = s.spawn([&]() -> Task<> {
        a = co_await runAsync(pool, [] { return 21 * 2; });
        co_await backgroundThread(); // job system workers
        jobThread = std::this_thread::get_id();
        b = co_await runAsync(pool, [] { return 5; }); // resumed inline (we are off-thread)
        co_await switchTo(s);                         // same as mainThread()
    });
    ASSERT_TRUE(oxtest::tickUntil(s, [&] { return h.isDone(); }));
    EXPECT_EQ(a, 42);
    EXPECT_EQ(b, 5);
    EXPECT_NE(jobThread, std::this_thread::get_id());
    jobs.waitAll();
}

TEST(AsyncThreading, CancelWhileOffThreadIsDeferred) {
    LiveCounter::reset();
    CoroutineScheduler s;
    std::atomic<bool> release{false};
    std::atomic<bool> sawCancel{false};
    bool afterMain = false;
    auto h = s.spawn([&]() -> Task<> {
        LiveCounter c;
        co_await backgroundThread();
        auto token = co_await currentCancellation();
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        sawCancel = token.isCancelled();
        co_await mainThread();
        afterMain = true; // must not run: cancelled
    });
    s.tick(0.016);
    h.cancel();
    EXPECT_TRUE(h.isRunning()); // deferred: the frame is executing on a worker
    EXPECT_EQ(LiveCounter::alive, 1);
    release = true;
    ASSERT_TRUE(oxtest::tickUntil(s, [&] { return h.isDone(); }));
    EXPECT_TRUE(sawCancel.load());
    EXPECT_FALSE(afterMain);
    EXPECT_EQ(h.status(), CoroutineStatus::Cancelled);
    EXPECT_EQ(LiveCounter::alive, 0);
}

TEST(AsyncThreading, RootFinishingOffThread) {
    CoroutineScheduler s;
    auto h = s.spawn([&]() -> Task<> {
        co_await backgroundThread();
        co_return; // finishes on the worker; the scheduler finalises it on its thread
    });
    ASSERT_TRUE(oxtest::tickUntil(s, [&] { return h.isDone(); }));
    EXPECT_EQ(h.status(), CoroutineStatus::Completed);
    EXPECT_EQ(s.liveCount(), 0u);
}

TEST(AsyncThreading, WhenAnyLoserOnWorkerIsTornDownWhenItReturns) {
    LiveCounter::reset();
    CoroutineScheduler s;
    std::atomic<bool> release{false};
    usize winner = 99;
    bool loserContinued = false;
    auto h = s.spawn([&]() -> Task<> {
        auto loser = [&]() -> Task<int> {
            LiveCounter c;
            co_await backgroundThread();
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            co_await mainThread();
            loserContinued = true;
            co_return 1;
        };
        auto r = co_await whenAny(loser(), toTask(frames(1)));
        winner = r.index;
    });
    s.tick(0.016); // frames(1) wins; the loser is still on the worker
    EXPECT_TRUE(h.isRunning());
    EXPECT_EQ(LiveCounter::alive, 1);
    release = true;
    ASSERT_TRUE(oxtest::tickUntil(s, [&] { return h.isDone(); }));
    EXPECT_EQ(winner, 1u);
    EXPECT_FALSE(loserContinued);
    EXPECT_EQ(LiveCounter::alive, 0);
}

TEST(AsyncThreading, WhenAllOnBackgroundBranches) {
    CoroutineScheduler s;
    std::vector<int> out;
    std::thread::id resumedOn;
    auto h = s.spawn([&]() -> Task<> {
        auto work = [](int v) -> Task<int> {
            co_await backgroundThread();
            co_return v * v;
        };
        auto [a, b, c] = co_await whenAll(work(2), work(3), work(4));
        resumedOn = std::this_thread::get_id();
        out = {a, b, c};
    });
    ASSERT_TRUE(oxtest::tickUntil(s, [&] { return h.isDone(); }));
    EXPECT_EQ(out, (std::vector<int>{4, 9, 16}));
    EXPECT_EQ(resumedOn, std::this_thread::get_id()); // branch completions are joined on the main thread
}
