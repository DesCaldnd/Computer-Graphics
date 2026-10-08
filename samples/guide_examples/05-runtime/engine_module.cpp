// Глава 05: Engine, свой IEngineModule (служба + система), игровой цикл, пауза/time scale (docs/guide/05-runtime.md).
#include <oxwald/runtime/engine.hpp>
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

// ---- служба игры: живёт в engine.services() ----
struct ScoreService {
    int score = 0;
    int fixedTicks = 0;
};

// ---- система, использующая службу ----
class ScoreSystem final : public ox::ISystem {
public:
    std::string_view name() const override { return "Game.Score"; }
    ox::SystemPhase phase() const override { return ox::SystemPhase::FixedUpdate; }
    void update(ox::SystemContext& ctx) override { ++ctx.services.get<ScoreService>().fixedTicks; }
};

// ---- модуль: точка расширения движка ----
class GameModule final : public ox::IEngineModule {
public:
    std::string_view name() const override { return "game"; }
    void registerTypes() override { ox::registerSceneTypes(); } // рефлексия своих компонентов
    ox::Status init(ox::Engine& engine, ox::Services& services) override {
        services.emplace<ScoreService>();
        m_levelConn = engine.levelLoaded.connect([this](const std::string&) { ++levelsLoaded; });
        return {};
    }
    void registerSystems(ox::Engine&, ox::SystemScheduler& scheduler) override { scheduler.emplace<ScoreSystem>(); }
    void preUpdate(ox::Engine& engine, const ox::FrameTime& time) override {
        if (!time.paused) engine.services().get<ScoreService>().score += 1; // раз в кадр, до систем
    }
    void onWorldUnloading(ox::Engine&, ox::World&) override { /* отпустить ссылки на сущности старого мира */ }
    void shutdown(ox::Engine&, ox::Services&) override { m_levelConn.disconnect(); }

    int levelsLoaded = 0;

private:
    ox::Connection m_levelConn;
};

ox::EngineConfig makeConfig(const std::filesystem::path& userDir) {
    ox::EngineConfig config;
    config.appName = "GuideRuntime";
    config.headless = true;       // NullRenderer, без окна
    config.userDir = userDir;
    config.fixedRate = 64.0;      // вместо 60 Гц из проекта: точные двоичные дроби
    config.workerThreads = 2;
    config.saveUserSettingsOnShutdown = false;
    return config;
}

} // namespace

TEST(GuideRuntimeEngine, ModuleServiceAndSystem) {
    const auto userDir = std::filesystem::temp_directory_path() / ("oxwald_guide_rt_" + ox::Uuid::generate().toString());

    ox::Engine engine;
    engine.addModule(std::make_unique<GameModule>()); // до init()
    auto status = engine.init(makeConfig(userDir));
    ASSERT_TRUE(status) << status.error().message;

    // tick(dt) — кадр с явной дельтой (тесты, редактор, lockstep). run() / tick() меряют время сами.
    for (int i = 0; i < 4; ++i) engine.tick(1.0 / 64.0);

    auto& score = engine.services().get<ScoreService>();
    EXPECT_EQ(score.score, 4);      // preUpdate: раз в кадр
    EXPECT_EQ(score.fixedTicks, 4); // FixedUpdate: 4 шага по 1/64 с
    EXPECT_NE(engine.findModule("game"), nullptr);

    engine.shutdown(); // также вызывается из ~Engine
    std::filesystem::remove_all(userDir);
}

TEST(GuideRuntimeEngine, FixedStepAlphaPauseTimeScale) {
    const auto userDir = std::filesystem::temp_directory_path() / ("oxwald_guide_rt_" + ox::Uuid::generate().toString());
    ox::Engine engine;
    engine.addModule(std::make_unique<GameModule>());
    ASSERT_TRUE(engine.init(makeConfig(userDir)));
    auto& score = engine.services().get<ScoreService>();
    const double step = 1.0 / 64.0;

    engine.tick(step / 2); // полшага: FixedUpdate не вызывается, alpha = 0.5
    EXPECT_EQ(engine.stats().fixedSteps, 0u);
    EXPECT_DOUBLE_EQ(engine.stats().alpha, 0.5);
    engine.tick(step * 1.5); // накопилось 2 шага
    EXPECT_EQ(engine.stats().fixedSteps, 2u);

    // Пауза: FixedUpdate стоит, Update-системы идут с dt = 0 (камера, UI).
    engine.setPaused(true);
    const int before = score.fixedTicks;
    engine.tick(step);
    EXPECT_EQ(score.fixedTicks, before);
    engine.stepFrames(1); // покадровая отладка: ровно один фиксированный шаг
    engine.tick(step);
    EXPECT_EQ(score.fixedTicks, before + 1);

    // Замедление времени: 2 кадра по 1/64 с реального времени = 1 шаг игрового.
    engine.setPaused(false);
    engine.setTimeScale(0.5);
    engine.tick(step);
    engine.tick(step);
    EXPECT_EQ(score.fixedTicks, before + 2);

    engine.shutdown();
    std::filesystem::remove_all(userDir);
}
