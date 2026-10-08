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
#include <oxwald/scene/runtime_id.hpp>
#endif
#if OX_HAS_ASSETS
#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/assets/asset_registry.hpp>
#include <oxwald/assets/pak.hpp>
#include <oxwald/core/vfs.hpp>
#endif
#if OX_HAS_GAMEPLAY && OX_GAMEPLAY_HAS_ASSETS
#include <oxwald/gameplay/asset_providers.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <unordered_map>

// Built-in integrations of the optional CPU modules. They only create/own the services and their per-frame
// updates; ECS components and systems that bind them to entities live in the gameplay module.
namespace ox {

namespace {

#if OX_HAS_ASSETS
// project:// view of an asset source by asset path: "project://<assetDir>/<asset path>" reads the cooked artifact
// (scenes/prefabs are OXB1 documents, so Engine::loadScene works unchanged on cooked games). Thread-safe because
// IAssetSource implementations are.
class AssetSourceMount final : public IMountSource {
public:
    AssetSourceMount(assets::IAssetSource& source, std::string assetDir)
        : m_source(source), m_prefix(std::move(assetDir)) {
        while (!m_prefix.empty() && m_prefix.back() == '/') m_prefix.pop_back();
    }
    [[nodiscard]] bool exists(std::string_view relPath) const override { return uuidOf(relPath).has_value(); }
    [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view relPath) const override {
        auto id = uuidOf(relPath);
        if (!id) return makeError("'{}' is not a cooked asset", relPath);
        return m_source.readArtifact(*id);
    }
    [[nodiscard]] std::vector<std::string> list(std::string_view relDir, bool recursive) const override {
        std::vector<std::string> out;
        std::string dir(relDir);
        if (!dir.empty() && !dir.ends_with('/')) dir += '/';
        for (const Uuid& id : m_source.allAssets()) {
            auto rec = m_source.record(id);
            if (!rec || rec->path.empty() || rec->path.find('#') != std::string::npos) continue;
            const std::string full = m_prefix.empty() ? rec->path : m_prefix + "/" + rec->path;
            if (!full.starts_with(dir)) continue;
            if (!recursive && full.find('/', dir.size()) != std::string::npos) continue;
            out.push_back(full);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    [[nodiscard]] std::optional<Uuid> uuidOf(std::string_view relPath) const {
        if (!m_prefix.empty()) {
            if (relPath.size() <= m_prefix.size() || !relPath.starts_with(m_prefix) || relPath[m_prefix.size()] != '/') {
                return std::nullopt;
            }
            relPath.remove_prefix(m_prefix.size() + 1);
        }
        return m_source.uuidForPath(relPath);
    }
    assets::IAssetSource& m_source;
    std::string m_prefix;
};

// Asset database integration. Editor/dev builds: AssetRegistry over the project's first asset dir (importers,
// .oxcache, hot reimport when watching); cooked games (EngineConfig::pakPath): PakAssetSource only. Either way an
// AssetManager service (update() every frame on the game thread) and, with the gameplay module, the gameplay
// providers (meshes, animation, behaviour trees, prefabs, scripts, audio, heightmaps) + hot reload events.
class AssetsModule final : public IEngineModule {
public:
    [[nodiscard]] std::string_view name() const override { return "assets"; }
    void registerTypes() override { assets::registerAssetTypes(); }
    Status init(Engine& engine, Services& services) override {
        const EngineConfig& cfg = engine.config();
        const ProjectSettings& project = engine.projectSettings();
        const std::string assetDir = project.assetDirs.empty() ? std::string("Assets") : project.assetDirs.front();
        assets::IAssetSource* source = nullptr;
        if (!cfg.pakPath.empty()) {
            auto& pak = services.emplace<assets::PakAssetSource>();
            std::vector<std::filesystem::path> paks{cfg.pakPath};
            paks.insert(paks.end(), cfg.patchPaks.begin(), cfg.patchPaks.end());
            for (const auto& path : paks) {
                auto reader = assets::PakReader::open(path);
                if (!reader) return makeError("pak '{}': {}", path.string(), reader.error().message);
                if (auto st = pak.addPak(*reader); !st) return st;
                // Raw pak entries (e.g. the project file) under project://, the cooked assets by path above them.
                engine.vfs().mount("project", std::make_unique<assets::PakMountSource>(*reader), 10);
            }
            engine.vfs().mount("project", std::make_unique<AssetSourceMount>(pak, assetDir), 20);
            source = &pak;
            OX_LOG_INFO("engine", "cooked assets: {} ({} patch paks)", cfg.pakPath.string(), cfg.patchPaks.size());
        } else if (engine.project()) {
            assets::AssetRegistry::Options o;
            o.assetsDir = assetDir;
            m_registry = &services.emplace<assets::AssetRegistry>(engine.project()->root(), o);
#if OX_HAS_GAMEPLAY && OX_GAMEPLAY_HAS_ASSETS
            gameplay::registerGameplayImporters(m_registry->importers());
#endif
            const assets::ScanResult scan = m_registry->scan();
            if (project.assetDirs.size() > 1) {
                OX_LOG_WARN("engine", "only the first asset dir ('{}') is an asset database root; {} more ignored",
                            assetDir, project.assetDirs.size() - 1);
            }
            if (cfg.editor || cfg.fileWatching) {
                m_registry->startWatching();
                m_registry->poll(); // baseline
                m_watching = true;
            }
            source = m_registry;
            OX_LOG_INFO("engine", "asset database {}/{}: {} assets ({} new metas)", engine.project()->root().string(),
                        assetDir, scan.found, scan.metasCreated);
        } else {
            return {}; // no project, no pak: nothing to load assets from
        }
        services.addExternal<assets::IAssetSource>(*source);
        assets::AssetManager::Options mo;
        mo.memoryBudget = cfg.assetMemoryBudget;
        m_manager = &services.emplace<assets::AssetManager>(*source, &engine.jobs(), mo);
#if OX_HAS_GAMEPLAY && OX_GAMEPLAY_HAS_ASSETS
        auto& providers = services.emplace<gameplay::AssetProviders>(*m_manager);
        providers.registerIn(services);
#endif
        return {};
    }
    void preUpdate(Engine&, const FrameTime&) override {
        OX_PROFILE_ZONE_N("Assets");
        if (m_watching) m_registry->poll(); // hot reimport -> AssetManager reloads -> onReloaded in update()
        if (m_manager) m_manager->update();
    }
    void shutdown(Engine&, Services&) override {
        if (m_registry) m_registry->stopWatching();
        if (m_manager) m_manager->waitAll(); // in-flight loads use the job system and the source
        m_registry = nullptr;
        m_manager = nullptr;
        m_watching = false;
    }

private:
    assets::AssetRegistry* m_registry = nullptr;
    bool m_watching = false;
    assets::AssetManager* m_manager = nullptr;
};
#endif

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
    void onWorldChanged(Engine& engine, World& world) override {
        m_connections.clear();
        // Entity-owned coroutines (owner = entityRuntimeId(e), the same id gameplay::coroutineOwner(e) uses) die
        // with their entity, before its components are freed. The gameplay module's CoroutineRuntime does this
        // when it is active; without it the runtime does.
        if (!m_scheduler || engine.findModule("gameplay")) return;
        entt::registry& r = world.registry();
        m_connections.emplace_back(r.on_construct<PendingDestroyTag>().connect<&AsyncModule::onEntityDestroyed>(*this));
        m_connections.emplace_back(r.on_destroy<IdComponent>().connect<&AsyncModule::onEntityDestroyed>(*this));
    }
    void onWorldUnloading(Engine& engine, World& world) override {
        m_connections.clear();
        if (!m_scheduler) return;
        if (&world != &engine.editWorld()) {
            // Leaving editor play mode: only the play copy's entity coroutines go; editor/global ones keep running.
            for (auto e : world.registry().view<IdComponent>()) m_scheduler->cancelOwner(entityRuntimeId(e));
            return;
        }
        m_scheduler->cancelAll(); // level change / shutdown
    }
    void shutdown(Engine&, Services&) override {
        m_connections.clear();
        if (m_scheduler) m_scheduler->shutdown(); // leak report while the job system is still alive
        m_scheduler = nullptr;
    }

private:
    void onEntityDestroyed(entt::registry&, entt::entity e) {
        if (m_scheduler) m_scheduler->cancelOwner(entityRuntimeId(e));
    }
    CoroutineScheduler* m_scheduler = nullptr;
    std::vector<entt::scoped_connection> m_connections;
};
#endif

#if OX_HAS_GAMEPLAY
// Local player input for PredictedCharacter entities from InputSystem actions (component's moveAction /
// jumpAction), relative to the primary camera's yaw. Jump presses are latched per frame so a press is not lost
// in frames without a fixed step, and consumed by the next sample.
class InputCharacterSource final : public gameplay::ICharacterInputSource {
public:
    explicit InputCharacterSource(InputSystem& input) : m_input(input) {}
    gameplay::CharacterInput sample(Entity e, const gameplay::PredictedCharacterComponent& c, f32) override {
        gameplay::CharacterInput in;
        if (!c.moveAction.empty()) in.move = m_input.axis2D(c.moveAction);
        if (!c.jumpAction.empty()) {
            auto [it, inserted] = m_jumpLatch.try_emplace(c.jumpAction, false);
            in.jump = it->second || (inserted && m_input.triggered(c.jumpAction));
            it->second = false;
        }
        in.yaw = cameraYaw(e);
        return in;
    }
    // Once per frame after InputSystem::update.
    void latch() {
        for (auto& [action, pressed] : m_jumpLatch) pressed = pressed || m_input.triggered(action);
    }

private:
    static f32 cameraYaw(Entity e) {
        World* w = e.world();
        if (!w) return 0.f;
        for (auto [h, cam] : w->registry().view<CameraComponent>().each()) {
            if (!cam.primary) continue;
            const glm::vec3 f = w->wrap(h).worldRotation() * glm::vec3(0.f, 0.f, -1.f);
            if (f.x * f.x + f.z * f.z < 1e-8f) return 0.f;
            return std::atan2(-f.x, -f.z); // yaw 0 = -Z, matches gameplay::desiredCharacterVelocity
        }
        return 0.f;
    }
    InputSystem& m_input;
    std::unordered_map<std::string, bool> m_jumpLatch;
};

// ECS components + systems of the gameplay module, using the services created by the modules above. The engine
// ticks the CoroutineScheduler itself (tickCoroutines = false).
class GameplayModule final : public IEngineModule {
public:
    [[nodiscard]] std::string_view name() const override { return "gameplay"; }
    void registerTypes() override { registerGameplayTypes(); }
    Status init(Engine& engine, Services& services) override {
        if (!services.has<gameplay::ICharacterInputSource>()) {
            m_input = &services.emplace<InputCharacterSource>(engine.input());
            services.addExternal<gameplay::ICharacterInputSource>(*m_input);
        }
        return {};
    }
    void preUpdate(Engine&, const FrameTime&) override {
        if (m_input) m_input->latch();
    }
    void shutdown(Engine&, Services&) override { m_input = nullptr; }
    void registerSystems(Engine& engine, SystemScheduler& scheduler) override {
        gameplay::GameplayConfig c;
        const ProjectSettings& p = engine.projectSettings();
        c.physics = p.moduleEnabled("physics");
        c.audio = p.moduleEnabled("audio");
        c.scripting = p.moduleEnabled("script");
        c.ai = p.moduleEnabled("ai");
        c.networking = p.moduleEnabled("net");
        c.world = p.moduleEnabled("world");
        c.physicsWorld.gravity = p.physics.gravity;
        c.physicsWorld.maxBodies = p.physics.maxBodies;
        c.tickCoroutines = false;
        c.addTransformSystem = false;
        addGameplaySystems(scheduler, engine.services(), c);
    }

private:
    InputCharacterSource* m_input = nullptr;
};
#endif

} // namespace

std::vector<std::unique_ptr<IEngineModule>> makeBuiltinModules(const EngineConfig& config,
                                                               const ProjectSettings& project) {
    std::vector<std::unique_ptr<IEngineModule>> out;
    [[maybe_unused]] auto enabled = [&](std::string_view m) { return project.moduleEnabled(m); };
#if OX_HAS_ASSETS
    if (enabled("assets")) out.push_back(std::make_unique<AssetsModule>()); // first: providers for gameplay, VFS
#endif
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
