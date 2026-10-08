// Built-in module integration (only the modules linked into this build are tested).
#include "test_util.hpp"

#include <oxwald/runtime/engine.hpp>

#if OX_HAS_PHYSICS
#include <oxwald/physics/physics_world.hpp>
#endif
#if OX_HAS_AUDIO
#include <oxwald/audio/audio_engine.hpp>
#endif
#if OX_HAS_SCRIPT
#include <oxwald/script/script_vm.hpp>
#endif
#if OX_HAS_ASYNC
#include <oxwald/async/scheduler.hpp>
#include <oxwald/async/task.hpp>
#endif

#include <gtest/gtest.h>

using namespace ox;

namespace {
EngineConfig cfg(const test::TempDir& dir) {
    EngineConfig c;
    c.headless = true;
    c.workerThreads = 2;
    c.userDir = dir / "user";
    c.fixedRate = 64.0;
    return c;
}
} // namespace

TEST(Integration, OptionalModuleServices) {
    test::TempDir dir;
    Engine engine;
    ASSERT_TRUE(engine.init(cfg(dir)));
#if OX_HAS_PHYSICS
    EXPECT_TRUE(engine.services().has<physics::PhysicsWorld>());
    EXPECT_NE(engine.findModule("physics"), nullptr);
#endif
#if OX_HAS_AUDIO
    ASSERT_TRUE(engine.services().has<audio::AudioEngine>());
    EXPECT_TRUE(engine.services().get<audio::AudioEngine>().offline()); // headless -> offline mixing
#endif
#if OX_HAS_SCRIPT
    ASSERT_TRUE(engine.services().has<script::ScriptVM>());
    EXPECT_TRUE(engine.services().get<script::ScriptVM>().hasApi("input"));
#endif
#if OX_HAS_ASYNC
    EXPECT_TRUE(engine.services().has<CoroutineScheduler>());
#endif
    for (int i = 0; i < 3; ++i) engine.tick(1.0 / 64.0);
}

#if OX_HAS_AUDIO
TEST(Integration, AudioSettingsOnlyTouchChangedBuses) {
    test::TempDir dir;
    Engine engine;
    ASSERT_TRUE(engine.init(cfg(dir)));
    auto& a = engine.services().get<audio::AudioEngine>();
    ASSERT_NE(a.bus("Music"), nullptr);
    a.bus("Music")->setVolume(0.1f); // game code ducks the music

    AudioSettings s = engine.settings().user().audio;
    s.masterVolume = 0.5f;
    engine.settings().setAudio(s);
    EXPECT_FLOAT_EQ(a.master()->volume(), 0.5f);
    EXPECT_FLOAT_EQ(a.bus("Music")->volume(), 0.1f) << "unrelated settings change keeps the game's bus volume";

    s.busVolumes["Music"] = 0.8f;
    engine.settings().setAudio(s);
    EXPECT_FLOAT_EQ(a.bus("Music")->volume(), 0.8f) << "the user's own Music slider still applies";
}
#endif

TEST(Integration, ModuleTogglesFromProject) {
    test::TempDir dir;
    ProjectSettings p;
    p.modules = {{"physics", false}, {"audio", false}, {"script", false}, {"async", false}};
    auto c = cfg(dir);
    c.projectSettings = p;
    Engine engine;
    ASSERT_TRUE(engine.init(c));
    EXPECT_EQ(engine.findModule("physics"), nullptr);
    EXPECT_EQ(engine.findModule("audio"), nullptr);
    EXPECT_EQ(engine.findModule("script"), nullptr);
    EXPECT_EQ(engine.findModule("async"), nullptr);
}

#if OX_HAS_ASYNC
TEST(Integration, CoroutinesFollowGameTimePauseAndLevelChanges) {
    test::TempDir dir;
    Engine engine;
    ASSERT_TRUE(engine.init(cfg(dir)));
    auto& sched = engine.services().get<CoroutineScheduler>();
    int stage = 0;
    int fixedResumes = 0;
    bool destroyed = false;
    struct Guard {
        bool* flag;
        ~Guard() { *flag = true; }
    };
    sched.spawn([&]() -> Task<> {
        Guard g{&destroyed};
        stage = 1;
        co_await seconds(0.5);
        stage = 2;
        co_await nextFixedUpdate();
        ++fixedResumes;
        co_await seconds(100.0);
        stage = 3;
    });
    EXPECT_EQ(stage, 1);
    const f64 dt = 1.0 / 64.0;
    engine.setPaused(true);
    for (int i = 0; i < 64; ++i) engine.tick(dt); // paused: game time frozen
    EXPECT_EQ(stage, 1);
    engine.setPaused(false);
    engine.setTimeScale(2.0);
    for (int i = 0; i < 17 && stage < 2; ++i) engine.tick(dt); // 0.5 s game time = 0.25 s real at 2x
    EXPECT_EQ(stage, 2);
    engine.tick(dt);
    engine.tick(dt);
    EXPECT_EQ(fixedResumes, 1); // resumed by the FixedUpdate tick after the physics step
    EXPECT_FALSE(destroyed);

    // Level change (scene unload) cancels every coroutine and unwinds its frame.
    engine.setWorld(std::make_unique<World>(), "test://other");
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(sched.liveCount(), 0u);
    EXPECT_EQ(stage, 2);
}
#endif

#if OX_HAS_SCRIPT
TEST(Integration, ScriptsRunOnlyInPlayMode) {
    test::TempDir dir;
    auto c = cfg(dir);
    c.editor = true;
    Engine engine;
    ASSERT_TRUE(engine.init(c));
    auto& vm = engine.services().get<script::ScriptVM>();
    const f64 before = vm.time();
    engine.tick(0.1);
    EXPECT_DOUBLE_EQ(vm.time(), before);
    engine.enterPlayMode();
    engine.tick(0.1);
    EXPECT_NEAR(vm.time(), before + 0.1, 1e-6);
}
#endif
