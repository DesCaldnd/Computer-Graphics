#include "async_test_utils.hpp"

#include <oxwald/core/log.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace ox;
using oxtest::LiveCounter;

TEST(AsyncScheduler, NextFrameAndFramesOrdering) {
    CoroutineScheduler s;
    std::vector<std::string> log;
    s.spawn([&]() -> Task<> {
        log.push_back("a0");
        co_await nextFrame();
        log.push_back("a1");
        co_await frames(2);
        log.push_back("a3");
    });
    s.spawn([&]() -> Task<> {
        log.push_back("b0");
        co_await frames(0); // continues immediately
        log.push_back("b0'");
        co_await nextFrame();
        log.push_back("b1");
        co_await nextFrame();
        log.push_back("b2");
    });
    EXPECT_EQ(log, (std::vector<std::string>{"a0", "b0", "b0'"}));
    s.tick(0.016);
    EXPECT_EQ(log, (std::vector<std::string>{"a0", "b0", "b0'", "a1", "b1"}));
    s.tick(0.016);
    EXPECT_EQ(log.back(), "b2");
    s.tick(0.016);
    EXPECT_EQ(log.back(), "a3");
    EXPECT_EQ(s.liveCount(), 0u);
}

TEST(AsyncScheduler, SecondsRespectTimeScaleAndPause) {
    CoroutineScheduler s;
    bool game = false, real = false;
    s.spawn([&]() -> Task<> {
        co_await seconds(1.0);
        game = true;
    });
    s.spawn([&]() -> Task<> {
        co_await realSeconds(1.0);
        real = true;
    });
    s.setTimeScale(0.5);
    for (int i = 0; i < 10; ++i) {
        s.tick(0.1); // 1.0 real s, 0.5 game s
    }
    EXPECT_TRUE(real);
    EXPECT_FALSE(game);
    s.setPaused(true);
    for (int i = 0; i < 20; ++i) {
        s.tick(0.1);
    }
    EXPECT_FALSE(game);
    EXPECT_NEAR(s.gameTime(), 0.5, 1e-9);
    s.setPaused(false);
    s.setTimeScale(1.0);
    for (int i = 0; i < 4; ++i) {
        s.tick(0.1);
    }
    EXPECT_FALSE(game); // 0.9
    s.tick(0.11);
    EXPECT_TRUE(game);
}

TEST(AsyncScheduler, ZeroSecondsWaitsOneTick) {
    CoroutineScheduler s;
    int step = 0;
    s.spawn([&]() -> Task<> {
        co_await seconds(0.0);
        step = 1;
    });
    EXPECT_EQ(step, 0);
    s.tick(0.0);
    EXPECT_EQ(step, 1);
}

TEST(AsyncScheduler, UntilAndWhileTrue) {
    CoroutineScheduler s;
    int counter = 0;
    bool door = false, done = false;
    s.spawn([&]() -> Task<> {
        co_await until([&] { return door; });
        co_await whileTrue([&] { return counter < 5; });
        done = true;
    });
    s.tick(0.016);
    s.tick(0.016);
    EXPECT_FALSE(done);
    door = true;
    counter = 3;
    s.tick(0.016);
    EXPECT_FALSE(done);
    counter = 5;
    s.tick(0.016);
    EXPECT_TRUE(done);
}

TEST(AsyncScheduler, FixedUpdate) {
    CoroutineScheduler s;
    int steps = 0;
    s.spawn([&]() -> Task<> {
        for (int i = 0; i < 3; ++i) {
            co_await nextFixedUpdate();
            ++steps;
        }
    });
    s.tick(0.016);
    EXPECT_EQ(steps, 0);
    s.fixedTick(1.0 / 60.0);
    EXPECT_EQ(steps, 1);
    s.fixedTick(1.0 / 60.0);
    s.fixedTick(1.0 / 60.0);
    EXPECT_EQ(steps, 3);
    EXPECT_EQ(s.liveCount(), 0u);
}

