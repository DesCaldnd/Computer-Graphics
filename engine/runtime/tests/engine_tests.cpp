#include "test_util.hpp"

#include <oxwald/core/jobs.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

#include <thread>

using namespace ox;

namespace {

std::vector<std::string> g_log;

struct MockServiceA {
    MockServiceA() { g_log.push_back("A+"); }
    ~MockServiceA() { g_log.push_back("A-"); }
};
struct MockServiceB {
    explicit MockServiceB(MockServiceA& a) : dep(a) { g_log.push_back("B+"); }
    ~MockServiceB() { g_log.push_back("B-"); }
    MockServiceA& dep;
};

class MockModuleA final : public IEngineModule {
public:
    std::string_view name() const override { return "mockA"; }
    Status init(Engine& engine, Services& services) override {
        EXPECT_TRUE(services.has<JobSystem>());
        EXPECT_TRUE(services.has<Vfs>());
        EXPECT_TRUE(services.has<InputSystem>());
        services.emplace<MockServiceA>();
        g_log.push_back("init:A");
        return {};
    }
    void shutdown(Engine&, Services& services) override {
        EXPECT_TRUE(services.has<MockServiceA>());
        g_log.push_back("shutdown:A");
    }
};

class MockModuleB final : public IEngineModule {
public:
    std::string_view name() const override { return "mockB"; }
    Status init(Engine&, Services& services) override {
        services.emplace<MockServiceB>(services.get<MockServiceA>());
        g_log.push_back("init:B");
        return {};
    }
    void shutdown(Engine&, Services&) override { g_log.push_back("shutdown:B"); }
};

class FailingModule final : public IEngineModule {
public:
    std::string_view name() const override { return "failing"; }
    Status init(Engine&, Services&) override { return makeError("boom"); }
};

class LoggingRenderer final : public IRenderer {
public:
    ~LoggingRenderer() override { g_log.push_back("renderer-"); }
    std::string_view name() const override { return "Logging"; }
    Status init(Services& services, const RenderSurface&) override {
        EXPECT_TRUE(services.has<MockServiceB>());
        g_log.push_back("renderer.init");
        return {};
    }
    void shutdown() override { g_log.push_back("renderer.shutdown"); }
    void extract(const World&, const FrameContext&) override {}
    void render(const FrameContext&) override {}
    void resize(glm::uvec2) override {}
    void settingsChanged() override {}
};

class SettingsCountingRenderer final : public IRenderer {
public:
    std::string_view name() const override { return "SettingsCounting"; }
    Status init(Services&, const RenderSurface&) override { return {}; }
    void shutdown() override {}
    void extract(const World&, const FrameContext&) override {}
    void render(const FrameContext&) override {}
    void resize(glm::uvec2) override {}
    void settingsChanged() override { ++changes; }
    std::atomic<int> changes{0};
};

class CountingSystem final : public ISystem {
public:
    CountingSystem(SystemPhase phase, std::string name, bool playOnly = false)
        : m_phase(phase), m_name(std::move(name)), m_playOnly(playOnly) {}
    std::string_view name() const override { return m_name; }
    SystemPhase phase() const override { return m_phase; }
    bool playModeOnly() const override { return m_playOnly; }
    void update(SystemContext& ctx) override {
        ++count;
        lastDt = ctx.dt;
        lastAlpha = ctx.alpha;
    }
    int count = 0;
    f32 lastDt = 0.0f;
    f32 lastAlpha = 0.0f;

private:
    SystemPhase m_phase;
    std::string m_name;
    bool m_playOnly;
};

EngineConfig testConfig(const test::TempDir& dir) {
    EngineConfig c;
    c.appName = "OxwaldRuntimeTests";
    c.headless = true;
    c.workerThreads = 2;
    c.userDir = dir / "user";
    c.fixedRate = 64.0; // exact binary fractions
    c.maxFixedSteps = 8;
    return c;
}

} // namespace

