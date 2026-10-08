// Глава 03: свои компоненты и системы, фазы кадра, fixed step (docs/guide/03-ecs-scene.md).
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/system.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace {

// ---- свой компонент: обычная структура данных ----
struct SpinComponent {
    ox::f32 degreesPerSecond = 90.0f;
};

void registerGameTypes() {
    ox::registerSceneTypes();
    OX_REFLECT_TYPE(SpinComponent, "Game.Spin")
        .attributes(ox::attr::Category{"Gameplay"})
        .field("degreesPerSecond", &SpinComponent::degreesPerSecond);
    ox::ComponentRegistry::instance().add<SpinComponent>({.icon = "refresh"});
}

// ---- система: логика над компонентами ----
class SpinSystem final : public ox::ISystem {
public:
    std::string_view name() const override { return "Game.Spin"; }
    ox::SystemPhase phase() const override { return ox::SystemPhase::FixedUpdate; }
    void update(ox::SystemContext& ctx) override {
        for (auto [e, spin] : ctx.world.view<SpinComponent>().each()) {
            ox::Entity entity = ctx.world.wrap(e);
            const glm::quat step = glm::angleAxis(glm::radians(spin.degreesPerSecond * ctx.dt), glm::vec3(0, 1, 0));
            entity.setRotation(step * entity.localTransform().rotation); // помечает трансформ грязным
        }
    }
};

// Служба (service), доступная системам через ctx.services.
struct FrameLog {
    std::vector<std::string> lines;
};

class LogSystem final : public ox::ISystem {
public:
    LogSystem(std::string name, ox::SystemPhase phase, ox::i32 order = 0) : m_name(std::move(name)), m_phase(phase), m_order(order) {}
    std::string_view name() const override { return m_name; }
    ox::SystemPhase phase() const override { return m_phase; }
    ox::i32 order() const override { return m_order; }
    void update(ox::SystemContext& ctx) override { ctx.services.get<FrameLog>().lines.push_back(m_name); }

private:
    std::string m_name;
    ox::SystemPhase m_phase;
    ox::i32 m_order;
};

// Система только для режима игры (в редакторе в Edit-режиме пропускается).
class EnemyAiSystem final : public ox::ISystem {
public:
    std::string_view name() const override { return "Game.EnemyAI"; }
    ox::SystemPhase phase() const override { return ox::SystemPhase::Update; }
    bool playModeOnly() const override { return true; }
    void update(ox::SystemContext&) override { ++ticks; }
    int ticks = 0;
};

} // namespace

TEST(GuideSceneSystems, SpinInFixedUpdate) {
    registerGameTypes();
    ox::World world;
    ox::Services services;

    ox::Entity fan = world.create("Fan");
    fan.add<SpinComponent>().degreesPerSecond = 90.0f;

    ox::SystemScheduler scheduler(1.0 / 60.0 /*fixed dt*/, 8 /*max fixed steps*/);
    scheduler.emplace<ox::TransformSystem>(); // PostUpdate, order -1000
    scheduler.emplace<SpinSystem>();
    scheduler.attach(world, services);

    for (int frame = 0; frame < 60; ++frame) scheduler.tick(world, services, 1.0 / 60.0); // 1 секунда

    // 60 фиксированных шагов по 1/60 с при 90°/с = поворот на 90°.
    const glm::quat expected = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    EXPECT_TRUE(ox::nearlyEqual(fan.localTransform().rotation, expected, 1e-3f));
    scheduler.detach();
}

TEST(GuideSceneSystems, PhaseOrder) {
    ox::World world;
    ox::Services services;
    services.emplace<FrameLog>();

    ox::SystemScheduler scheduler(1.0 / 60.0);
    scheduler.emplace<LogSystem>("Camera", ox::SystemPhase::Update, 10);
    scheduler.emplace<LogSystem>("Player", ox::SystemPhase::Update, 0);
    scheduler.emplace<LogSystem>("Input", ox::SystemPhase::PreUpdate);
    scheduler.emplace<LogSystem>("Physics", ox::SystemPhase::FixedUpdate);
    scheduler.emplace<LogSystem>("Snapshot", ox::SystemPhase::Extract);

    scheduler.tick(world, services, 1.0 / 60.0);
    EXPECT_EQ(services.get<FrameLog>().lines,
              (std::vector<std::string>{"Input", "Physics", "Player", "Camera", "Snapshot"}));
}

TEST(GuideSceneSystems, FixedStepsAndPlayMode) {
    ox::World world;
    ox::Services services;
    services.emplace<FrameLog>();

    ox::SystemScheduler scheduler(0.01, 5);
    scheduler.emplace<LogSystem>("Physics", ox::SystemPhase::FixedUpdate);
    auto& ai = scheduler.emplace<EnemyAiSystem>();

    scheduler.tick(world, services, 0.035); // 3 шага по 0.01, остаток 0.005 -> alpha = 0.5
    EXPECT_EQ(services.get<FrameLog>().lines.size(), 3u);
    EXPECT_NEAR(scheduler.fixedTimestep().alpha(), 0.5, 1e-6);
    EXPECT_EQ(ai.ticks, 0); // не в режиме игры

    scheduler.setPlaying(true);
    scheduler.setEnabled("Physics", false); // временно выключить по имени
    scheduler.tick(world, services, 0.01);
    EXPECT_EQ(ai.ticks, 1);
    EXPECT_EQ(services.get<FrameLog>().lines.size(), 3u);
}
