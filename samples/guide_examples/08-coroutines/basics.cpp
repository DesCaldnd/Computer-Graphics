// Глава 08: основы корутин — Task, CoroutineScheduler, ожидания по времени и кадрам, Future/Promise,
// адаптер колбэков, runAsync, Generator (docs/guide/08-coroutines.md).
#include <oxwald/async/async.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ox;

namespace {

// --- Первая корутина: обратный отсчёт по игровому времени.
Task<int> countdown(int n, std::vector<int>& log) {
    for (int i = n; i > 0; --i) {
        log.push_back(i);
        co_await seconds(1.0); // игровое время: учитывает timeScale и паузу
    }
    co_return n;
}

// --- Callback-API (как у загрузчика ассетов), который мы хотим «ожидать».
struct CallbackLoader {
    std::vector<std::pair<std::string, std::function<void(std::string)>>> pending;
    void loadAsync(std::string path, std::function<void(std::string)> done) {
        pending.emplace_back(std::move(path), std::move(done));
    }
    void completeAll() {
        for (auto& [path, done] : pending) done("data:" + path);
        pending.clear();
    }
};

Task<std::string> loadText(CallbackLoader& loader, std::string path) {
    // Одна строка превращает колбэк в co_await.
    std::string data = co_await awaitCallback<std::string>([&](auto resume) { loader.loadAsync(path, resume); });
    co_return data;
}

// --- Синхронный генератор: обход клеток поля без промежуточного вектора.
struct Cell {
    int x = 0, y = 0;
};
Generator<Cell> cellsAround(Cell c, int radius) {
    for (int y = c.y - radius; y <= c.y + radius; ++y)
        for (int x = c.x - radius; x <= c.x + radius; ++x) co_yield Cell{x, y};
}

// Тикает планировщик, пока не выполнится условие (для фоновых потоков нужно реальное ожидание).
bool tickUntil(CoroutineScheduler& s, const std::function<bool()>& done, int maxTicks = 5000) {
    for (int i = 0; i < maxTicks && !done(); ++i) {
        s.tick(1.0 / 60.0);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return done();
}

} // namespace

TEST(GuideCoroBasics, SpawnRunsUntilFirstSuspension) {
    CoroutineScheduler sched; // поток-создатель = главный (игровой) поток планировщика
    std::vector<int> log;
    CoroutineHandle h = sched.spawn(countdown(3, log), {.name = "Countdown"});

    // spawn() сразу выполняет корутину до первого co_await (как StartCoroutine в Unity).
    EXPECT_EQ(log, std::vector<int>{3});
    EXPECT_TRUE(h.isRunning());

    for (int frame = 0; frame < 20 && h.isRunning(); ++frame) {
        sched.tick(0.5); // dt реального кадра
    }
    EXPECT_EQ(log, (std::vector<int>{3, 2, 1}));
    EXPECT_EQ(h.status(), CoroutineStatus::Completed);
}

TEST(GuideCoroBasics, GameTimeVsRealTime) {
    CoroutineScheduler sched;
    bool gameDone = false, realDone = false;
    sched.spawn([&]() -> Task<> {
        co_await seconds(1.0); // игровое время
        gameDone = true;
    });
    sched.spawn([&]() -> Task<> {
        co_await realSeconds(1.0); // реальное время (меню, UI)
        realDone = true;
    });

    sched.setPaused(true); // пауза замораживает только игровое время
    sched.tick(1.5);
    EXPECT_FALSE(gameDone);
    EXPECT_TRUE(realDone);

    sched.setPaused(false);
    sched.setTimeScale(0.5); // замедление: 1 с реального времени = 0.5 с игрового
    sched.tick(1.0);
    EXPECT_FALSE(gameDone);
    sched.tick(1.0);
    EXPECT_TRUE(gameDone);
}

TEST(GuideCoroBasics, FramesPredicatesAndFixedUpdate) {
    CoroutineScheduler sched;
    std::vector<std::string> log;
    bool bridgeLowered = false;
    sched.spawn([&]() -> Task<> {
        co_await nextFrame();                              // следующий tick
        log.push_back("frame1");
        co_await frames(2);                                // ещё два tick
        log.push_back("frame3");
        co_await until([&] { return bridgeLowered; });     // проверяется раз в tick
        log.push_back("bridge");
        co_await nextFixedUpdate();                        // следующий fixedTick (после шага физики)
        log.push_back("fixed");
    });
    sched.tick(0.016);
    EXPECT_EQ(log, std::vector<std::string>{"frame1"});
    sched.tick(0.016);
    sched.tick(0.016);
    EXPECT_EQ(log.back(), "frame3");
    sched.tick(0.016);
    EXPECT_EQ(log.back(), "frame3"); // мост ещё поднят
    bridgeLowered = true;
    sched.tick(0.016);
    EXPECT_EQ(log.back(), "bridge");
    sched.fixedTick(1.0 / 60.0);
    EXPECT_EQ(log.back(), "fixed");
}

TEST(GuideCoroBasics, PromiseFutureAndCallbacks) {
    CoroutineScheduler sched;

    // Promise заполняется из любого потока; корутина продолжится в ближайшем tick на главном потоке.
    Promise<int> scorePromise;
    Future<int> score = scorePromise.future();
    int received = 0;
    sched.spawn([&, score]() -> Task<> { received = co_await score; });
    std::thread([&] { scorePromise.setValue(1500); }).join();
    EXPECT_EQ(received, 0); // ещё не tick
    sched.tick(0.016);
    EXPECT_EQ(received, 1500);

    // Callback-API через awaitCallback.
    CallbackLoader loader;
    std::string text;
    sched.spawn([&]() -> Task<> { text = co_await loadText(loader, "dialogs/intro.txt"); });
    loader.completeAll();
    sched.tick(0.016);
    EXPECT_EQ(text, "data:dialogs/intro.txt");

    // Если колбэк так и не вызван (все копии resume уничтожены) — BrokenPromise, а не вечное ожидание.
    bool broken = false;
    sched.spawn([&]() -> Task<> {
        try {
            co_await loadText(loader, "lost.txt");
        } catch (const BrokenPromise&) {
            broken = true;
        }
    });
    loader.pending.clear();
    sched.tick(0.016);
    EXPECT_TRUE(broken);
}

TEST(GuideCoroBasics, RunAsyncOnThreadPool) {
    ThreadPoolExecutor pool(2);
    CoroutineScheduler sched;
    int answer = 0;
    auto h = sched.spawn([&]() -> Task<> {
        // Обычная функция на пуле потоков; результат ждём как Future.
        Future<int> f = runAsync(pool, [] { return 6 * 7; });
        answer = co_await f;
    });
    ASSERT_TRUE(tickUntil(sched, [&] { return h.isDone(); }));
    EXPECT_EQ(answer, 42);
}

TEST(GuideCoroBasics, ExceptionsPropagateToAwaiterAndFailRoot) {
    CoroutineScheduler sched;
    auto mayFail = [](bool fail) -> Task<int> {
        co_await nextFrame();
        if (fail) throw std::runtime_error("no ammo");
        co_return 1;
    };
    std::string caught;
    auto ok = sched.spawn([&]() -> Task<> {
        try {
            co_await mayFail(true);
        } catch (const std::exception& e) {
            caught = e.what(); // исключение дошло до ожидающего
        }
    });
    auto failed = sched.spawn([&]() -> Task<> { co_await mayFail(true); }, {.name = "Reload"});
    sched.tick(0.016);
    EXPECT_EQ(caught, "no ammo");
    EXPECT_EQ(ok.status(), CoroutineStatus::Completed);
    EXPECT_EQ(failed.status(), CoroutineStatus::Failed); // корневая задача упала: статус + лог ошибки
    EXPECT_EQ(failed.error(), "no ammo");
}

TEST(GuideCoroBasics, GeneratorInRangeFor) {
    int count = 0, sumX = 0;
    for (Cell c : cellsAround({10, 5}, 1)) {
        ++count;
        sumX += c.x;
    }
    EXPECT_EQ(count, 9);
    EXPECT_EQ(sumX, 90);
}