TEST(Engine, InitShutdownOrderWithMockServices) {
    g_log.clear();
    test::TempDir dir;
    {
        Engine engine;
        engine.addModule(std::make_unique<MockModuleA>());
        engine.addModule(std::make_unique<MockModuleB>());
        engine.setRenderer(std::make_unique<LoggingRenderer>());
        ASSERT_TRUE(engine.init(testConfig(dir)));
        EXPECT_TRUE(engine.services().has<Settings>());
        EXPECT_TRUE(engine.services().has<SaveGameSystem>());
        EXPECT_TRUE(engine.services().has<Console>());
        EXPECT_TRUE(engine.services().has<IRenderer>());
        EXPECT_EQ(engine.renderer().name(), "Logging");
        EXPECT_TRUE(engine.tick(1.0 / 64.0));
        engine.shutdown();
        EXPECT_FALSE(engine.initialized());
        engine.shutdown(); // idempotent
    }
    const std::vector<std::string> expected = {"A+",          "init:A", "B+", "init:B", "renderer.init", "renderer.shutdown",
                                               "shutdown:B", "shutdown:A", "renderer-", "B-", "A-"};
    EXPECT_EQ(g_log, expected);
}

TEST(Engine, FailingModuleAbortsInitCleanly) {
    test::TempDir dir;
    Engine engine;
    engine.addModule(std::make_unique<FailingModule>());
    auto st = engine.init(testConfig(dir));
    ASSERT_FALSE(st);
    EXPECT_NE(st.error().message.find("boom"), std::string::npos);
    EXPECT_FALSE(engine.initialized());
    EXPECT_EQ(engine.services().size(), 0u);
}

TEST(Engine, FixedStepCountsAndInterpolationAlpha) {
    test::TempDir dir;
    Engine engine;
    ASSERT_TRUE(engine.init(testConfig(dir)));
    auto& fixed = engine.scheduler().emplace<CountingSystem>(SystemPhase::FixedUpdate, "Fixed");
    auto& update = engine.scheduler().emplace<CountingSystem>(SystemPhase::Update, "Update");
    const f64 step = 1.0 / 64.0;

    engine.tick(step);
    EXPECT_EQ(engine.stats().fixedSteps, 1u);
    EXPECT_DOUBLE_EQ(engine.stats().alpha, 0.0);
    engine.tick(2 * step);
    EXPECT_EQ(engine.stats().fixedSteps, 2u);
    engine.tick(step / 2);
    EXPECT_EQ(engine.stats().fixedSteps, 0u);
    EXPECT_DOUBLE_EQ(engine.stats().alpha, 0.5);
    EXPECT_FLOAT_EQ(update.lastAlpha, 0.5f);
    engine.tick(step / 4);
    EXPECT_EQ(engine.stats().fixedSteps, 0u);
    EXPECT_DOUBLE_EQ(engine.stats().alpha, 0.75);
    engine.tick(step / 4);
    EXPECT_EQ(engine.stats().fixedSteps, 1u);
    EXPECT_DOUBLE_EQ(engine.stats().alpha, 0.0);
    EXPECT_EQ(fixed.count, 4);
    EXPECT_FLOAT_EQ(fixed.lastDt, f32(step));

    // Hitch: 0.25 s = 16 steps, clamped to maxFixedSteps (8); the rest is dropped (spiral-of-death guard).
    engine.tick(0.25);
    EXPECT_EQ(engine.stats().fixedSteps, 8u);
    EXPECT_GT(engine.stats().droppedTime, 0.0);
    // Deltas above maxFrameDelta are clamped first.
    engine.tick(10.0);
    EXPECT_EQ(engine.stats().fixedSteps, 8u);
    EXPECT_EQ(update.count, 7);
    EXPECT_EQ(engine.stats().totalFixedSteps, 20u);
}

TEST(Engine, PauseTimeScaleAndStep) {
    test::TempDir dir;
    Engine engine;
    ASSERT_TRUE(engine.init(testConfig(dir)));
    auto& fixed = engine.scheduler().emplace<CountingSystem>(SystemPhase::FixedUpdate, "Fixed");
    auto& update = engine.scheduler().emplace<CountingSystem>(SystemPhase::Update, "Update");
    const f64 step = 1.0 / 64.0;

    engine.setPaused(true);
    for (int i = 0; i < 3; ++i) engine.tick(step);
    EXPECT_EQ(fixed.count, 0);
    EXPECT_EQ(update.count, 3); // frame systems still run (UI, cameras) with dt = 0
    EXPECT_FLOAT_EQ(update.lastDt, 0.0f);
    EXPECT_DOUBLE_EQ(engine.stats().gameTime, 0.0);

    engine.stepFrames(2);
    engine.tick(step);
    EXPECT_EQ(fixed.count, 1);
    engine.tick(0.1); // any real delta: one fixed step exactly
    EXPECT_EQ(fixed.count, 2);
    engine.tick(step);
    EXPECT_EQ(fixed.count, 2);
    EXPECT_DOUBLE_EQ(engine.stats().gameTime, 2 * step);

    engine.setPaused(false);
    engine.setTimeScale(0.5);
    engine.tick(2 * step);
    EXPECT_EQ(fixed.count, 3);
    engine.setTimeScale(2.0);
    engine.tick(step);
    EXPECT_EQ(fixed.count, 5);
    EXPECT_DOUBLE_EQ(engine.stats().gameTime, 5 * step);
    engine.setTimeScale(-1.0);
    EXPECT_EQ(engine.timeScale(), 0.0);
}

