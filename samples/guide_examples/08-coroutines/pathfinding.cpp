// Глава 08: фоновый поиск пути — тяжёлая работа на пуле потоков, результат применяется на игровом потоке
// (docs/guide/08-coroutines.md).
#include <oxwald/async/async.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

using namespace ox;

namespace {

struct Cell {
    int x = 0, y = 0;
    bool operator==(const Cell&) const = default;
};

// Навигационная сетка: findPath() только читает данные, поэтому безопасна для вызова с любого потока.
struct NavGrid {
    std::vector<Cell> findPath(Cell from, Cell to) const {
        std::vector<Cell> path{from};
        Cell c = from;
        while (!(c == to)) {
            if (c.x != to.x) c.x += to.x > c.x ? 1 : -1;
            else c.y += to.y > c.y ? 1 : -1;
            path.push_back(c);
        }
        return path;
    }
};

struct Agent {
    Cell cell;
    std::vector<Cell> path;
    std::thread::id searchedOn, appliedOn; // для проверки в тесте
};

Task<> repath(Agent& agent, const NavGrid& nav, Cell goal) {
    const Cell from = agent.cell;              // игровое состояние читаем на главном потоке
    co_await backgroundThread();               // -> фоновый исполнитель планировщика
    std::vector<Cell> path = nav.findPath(from, goal);
    agent.searchedOn = std::this_thread::get_id();
    co_await mainThread();                     // <- обратно; продолжим в ближайшем tick
    agent.path = std::move(path);              // безопасно: снова на игровом потоке
    agent.appliedOn = std::this_thread::get_id();
}

} // namespace

TEST(GuideCoroPathfinding, SearchOffThreadApplyOnMain) {
    // В движке фоновый исполнитель — JobSystemExecutor; в тесте — собственный пул.
    ThreadPoolExecutor pool(2);
    CoroutineScheduler sched(&pool);
    NavGrid nav;
    Agent agent{.cell = {0, 0}};

    auto h = sched.spawn(repath(agent, nav, {3, 2}), {.name = "Repath", .owner = 1});
    for (int i = 0; i < 5000 && h.isRunning(); ++i) {
        sched.tick(1.0 / 60.0);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    ASSERT_EQ(h.status(), CoroutineStatus::Completed);
    EXPECT_EQ(agent.path.size(), 6u);
    EXPECT_EQ(agent.path.back(), (Cell{3, 2}));
    EXPECT_NE(agent.searchedOn, std::this_thread::get_id());
    EXPECT_EQ(agent.appliedOn, std::this_thread::get_id());
}

TEST(GuideCoroPathfinding, ManyAgentsInParallel) {
    ThreadPoolExecutor pool(4);
    CoroutineScheduler sched(&pool);
    NavGrid nav;
    std::vector<Agent> agents(8);
    for (int i = 0; i < 8; ++i) agents[static_cast<usize>(i)].cell = {i, 0};
    // whenAll над вектором задач: все поиски идут параллельно, ждём последний.
    auto h = sched.spawn([&]() -> Task<> {
        std::vector<Task<>> jobs;
        for (Agent& a : agents) jobs.push_back(repath(a, nav, {0, 5}));
        co_await whenAll(std::move(jobs));
    });
    for (int i = 0; i < 5000 && h.isRunning(); ++i) {
        sched.tick(1.0 / 60.0);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    ASSERT_EQ(h.status(), CoroutineStatus::Completed);
    for (const Agent& a : agents) EXPECT_EQ(a.path.back(), (Cell{0, 5}));
}
