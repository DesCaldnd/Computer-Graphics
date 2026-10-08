#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/script_bindings.hpp>

#if OX_HAS_PHYSICS
#include <oxwald/physics/physics_world.hpp>
#endif
#if OX_HAS_AUDIO
#include <oxwald/audio/audio_bus.hpp>
#include <oxwald/audio/audio_engine.hpp>
#endif
#if OX_HAS_SCRIPT
#include <oxwald/script/script_vm.hpp>
#if OX_SCRIPT_HAS_ASYNC
#include <oxwald/script/async_bridge.hpp>
#endif
#endif
#if OX_HAS_GAMEPLAY
#include <oxwald/gameplay/gameplay.hpp>
#endif
#if OX_HAS_ASYNC
#include <oxwald/async/executor.hpp>
#include <oxwald/async/scheduler.hpp>
#include <oxwald/core/jobs.hpp>
#endif

// Built-in integrations of the optional CPU modules. They only create/own the services and their per-frame
// updates; ECS components and systems that bind them to entities live in the gameplay module.
namespace ox {

namespace {

#if OX_HAS_PHYSICS
class PhysicsModule final : public IEngineModule {
public:
    [[nodiscard]] std::string_view name() const override { return "physics"; }
    Status init(Engine& engine, Services& services) override {
        physics::PhysicsWorldDesc desc;
        const auto& p = engine.projectSettings().physics;
        desc.gravity = p.gravity;
        desc.maxBodies = p.maxBodies;
        desc.workerThreads = engine.config().workerThreads == 1 ? 0 : -1;
        services.emplace<physics::PhysicsWorld>(desc);
        return {};
    }
};
#endif

#if OX_HAS_AUDIO
class AudioUpdateSystem final : public ISystem {
public:
    [[nodiscard]] std::string_view name() const override { return "AudioUpdate"; }
    [[nodiscard]] SystemPhase phase() const override { return SystemPhase::PostUpdate; }
    [[nodiscard]] i32 order() const override { return 1000; }
    void update(SystemContext& ctx) override {
        if (auto* a = ctx.services.tryGet<audio::AudioEngine>(); a && a->initialized()) a->update(ctx.dt);
    }
};

class AudioModule final : public IEngineModule {
public:
    [[nodiscard]] std::string_view name() const override { return "audio"; }
    Status init(Engine& engine, Services& services) override {
        auto& a = services.emplace<audio::AudioEngine>();
        audio::AudioEngineConfig c;
        c.offline = engine.config().headless;
        c.sampleRate = engine.projectSettings().audio.sampleRate;
        c.maxVoices = engine.projectSettings().audio.maxVoices;
        if (!a.init(c) && !c.offline) {
            OX_LOG_WARN("engine", "no audio device; falling back to offline audio");
            c.offline = true;
            a.init(c);
        }
        m_audio = &a;
        m_settings = &engine.settings();
        applyVolumes();
        m_connection = engine.settings().changed.connect([this](SettingsCategory cat) {
            if (hasCategory(cat, SettingsCategory::Audio)) applyVolumes();
        });
        return {};
    }
    void registerSystems(Engine&, SystemScheduler& scheduler) override { scheduler.emplace<AudioUpdateSystem>(); }
    void shutdown(Engine&, Services&) override {
        m_connection.disconnect();
        if (m_audio) m_audio->shutdown();
        m_audio = nullptr;
    }

private:
    void applyVolumes() {
        if (!m_audio || !m_audio->initialized()) return;
        if (auto* master = m_audio->master()) master->setVolume(m_settings->user().audio.masterVolume);
        for (const char* bus : {"Music", "SFX", "Voice", "UI", "Ambience"}) {
            if (auto* b = m_audio->bus(bus)) b->setVolume(m_settings->busVolume(bus));
        }
    }
    audio::AudioEngine* m_audio = nullptr;
    Settings* m_settings = nullptr;
    ScopedConnection m_connection;
};
#endif

#if OX_HAS_SCRIPT
// PreUpdate (order 0, play mode): bridge.update() wakes Lua coroutines whose futures completed, then vm.update(dt)
// runs script timers/coroutines on game time. With the gameplay module, its ScriptRuntime does exactly this
// (Gameplay.Script.PreUpdate, also order 0), so the runtime system is not added.
class ScriptVMSystem final : public ISystem {
public:
    ScriptVMSystem(script::ScriptVM& vm, void* bridge) : m_vm(vm), m_bridge(bridge) {}
    [[nodiscard]] std::string_view name() const override { return "Runtime.ScriptVM"; }
    [[nodiscard]] SystemPhase phase() const override { return SystemPhase::PreUpdate; }
    [[nodiscard]] bool playModeOnly() const override { return true; }
    void update(SystemContext& ctx) override {
#if OX_SCRIPT_HAS_ASYNC
        static_cast<script::AsyncBridge*>(m_bridge)->update();
#endif
        m_vm.update(ctx.dt);
    }

private:
    script::ScriptVM& m_vm;
    [[maybe_unused]] void* m_bridge;
};

class ScriptModule final : public IEngineModule {
public:
    [[nodiscard]] std::string_view name() const override { return "script"; }
    Status init(Engine& engine, Services& services) override {
        script::ScriptVMConfig c;
        if (engine.project()) c.searchRoots.push_back(engine.project()->root() / "scripts");
        m_vm = &services.emplace<script::ScriptVM>(c);
        bindInputLuaApi(*m_vm, engine.input());
#if OX_SCRIPT_HAS_ASYNC
        // One bridge per VM (Lua `await` on ox::Future); destroyed before the VM (reverse service order).
        m_bridge = &services.emplace<script::AsyncBridge>(*m_vm);
#endif
        return {};
    }
    void registerSystems(Engine&, SystemScheduler& scheduler) override {
#if !OX_HAS_GAMEPLAY
        scheduler.emplace<ScriptVMSystem>(*m_vm, m_bridge);
#endif
    }
    void shutdown(Engine&, Services&) override {
        m_vm = nullptr;
        m_bridge = nullptr;
    }

private:
    script::ScriptVM* m_vm = nullptr;
    void* m_bridge = nullptr; // script::AsyncBridge when OX_SCRIPT_HAS_ASYNC
};
#endif

#if OX_HAS_ASYNC
// PreUpdate order 10: after the script VM (order 0) -> bridge.update(); vm.update(dt); scheduler.tick(dt, frame).
// The scheduler scales game time itself, so it gets the real delta plus the engine's pause/time scale.
class CoroutineTickSystem final : public ISystem {
public:
    CoroutineTickSystem(Engine& engine, CoroutineScheduler& s) : m_engine(engine), m_scheduler(s) {}
    [[nodiscard]] std::string_view name() const override { return "Runtime.Coroutines"; }
    [[nodiscard]] SystemPhase phase() const override { return SystemPhase::PreUpdate; }
    [[nodiscard]] i32 order() const override { return 10; }
    void update(SystemContext&) override {
        const FrameTime& t = m_engine.frameTime();
        if (t.stepping) {
            // A paused single step advances game time by exactly the stepped amount.
            m_scheduler.setPaused(false);
            m_scheduler.setTimeScale(1.0);
            m_scheduler.tick(t.dt, t.frameIndex);
            return;
        }
        m_scheduler.setPaused(t.paused || !t.playing);
        m_scheduler.setTimeScale(t.timeScale);
        m_scheduler.tick(t.realDt, t.frameIndex);
    }

private:
    Engine& m_engine;
    CoroutineScheduler& m_scheduler;
};

// FixedUpdate order 10: after the physics step (Gameplay.Physics.Step, order 0) resumes nextFixedUpdate() waits.
class CoroutineFixedTickSystem final : public ISystem {
public:
    explicit CoroutineFixedTickSystem(CoroutineScheduler& s) : m_scheduler(s) {}
    [[nodiscard]] std::string_view name() const override { return "Runtime.Coroutines.Fixed"; }
    [[nodiscard]] SystemPhase phase() const override { return SystemPhase::FixedUpdate; }
    [[nodiscard]] i32 order() const override { return 10; }
    [[nodiscard]] bool playModeOnly() const override { return true; }
    void update(SystemContext& ctx) override { m_scheduler.fixedTick(ctx.fixedDt); }

private:
    CoroutineScheduler& m_scheduler;
};

// One game-thread CoroutineScheduler (registered in Services) with the job system as background executor; every
// coroutine is cancelled when the world unloads.
class AsyncModule final : public IEngineModule {
public:
    [[nodiscard]] std::string_view name() const override { return "async"; }
    Status init(Engine& engine, Services& services) override {
        // Executor registered first so it outlives the scheduler; both go before the JobSystem (reverse order).
        auto& executor = services.emplace<JobSystemExecutor>(engine.jobs());
        m_scheduler = &services.emplace<CoroutineScheduler>(&executor);
        return {};
    }
    void registerSystems(Engine& engine, SystemScheduler& scheduler) override {
        scheduler.emplace<CoroutineTickSystem>(engine, *m_scheduler);
        scheduler.emplace<CoroutineFixedTickSystem>(*m_scheduler);
    }
    void onWorldUnloading(Engine&, World&) override {
        if (m_scheduler) m_scheduler->cancelAll();
    }
    void shutdown(Engine&, Services&) override {
        if (m_scheduler) m_scheduler->shutdown(); // leak report while the job system is still alive
        m_scheduler = nullptr;
    }

private:
    CoroutineScheduler* m_scheduler = nullptr;
};
#endif

#if OX_HAS_GAMEPLAY
// ECS components + systems of the gameplay module, using the services created by the modules above. The engine
// ticks the CoroutineScheduler itself (tickCoroutines = false).
class GameplayModule final : public IEngineModule {
public:
    [[nodiscard]] std::string_view name() const override { return "gameplay"; }
    void registerTypes() override { registerGameplayTypes(); }
    void registerSystems(Engine& engine, SystemScheduler& scheduler) override {
        gameplay::GameplayConfig c;
        const ProjectSettings& p = engine.projectSettings();
        c.physics = p.moduleEnabled("physics");
        c.audio = p.moduleEnabled("audio");
        c.scripting = p.moduleEnabled("script");
        c.ai = p.moduleEnabled("ai");
        c.networking = p.moduleEnabled("net");
        c.physicsWorld.gravity = p.physics.gravity;
        c.physicsWorld.maxBodies = p.physics.maxBodies;
        c.tickCoroutines = false;
        c.addTransformSystem = false;
        addGameplaySystems(scheduler, engine.services(), c);
    }
};
#endif

} // namespace

std::vector<std::unique_ptr<IEngineModule>> makeBuiltinModules(const EngineConfig& config,
                                                               const ProjectSettings& project) {
    std::vector<std::unique_ptr<IEngineModule>> out;
    [[maybe_unused]] auto enabled = [&](std::string_view m) { return project.moduleEnabled(m); };
#if OX_HAS_PHYSICS
    if (enabled("physics")) out.push_back(std::make_unique<PhysicsModule>());
#endif
#if OX_HAS_AUDIO
    if (enabled("audio") && !config.dedicatedServer) out.push_back(std::make_unique<AudioModule>());
#endif
#if OX_HAS_SCRIPT
    if (enabled("script")) out.push_back(std::make_unique<ScriptModule>());
#endif
#if OX_HAS_ASYNC
    if (enabled("async")) out.push_back(std::make_unique<AsyncModule>());
#endif
#if OX_HAS_GAMEPLAY
    if (enabled("gameplay")) out.push_back(std::make_unique<GameplayModule>());
#endif
    (void)config;
    return out;
}

} // namespace ox