TEST(Engine, EditorPlayModeSimulatesACopy) {
    test::TempDir dir;
    Engine engine;
    auto cfg = testConfig(dir);
    cfg.editor = true;
    ASSERT_TRUE(engine.init(cfg));
    auto& playOnly = engine.scheduler().emplace<CountingSystem>(SystemPhase::Update, "PlayOnly", true);
    EXPECT_EQ(engine.mode(), EngineMode::Edit);
    Entity e = engine.world().create("Box");
    e.setPosition({1, 2, 3});
    const Uuid id = e.uuid();
    engine.tick(1.0 / 64.0);
    EXPECT_EQ(playOnly.count, 0);

    int modeChanges = 0;
    auto c = engine.modeChanged.connect([&](EngineMode) { ++modeChanges; });
    engine.enterPlayMode();
    EXPECT_EQ(engine.mode(), EngineMode::Play);
    EXPECT_NE(&engine.world(), &engine.editWorld());
    Entity copy = engine.world().find(id);
    ASSERT_TRUE(copy.valid());
    copy.setPosition({9, 9, 9});
    engine.world().create("Spawned");
    engine.tick(1.0 / 64.0);
    EXPECT_EQ(playOnly.count, 1);

    engine.exitPlayMode();
    EXPECT_EQ(engine.mode(), EngineMode::Edit);
    EXPECT_EQ(&engine.world(), &engine.editWorld());
    EXPECT_EQ(engine.world().find(id).localTransform().position, glm::vec3(1, 2, 3));
    EXPECT_FALSE(engine.world().findByName("Spawned").valid());
    EXPECT_EQ(modeChanges, 2);
    c.disconnect();
}

namespace {
void writeLevel(const std::filesystem::path& path, std::initializer_list<const char*> names) {
    World w;
    for (const char* n : names) w.create(n);
    ASSERT_TRUE(saveScene(w, path));
}
} // namespace

