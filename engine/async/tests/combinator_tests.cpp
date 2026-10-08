#include "async_test_utils.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace ox;
using oxtest::LiveCounter;

namespace {

Task<int> after(int ticks, int v) {
    LiveCounter c;
    co_await frames(static_cast<u32>(ticks));
    co_return v;
}
Task<std::string> afterSeconds(double s, std::string v) {
    LiveCounter c;
    co_await seconds(s);
    co_return v;
}
Task<void> sleepTicks(int ticks) {
    LiveCounter c;
    co_await frames(static_cast<u32>(ticks));
}

} // namespace

TEST(AsyncCombinators, WhenAllTupleAndVoid) {
    CoroutineScheduler s;
    std::tuple<int, std::string, std::monostate> result;
    bool done = false;
    s.spawn([&]() -> Task<> {
        result = co_await whenAll(after(3, 1), afterSeconds(0.05, "x"), sleepTicks(1));
        done = true;
    });
    s.tick(0.016);
    s.tick(0.016);
    EXPECT_FALSE(done);
    s.tick(0.016);
    s.tick(0.016);
    EXPECT_TRUE(done);
    EXPECT_EQ(std::get<0>(result), 1);
    EXPECT_EQ(std::get<1>(result), "x");
}

TEST(AsyncCombinators, WhenAllRange) {
    CoroutineScheduler s;
    std::vector<int> values;
    s.spawn([&]() -> Task<> {
        std::vector<Task<int>> tasks;
        for (int i = 0; i < 5; ++i) {
            tasks.push_back(after(5 - i, i * 10));
        }
        values = co_await whenAll(std::move(tasks));
        std::vector<Task<void>> voids;
        voids.push_back(sleepTicks(1));
        voids.push_back(sleepTicks(2));
        co_await whenAll(std::move(voids));
        values.push_back(-1);
    });
    oxtest::tickUntil(s, [&] { return s.liveCount() == 0; });
    EXPECT_EQ(values, (std::vector<int>{0, 10, 20, 30, 40, -1}));
}

TEST(AsyncCombinators, WhenAllSynchronousBranches) {
    CoroutineScheduler s;
    int sum = 0;
    s.spawn([&]() -> Task<> {
        auto [a, b] = co_await whenAll(after(0, 2), after(0, 3));
        sum = a + b;
    });
    EXPECT_EQ(sum, 5); // no suspension at all
}

TEST(AsyncCombinators, WhenAllRethrowsFirstErrorAfterAll) {
    CoroutineScheduler s;
    std::string error;
    int finished = 0;
    s.spawn([&]() -> Task<> {
        auto failing = []() -> Task<int> {
            co_await nextFrame();
            throw std::runtime_error("branch failed");
        };
        auto slow = [&]() -> Task<int> {
            co_await frames(3);
            ++finished;
            co_return 1;
        };
        try {
            co_await whenAll(failing(), slow());
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    oxtest::tickUntil(s, [&] { return s.liveCount() == 0; });
    EXPECT_EQ(error, "branch failed");
    EXPECT_EQ(finished, 1);
}

TEST(AsyncCombinators, WhenAnyCancelsLosers) {
    LiveCounter::reset();
    CoroutineScheduler s;
    Promise<int> neverPromise;
    usize index = 99;
    int winnerValue = 0;
    bool loserResumed = false;
    s.spawn([&]() -> Task<> {
        auto loser = [&](Future<int> f) -> Task<int> {
            LiveCounter c;
            int v = co_await f;
            loserResumed = true;
            co_return v;
        };
        auto r = co_await whenAny(after(10, 1), after(2, 2), loser(neverPromise.future()), afterSeconds(50.0, "s"));
        index = r.index;
        winnerValue = std::get<1>(r.value);
    });
    s.tick(0.016);
    EXPECT_EQ(LiveCounter::alive, 4);
    s.tick(0.016);
    EXPECT_EQ(index, 1u);
    EXPECT_EQ(winnerValue, 2);
    EXPECT_EQ(LiveCounter::alive, 0); // all losers unwound
    EXPECT_EQ(LiveCounter::destroyed, 4);
    neverPromise.setValue(5);
    for (int i = 0; i < 20; ++i) {
        s.tick(10.0);
    }
    EXPECT_FALSE(loserResumed);
    EXPECT_EQ(s.liveCount(), 0u);
}

TEST(AsyncCombinators, WhenAnyRangeAndImmediateWinner) {
    LiveCounter::reset();
    CoroutineScheduler s;
    usize idx = 99;
    int value = 0;
    s.spawn([&]() -> Task<> {
        std::vector<Task<int>> tasks;
        tasks.push_back(after(4, 1));
        tasks.push_back(after(0, 2)); // finishes during start: the third never starts
        tasks.push_back(after(1, 3));
        auto r = co_await whenAny(std::move(tasks));
        idx = r.index;
        value = r.value;
    });
    EXPECT_EQ(idx, 1u);
    EXPECT_EQ(value, 2);
    EXPECT_EQ(LiveCounter::alive, 0);
    EXPECT_EQ(s.liveCount(), 0u);
}

TEST(AsyncCombinators, Timeout) {
    CoroutineScheduler s;
    Promise<std::string> slow;
    Promise<std::string> fast;
    std::optional<std::string> a, b;
    bool voidOk = true;
    s.spawn([&]() -> Task<> { a = co_await timeout(slow.future(), 1.0); });
    s.spawn([&]() -> Task<> { b = co_await timeout(fast.future(), 1.0); });
    s.spawn([&]() -> Task<> { voidOk = co_await timeout(toTask(seconds(5.0)), 0.5); });
    s.tick(0.1);
    fast.setValue("reply");
    s.tick(0.1);
    EXPECT_EQ(b, "reply");
    for (int i = 0; i < 10; ++i) {
        s.tick(0.1);
    }
    EXPECT_FALSE(a.has_value());
    EXPECT_FALSE(voidOk);
    EXPECT_EQ(s.liveCount(), 0u);
}

TEST(AsyncCombinators, CancellingParentUnwindsBranches) {
    LiveCounter::reset();
    CoroutineScheduler s;
    auto h = s.spawn([&]() -> Task<> {
        co_await whenAll(after(100, 1), whenAll(after(200, 2), sleepTicks(300)));
    }, {.owner = 1});
    s.tick(0.016);
    EXPECT_EQ(LiveCounter::alive, 3);
    s.cancelOwner(1);
    EXPECT_EQ(LiveCounter::alive, 0);
    EXPECT_EQ(h.status(), CoroutineStatus::Cancelled);
}

TEST(AsyncCombinators, DialogueChoiceOrIdleTimeout) {
    CoroutineScheduler s;
    Signal<int> onChoice;
    std::vector<int> choices;
    auto talk = [&]() -> Task<> {
        auto r = co_await whenAny(toTask(event(onChoice)), toTask(realSeconds(30.0)));
        choices.push_back(r.index == 0 ? std::get<0>(r.value) : -1);
    };
    s.spawn(talk);
    s.spawn(talk);
    onChoice.emit(1); // both waiting dialogues receive it
    s.tick(0.1);
    EXPECT_EQ(choices, (std::vector<int>{1, 1}));
    EXPECT_EQ(onChoice.slotCount(), 0u);
    s.spawn(talk);
    for (int i = 0; i < 31; ++i) {
        s.tick(1.0);
    }
    EXPECT_EQ(choices.back(), -1);
    EXPECT_EQ(onChoice.slotCount(), 0u); // loser's connection dropped
}
