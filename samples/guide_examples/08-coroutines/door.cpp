// Глава 08: сценарий «дверь» — ждём конец анимации (Signal), таймер, условие; корутина привязана к сущности
// и отменяется при её уничтожении (docs/guide/08-coroutines.md).
#include <oxwald/async/async.hpp>
#include <oxwald/core/events.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace ox;

namespace {

// Упрощённые «системы» игры. В реальном проекте это аниматор, физика и триггер-объём.
struct Animator {
    Signal<std::string> onFinished; // имя клипа, который доиграл
    std::vector<std::string> played;
    void play(const std::string& clip) { played.push_back(clip); }
};

struct Door {
    u64 entityId = 0;     // владелец корутины (id сущности)
    bool collision = true;
    bool playerInside = false;
    Animator animator;
};

// RAII: что бы ни случилось (отмена, исключение), дверь не останется «проходимой».
struct RestoreCollision {
    Door& door;
    ~RestoreCollision() { door.collision = true; }
};

Task<> openDoor(Door& door) {
    co_await named("OpenDoor");                    // имя для панели Coroutines и отчёта об утечках
    RestoreCollision guard{door};
    door.animator.play("Open");
    co_await event(door.animator.onFinished);      // ждём конец клипа
    door.collision = false;
    co_await seconds(5.0);                         // открыта 5 с игрового времени (пауза учитывается)
    co_await until([&] { return !door.playerInside; }); // не закрываем дверь об игрока
    door.collision = true;
    door.animator.play("Close");
    co_await event(door.animator.onFinished);
}

} // namespace

TEST(GuideCoroDoor, FullSequence) {
    CoroutineScheduler sched;
    Door door{.entityId = 42};
    CoroutineHandle h = sched.spawn(openDoor(door), {.owner = door.entityId});

    EXPECT_EQ(door.animator.played, std::vector<std::string>{"Open"});
    door.animator.onFinished.emit("Open"); // анимация доиграла (может прийти с любого потока)
    sched.tick(0.1);
    EXPECT_FALSE(door.collision);

    door.playerInside = true;
    for (int i = 0; i < 10; ++i) sched.tick(1.0); // 5 с прошли, но игрок в проёме
    EXPECT_EQ(door.animator.played.size(), 1u);

    door.playerInside = false;
    sched.tick(0.1);
    EXPECT_EQ(door.animator.played.back(), "Close");
    EXPECT_TRUE(door.collision);

    door.animator.onFinished.emit("Close");
    sched.tick(0.1);
    EXPECT_EQ(h.status(), CoroutineStatus::Completed);
}

TEST(GuideCoroDoor, DestroyingTheEntityCancelsTheSequence) {
    CoroutineScheduler sched;
    Door door{.entityId = 7};
    CoroutineHandle h = sched.spawn(openDoor(door), {.owner = door.entityId});
    door.animator.onFinished.emit("Open");
    sched.tick(0.1);
    ASSERT_FALSE(door.collision); // дверь открыта, корутина ждёт seconds(5)

    // Хук уничтожения сущности: вызвать ДО освобождения компонентов, чтобы деструкторы могли их трогать.
    EXPECT_EQ(sched.cancelOwner(door.entityId), 1u);

    EXPECT_EQ(h.status(), CoroutineStatus::Cancelled);
    EXPECT_TRUE(door.collision);                         // RestoreCollision отработал при раскрутке кадра
    EXPECT_EQ(door.animator.onFinished.slotCount(), 0u); // подписки и таймеры сняты
    EXPECT_EQ(sched.liveCount(), 0u);
}

TEST(GuideCoroDoor, PauseFreezesTheTimer) {
    CoroutineScheduler sched;
    Door door{.entityId = 1};
    sched.spawn(openDoor(door), {.owner = door.entityId});
    door.animator.onFinished.emit("Open");
    sched.tick(0.1);

    sched.setPaused(true); // меню паузы
    for (int i = 0; i < 10; ++i) sched.tick(1.0);
    EXPECT_FALSE(door.collision); // таймер стоит

    sched.setPaused(false);
    for (int i = 0; i < 6; ++i) sched.tick(1.0);
    EXPECT_TRUE(door.collision);
    EXPECT_EQ(door.animator.played.back(), "Close");
}