TEST(Engine, ProjectStartupSceneAdditiveAndAsyncLevelChange) {
    test::TempDir dir;
    registerSceneTypes();
    const auto projectDir = dir / "Game";
    std::filesystem::create_directories(projectDir / "levels");
    writeLevel(projectDir / "levels" / "main.oxscene", {"Hero", "Ground"});
    writeLevel(projectDir / "levels" / "extra.oxscene", {"Tree", "Rock", "Bush"});
    writeLevel(projectDir / "levels" / "second.oxscene", {"Boss"});
    Project p = Project::create(projectDir, "Game");
    p.settings.startupScene = "project://levels/main.oxscene";
    ASSERT_TRUE(p.save());

    Engine engine;
    auto cfg = testConfig(dir);
    cfg.projectPath = projectDir;
    ASSERT_TRUE(engine.init(cfg));
    EXPECT_EQ(engine.projectSettings().name, "Game");
    EXPECT_EQ(engine.currentLevel(), "project://levels/main.oxscene");
    EXPECT_EQ(engine.world().entityCount(), 2u);

    auto roots = engine.loadSceneAdditive("project://levels/extra.oxscene");
    ASSERT_TRUE(roots);
    EXPECT_EQ(roots->size(), 3u);
    EXPECT_EQ(engine.world().entityCount(), 5u);
    EXPECT_EQ(engine.additiveScenes().size(), 1u);
    ASSERT_TRUE(engine.unloadAdditive("project://levels/extra.oxscene"));
    EXPECT_EQ(engine.world().entityCount(), 2u);
    EXPECT_FALSE(engine.unloadAdditive("project://levels/extra.oxscene"));

    std::vector<std::string> events;
    engine.setLoadingScreenHooks({[&](const std::string& l) { events.push_back("begin " + l); },
                                  [&](const std::string& l, bool ok) { events.push_back(std::string(ok ? "end " : "fail ") + l); }});
    auto unloadConn = engine.levelUnloading.connect([&](const std::string& l) { events.push_back("unload " + l); });
    engine.requestLevelChange("project://levels/second.oxscene");
    for (int i = 0; i < 500 && (events.empty() || engine.loading()); ++i) {
        engine.tick(1.0 / 64.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_FALSE(engine.loading());
    EXPECT_EQ(engine.currentLevel(), "project://levels/second.oxscene");
    EXPECT_TRUE(engine.world().findByName("Boss").valid());
    const std::vector<std::string> expected = {"begin project://levels/second.oxscene",
                                               "unload project://levels/main.oxscene",
                                               "end project://levels/second.oxscene"};
    EXPECT_EQ(events, expected);

    events.clear();
    engine.requestLevelChange("project://levels/missing.oxscene");
    for (int i = 0; i < 500 && (events.size() < 2); ++i) {
        engine.tick(1.0 / 64.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1], "fail project://levels/missing.oxscene");
    EXPECT_EQ(engine.currentLevel(), "project://levels/second.oxscene"); // old level kept
}

TEST(Engine, ConsoleCommandsControlTheLoop) {
    test::TempDir dir;
    Engine engine;
    ASSERT_TRUE(engine.init(testConfig(dir)));
    ASSERT_TRUE(engine.console().execute("pause"));
    EXPECT_TRUE(engine.paused());
    ASSERT_TRUE(engine.console().execute("timescale 0.25"));
    EXPECT_DOUBLE_EQ(engine.timeScale(), 0.25);
    ASSERT_TRUE(engine.console().execute("quit"));
    EXPECT_TRUE(engine.quitRequested());
    engine.run(1000); // returns immediately
    EXPECT_FALSE(engine.tick(0.01));
}

TEST(Engine, RunPacesToTargetFps) {
    test::TempDir dir;
    Engine engine;
    auto cfg = testConfig(dir);
    cfg.targetFps = 100.0;
    ASSERT_TRUE(engine.init(cfg));
    Stopwatch sw(true);
    engine.run(10);
    // 10 frames at 100 FPS: the first frame has no previous deadline, so >= ~90 ms.
    EXPECT_GE(sw.elapsedMs(), 85.0);
    EXPECT_LT(sw.elapsedMs(), 1000.0);
    EXPECT_EQ(engine.stats().frame, 9u);
}

TEST(Engine, GraphicsCVarChangesFromRegistryApplySettings) {
    test::TempDir dir;
    Engine engine;
    auto renderer = std::make_unique<SettingsCountingRenderer>();
    SettingsCountingRenderer* r = renderer.get();
    engine.setRenderer(std::move(renderer));
    EngineConfig c = testConfig(dir);
    c.threadedRendering = false;
    c.saveUserSettingsOnShutdown = false;
    ASSERT_TRUE(engine.init(c));
    ASSERT_TRUE(engine.tick(1.0 / 64.0));
    const int before = r->changes.load();
    auto& settings = engine.services().get<Settings>();
    ASSERT_TRUE(settings.user().graphics.vsync);

    // Not through the in-game console: plain registry calls (also what Lua cvar.set uses).
    ASSERT_TRUE(CVarRegistry::instance().execute("r.VSync false"));
    ASSERT_TRUE(engine.tick(1.0 / 64.0));
    EXPECT_FALSE(settings.user().graphics.vsync) << "user settings captured from the cvar";
    EXPECT_GT(r->changes.load(), before) << "renderer settings re-applied";

    const int afterFirst = r->changes.load();
    ASSERT_TRUE(CVarRegistry::instance().set("r.VSync", "true"));
    ASSERT_TRUE(engine.tick(1.0 / 64.0));
    EXPECT_TRUE(settings.user().graphics.vsync);
    EXPECT_GT(r->changes.load(), afterFirst);

    // Code-side changes are the caller's business (no implicit capture into the user settings).
    const int afterSecond = r->changes.load();
    CVarRegistry::instance().findAs<bool>("r.VSync")->set(false);
    ASSERT_TRUE(engine.tick(1.0 / 64.0));
    EXPECT_EQ(r->changes.load(), afterSecond);
    CVarRegistry::instance().findAs<bool>("r.VSync")->set(true);
    engine.shutdown();
}
