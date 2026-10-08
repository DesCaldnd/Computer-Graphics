// Глава 08: отмена (handle, токен, владелец, дочерние задачи) и интроспекция планировщика
// (docs/guide/08-coroutines.md).
#include <oxwald/async/async.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace ox;

namespace {

struct Effect { // «эффект», который должен исчезнуть при отмене
    int& alive;
    explicit Effect(int& a) : alive(a) { ++alive; }
    ~Effect() { --alive; }
};

Task<> burn(int& alive, int& ticks) {
    Effect fire(alive);
    for (;;) {
        ++ticks;
        co_await seconds(0.5);
    }
}

} // namespace

TEST(GuideCoroCancellation, HandleTokenAndOwner) {
    CoroutineScheduler sched;
    int alive = 0, ticks = 0;

    // 1) Через handle.
    CoroutineHandle a = sched.spawn(burn(alive, ticks), {.name = "Burn"});
    // 2) Через внешний токен: например, «все эффекты квеста» отменяются вместе.
    CancellationSource questEffects;
    sched.spawn(burn(alive, ticks), {.name = "QuestFx", .token = questEffects.token()});
    // 3) Через владельца (id сущности).
    sched.spawn(burn(alive, ticks), {.name = "TorchFx", .owner = 100});
    EXPECT_EQ(alive, 3);

    a.cancel(); // потокобезопасно; на главном потоке подвешенная задача раскручивается сразу
    EXPECT_EQ(a.status(), CoroutineStatus::Cancelled);
    questEffects.cancel();
    EXPECT_EQ(sched.cancelOwner(100), 1u);
    EXPECT_EQ(alive, 0); // все деструкторы отработали
    EXPECT_EQ(sched.liveCount(), 0u);
}

TEST(GuideCoroCancellation, ChildrenDieWithParent) {
    CoroutineScheduler sched;
    int alive = 0, ticks = 0;
    CoroutineHandle sparks;
    auto boss = sched.spawn(
        [&]() -> Task<> {
            // Дочерняя задача живёт сама по себе (не ждём её), но умрёт вместе с родителем и наследует владельца.
            sparks = co_await spawnChild(burn(alive, ticks), "Sparks");
            co_await seconds(100.0);
        },
        {.name = "BossFight", .owner = 5});
    ASSERT_TRUE(sparks.valid());
    EXPECT_EQ(sparks.owner(), 5u);
    boss.cancel();
    EXPECT_EQ(sparks.status(), CoroutineStatus::Cancelled);
    EXPECT_EQ(alive, 0);
}

TEST(GuideCoroCancellation, LongBackgroundLoopChecksCancellation) {
    ThreadPoolExecutor pool(1);
    CoroutineScheduler sched(&pool);
    std::atomic<int> iterations{0};
    std::atomic<bool> started{false};
    auto h = sched.spawn([&]() -> Task<> {
        co_await backgroundThread();
        auto cancel = co_await currentCancellation();
        started = true;
        while (!cancel.isCancelled()) { // фоновую часть нельзя «вырвать» — она сама проверяет флаг
            ++iterations;
            std::this_thread::yield();
        }
        co_await mainThread(); // здесь задача и будет уничтожена
    });
    while (!started) std::this_thread::yield();
    h.cancel(); // отмена отложена, пока задача на фоновом потоке
    for (int i = 0; i < 2000 && sched.liveCount() > 0; ++i) {
        sched.tick(0.016);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    EXPECT_EQ(h.status(), CoroutineStatus::Cancelled);
    EXPECT_GT(iterations.load(), 0);
}

TEST(GuideCoroCancellation, IntrospectionForTheEditor) {
    CoroutineScheduler sched;
    sched.spawn([]() -> Task<> {
        co_await named("OpenDoor");
        co_await seconds(2.0);
    }, {.owner = 9});
    Promise<int> pending;
    sched.spawn([f = pending.future()]() -> Task<> { co_await f; }, {.name = "LoadMesh"});
    sched.tick(0.5);

    std::vector<CoroutineInfo> list = sched.coroutines(); // данные для панели «Coroutines»
    std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.id < y.id; });
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "OpenDoor");
    EXPECT_EQ(list[0].owner, 9u);
    EXPECT_EQ(list[0].waitingOn, "seconds(1.50 left)");
    EXPECT_EQ(list[1].name, "LoadMesh");
    EXPECT_EQ(list[1].waitingOn, "future");
    EXPECT_EQ(sched.handlesForOwner(9).size(), 1u);
    sched.cancelAll(); // смена сцены
    EXPECT_EQ(sched.liveCount(), 0u);
}