TEST(AsyncScheduler, CancelViaOwnerWhileSuspendedRunsDestructors) {
    LiveCounter::reset();
    CoroutineScheduler s;
    Promise<int> never;
    Future<int> f = never.future();
    auto nested = [&]() -> Task<int> {
        LiveCounter inner;
        co_await seconds(100.0);
        co_return 1;
    };
    bool after = false;
    auto h1 = s.spawn(
        [&]() -> Task<> {
            LiveCounter outer;
            co_await nested();
            after = true;
        },
        {.name = "timer", .owner = 42});
    auto h2 = s.spawn(
        [&]() -> Task<> {
            LiveCounter c;
            co_await f;
            after = true;
        },
        {.name = "future", .owner = 42});
    auto other = s.spawn([&]() -> Task<> { co_await seconds(100.0); }, {.owner = 7});
    s.tick(0.1);
    EXPECT_EQ(LiveCounter::alive, 3);
    EXPECT_EQ(s.cancelOwner(42), 2u);
    EXPECT_EQ(LiveCounter::alive, 0);
    EXPECT_EQ(LiveCounter::destroyed, 3);
    EXPECT_EQ(h1.status(), CoroutineStatus::Cancelled);
    EXPECT_EQ(h2.status(), CoroutineStatus::Cancelled);
    EXPECT_TRUE(other.isRunning());
    // Future continuation detached; completing it later is harmless.
    never.setValue(1);
    s.tick(200.0);
    EXPECT_FALSE(after);
    EXPECT_EQ(other.status(), CoroutineStatus::Completed);
}

TEST(AsyncScheduler, CancelViaTokenAndHandle) {
    LiveCounter::reset();
    CoroutineScheduler s;
    CancellationSource src;
    auto h = s.spawn(
        [&]() -> Task<> {
            LiveCounter c;
            for (;;) {
                co_await nextFrame();
            }
        },
        {.token = src.token()});
    auto h2 = s.spawn([&]() -> Task<> {
        LiveCounter c;
        co_await until([] { return false; });
    });
    s.tick(0.016);
    EXPECT_EQ(LiveCounter::alive, 2);
    src.cancel();
    EXPECT_EQ(h.status(), CoroutineStatus::Cancelled);
    h2.cancel();
    EXPECT_EQ(h2.status(), CoroutineStatus::Cancelled);
    EXPECT_EQ(LiveCounter::alive, 0);

    // Already-cancelled token: never starts.
    bool ran = false;
    auto h3 = s.spawn(
        [&]() -> Task<> {
            ran = true;
            co_return;
        },
        {.token = src.token()});
    EXPECT_FALSE(ran);
    EXPECT_EQ(h3.status(), CoroutineStatus::Cancelled);
}

TEST(AsyncScheduler, SelfCancelUnwindsAtNextAwait) {
    LiveCounter::reset();
    CoroutineScheduler s;
    CoroutineHandle self;
    bool reached = false;
    self = s.spawn(
        [&]() -> Task<> {
            LiveCounter c;
            co_await nextFrame();
            self.cancel(); // running: deferred
            co_await nextFrame();
            reached = true;
        },
        {.deferStart = true});
    EXPECT_EQ(LiveCounter::alive, 0);
    s.tick(0.016); // starts
    EXPECT_EQ(LiveCounter::alive, 1);
    s.tick(0.016);
    EXPECT_FALSE(reached);
    EXPECT_EQ(LiveCounter::alive, 0);
    EXPECT_EQ(self.status(), CoroutineStatus::Cancelled);
}

TEST(AsyncScheduler, ExceptionsPropagateAndFailRoot) {
    CoroutineScheduler s;
    std::string caught;
    auto failing = []() -> Task<int> {
        co_await nextFrame();
        throw std::runtime_error("bad door");
    };
    s.spawn([&]() -> Task<> {
        try {
            co_await failing();
        } catch (const std::exception& e) {
            caught = e.what();
        }
    });
    auto h = s.spawn([&]() -> Task<> {
        co_await failing();
    }, {.name = "uncaught"});
    s.tick(0.016);
    EXPECT_EQ(caught, "bad door");
    EXPECT_EQ(h.status(), CoroutineStatus::Failed);
    EXPECT_EQ(h.error(), "bad door");
}

