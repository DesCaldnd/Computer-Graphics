// Глава 01: сервисы (DI), сигналы и шина событий (docs/guide/01-core.md).
#include <oxwald/core/events.hpp>
#include <oxwald/core/services.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Интерфейс сервиса и две реализации.
class IScoreService {
public:
    virtual ~IScoreService() = default;
    virtual void add(int points) = 0;
    [[nodiscard]] virtual int total() const = 0;
};

class LocalScore final : public IScoreService {
public:
    void add(int points) override { m_total += points; }
    [[nodiscard]] int total() const override { return m_total; }

private:
    int m_total = 0;
};

// Сервис, который зависит от другого: получает его через Services при создании.
class Achievements {
public:
    explicit Achievements(IScoreService& score) : m_score(score) {}
    [[nodiscard]] bool unlocked() const { return m_score.total() >= 100; }

private:
    IScoreService& m_score;
};

struct EnemyKilled {
    std::string enemy;
    int points = 0;
};

} // namespace

TEST(GuideCoreServices, RegisterAndResolve) {
    ox::Services services;
    services.add<IScoreService>(std::make_unique<LocalScore>()); // по интерфейсу
    services.emplace<Achievements>(services.get<IScoreService>()); // по конкретному типу

    services.get<IScoreService>().add(120);
    EXPECT_TRUE(services.get<Achievements>().unlocked());

    EXPECT_TRUE(services.has<IScoreService>());
    EXPECT_EQ(services.tryGet<LocalScore>(), nullptr); // ключ — тип, под которым регистрировали
    // Уничтожение — в обратном порядке: Achievements раньше IScoreService.
}

TEST(GuideCoreEvents, SignalWithScopedConnection) {
    ox::Signal<int, const std::string&> damaged; // (урон, источник)
    int total = 0;
    {
        ox::ScopedConnection c = damaged.connect([&](int amount, const std::string&) { total += amount; });
        damaged.emit(10, "fall");
        damaged(5, "fire"); // operator() == emit
    } // соединение разорвано автоматически
    damaged.emit(100, "ignored");
    EXPECT_EQ(total, 15);
    EXPECT_TRUE(damaged.empty());
}

TEST(GuideCoreEvents, EventBusImmediateAndQueued) {
    ox::EventBus bus;
    std::vector<std::string> log;
    ox::ScopedConnection sub = bus.subscribe<EnemyKilled>([&](const EnemyKilled& e) {
        log.push_back(e.enemy + ":" + std::to_string(e.points));
    });

    bus.publish(EnemyKilled{"orc", 10}); // сразу, в этом потоке
    EXPECT_EQ(log.size(), 1u);

    // Из рабочего потока — только в очередь; доставит dispatch() на главном потоке.
    std::thread worker([&] { bus.enqueue(EnemyKilled{"troll", 50}); });
    worker.join();
    EXPECT_EQ(log.size(), 1u);
    EXPECT_EQ(bus.dispatch(), 1u); // движок вызывает dispatch() раз в кадр
    EXPECT_EQ(log, (std::vector<std::string>{"orc:10", "troll:50"}));
}