TEST(AsyncScheduler, AwaitSignal) {
    CoroutineScheduler s;
    Signal<int, std::string> hit;
    Signal<> opened;
    int damage = 0;
    std::string by;
    bool wasOpened = false;
    s.spawn([&]() -> Task<> {
        auto [d, who] = co_await event(hit);
        damage = d;
        by = who;
        co_await event(opened);
        wasOpened = true;
    });
    EXPECT_EQ(hit.slotCount(), 1u);
    hit.emit(25, "orc");
    hit.emit(99, "late"); // ignored: one-shot
    EXPECT_EQ(damage, 0); // resumed on the next tick
    s.tick(0.016);
    EXPECT_EQ(damage, 25);
    EXPECT_EQ(by, "orc");
    EXPECT_EQ(hit.slotCount(), 0u);
    std::thread([&] { opened.emit(); }).join();
    s.tick(0.016);
    EXPECT_TRUE(wasOpened);
}

TEST(AsyncScheduler, ChildrenCancelledWithParent) {
    LiveCounter::reset();
    CoroutineScheduler s;
    CoroutineHandle child;
    auto parent = s.spawn(
        [&]() -> Task<> {
            child = co_await spawnChild([]() -> Task<> {
                LiveCounter c;
                co_await seconds(100.0);
            }());
            co_await seconds(100.0);
        },
        {.owner = 5});
    ASSERT_TRUE(child.valid());
    EXPECT_EQ(child.owner(), 5u);
    EXPECT_EQ(LiveCounter::alive, 1);
    parent.cancel();
    EXPECT_EQ(child.status(), CoroutineStatus::Cancelled);
    EXPECT_EQ(LiveCounter::alive, 0);
}

TEST(AsyncScheduler, IntrospectionAndNames) {
    CoroutineScheduler s;
    s.spawn([&]() -> Task<> {
        co_await named("OpenDoor");
        co_await seconds(2.0);
    }, {.owner = 9});
    Promise<int> p;
    s.spawn([f = p.future()]() -> Task<> { co_await f; }, {.name = "LoadMesh"});
    s.spawn([]() -> Task<> { co_await whenAll(toTask(nextFrame()), toTask(seconds(5.0))); }, {.name = "Both"});
    s.tick(0.5);
    auto list = s.coroutines();
    ASSERT_EQ(list.size(), 3u);
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.id < b.id; });
    EXPECT_EQ(list[0].name, "OpenDoor");
    EXPECT_EQ(list[0].owner, 9u);
    EXPECT_EQ(list[0].state, CoroutineState::Suspended);
    EXPECT_EQ(list[0].waitingOn, "seconds(1.50 left)");
    EXPECT_NEAR(list[0].ageSeconds, 0.5, 1e-9);
    EXPECT_EQ(list[0].ageFrames, 1u);
    EXPECT_EQ(list[1].waitingOn, "future");
    EXPECT_EQ(list[2].waitingOn, "seconds(4.50 left)"); // nextFrame branch finished
}

TEST(AsyncScheduler, ShutdownReportsLeaks) {
    LiveCounter::reset();
    std::vector<std::string> warnings;
    const int sink = log::addSink([&](const log::Record& r) {
        if (r.category == "async" && r.level == log::Level::Warn) {
            warnings.emplace_back(r.message);
        }
    });
    {
        CoroutineScheduler s;
        s.spawn([]() -> Task<> {
            LiveCounter c;
            co_await seconds(1000.0);
        }, {.name = "ForgottenQuest", .owner = 3});
    }
    log::removeSink(sink);
    EXPECT_EQ(LiveCounter::alive, 0);
    ASSERT_GE(warnings.size(), 2u);
    EXPECT_NE(warnings[1].find("ForgottenQuest"), std::string::npos);
}
