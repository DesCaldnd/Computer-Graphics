#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/events.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/system.hpp>
#include <oxwald/scene/world.hpp>

#include <functional>

namespace ox {

namespace {

using namespace ox::gameplay;

class FnSystem final : public ISystem {
public:
    using UpdateFn = std::function<void(SystemContext&)>;
    using AttachFn = std::function<void(World&, Services&)>;
    using DetachFn = std::function<void()>;

    FnSystem(std::string_view name, SystemPhase phase, i32 order, bool playOnly, UpdateFn fn, AttachFn attach = {},
             DetachFn detach = {})
        : m_name(name), m_phase(phase), m_order(order), m_playOnly(playOnly), m_fn(std::move(fn)),
          m_attach(std::move(attach)), m_detach(std::move(detach)) {}

    std::string_view name() const override { return m_name; }
    SystemPhase phase() const override { return m_phase; }
    i32 order() const override { return m_order; }
    bool playModeOnly() const override { return m_playOnly; }
    void onAttach(World& w, Services& s) override {
        if (m_attach) m_attach(w, s);
    }
    void onDetach(World&, Services&) override {
        if (m_detach) m_detach();
    }
    void update(SystemContext& ctx) override {
        if (m_fn) m_fn(ctx);
    }

private:
    std::string m_name;
    SystemPhase m_phase;
    i32 m_order;
    bool m_playOnly;
    UpdateFn m_fn;
    AttachFn m_attach;
    DetachFn m_detach;
};

template <class T, class... Args>
T& ensureService(Services& services, Args&&... args) {
    if (T* existing = services.tryGet<T>()) return *existing;
    return services.emplace<T>(std::forward<Args>(args)...);
}

void add(SystemScheduler& scheduler, std::string_view name, SystemPhase phase, i32 order, bool playOnly,
         FnSystem::UpdateFn fn, FnSystem::AttachFn attach = {}, FnSystem::DetachFn detach = {}) {
    if (scheduler.find(name)) return; // idempotent
    scheduler.add(std::make_unique<FnSystem>(name, phase, order, playOnly, std::move(fn), std::move(attach),
                                             std::move(detach)));
}

} // namespace

void addGameplaySystems(SystemScheduler& scheduler, Services& services, const GameplayConfig& config) {
    registerGameplayTypes();
    if (config.createEventBus) ensureService<EventBus>(services);

    PhysicsRuntime* physicsRt = nullptr;
    AnimationRuntime* animRt = nullptr;
    SplineRuntime* splineRt = nullptr;
    AudioRuntime* audioRt = nullptr;
    AIRuntime* aiRt = nullptr;
    NetworkRuntime* netRt = nullptr;
    ScriptRuntime* scriptRt = nullptr;
#if defined(OX_GAMEPLAY_HAS_ASYNC)
    CoroutineRuntime* coRt = nullptr;
#endif

    if (config.physics) {
        auto& world = ensureService<physics::PhysicsWorld>(services, config.physicsWorld);
        physicsRt = &ensureService<PhysicsRuntime>(services, world);
    }
    if (config.animation) animRt = &ensureService<AnimationRuntime>(services);
    if (config.splines) splineRt = &ensureService<SplineRuntime>(services);
    if (config.audio) audioRt = &ensureService<AudioRuntime>(services);
    if (config.ai) aiRt = &ensureService<AIRuntime>(services);
    if (config.networking) netRt = &ensureService<NetworkRuntime>(services);
#if defined(OX_GAMEPLAY_HAS_ASYNC)
    if (config.coroutines) coRt = &ensureService<CoroutineRuntime>(services);
#endif
    if (config.scripting) {
        auto& vm = ensureService<script::ScriptVM>(services, config.scriptVM);
        scriptRt = &ensureService<ScriptRuntime>(services, vm);
        scriptRt->bindRuntimes(services);
    }

    if (config.addTransformSystem && !scheduler.find("Transform")) scheduler.emplace<TransformSystem>();

    // One lifecycle system attaches/detaches every runtime in dependency order and applies edit <-> play
    // transitions at the start of the frame (bodies before scripts, so onCreate can use physics).
    auto state = std::make_shared<PlayStateTracker>();
    add(
        scheduler, systems::kLifecycle, SystemPhase::PreUpdate, -1000, false,
        [=](SystemContext& ctx) {
            const bool playing = ctx.playing;
            if (state->update(playing) == 0) return;
            if (playing) {
                if (physicsRt) physicsRt->syncPlayState(true);
                if (splineRt) splineRt->syncPlayState(true);
                if (audioRt) audioRt->syncPlayState(true);
                if (aiRt) aiRt->syncPlayState(true);
#if defined(OX_GAMEPLAY_HAS_ASYNC)
                if (coRt) coRt->syncPlayState(true);
#endif
                if (scriptRt) scriptRt->syncPlayState(true);
            } else {
                if (scriptRt) scriptRt->syncPlayState(false);
#if defined(OX_GAMEPLAY_HAS_ASYNC)
                if (coRt) coRt->syncPlayState(false);
#endif
                if (aiRt) aiRt->syncPlayState(false);
                if (audioRt) audioRt->syncPlayState(false);
                if (splineRt) splineRt->syncPlayState(false);
                if (physicsRt) physicsRt->syncPlayState(false);
            }
        },
        [=](World& w, Services& s) {
            state->reset();
            if (physicsRt) physicsRt->attach(w, s);
            if (animRt) animRt->attach(w, s);
            if (splineRt) splineRt->attach(w, s);
            if (audioRt) audioRt->attach(w, s);
            if (aiRt) aiRt->attach(w, s);
            if (netRt) netRt->attach(w, s);
#if defined(OX_GAMEPLAY_HAS_ASYNC)
            if (coRt) coRt->attach(w, s);
#endif
            if (scriptRt) scriptRt->attach(w, s);
        },
        [=] {
            if (scriptRt) scriptRt->detach();
#if defined(OX_GAMEPLAY_HAS_ASYNC)
            if (coRt) coRt->detach();
#endif
            if (netRt) netRt->detach();
            if (aiRt) aiRt->detach();
            if (audioRt) audioRt->detach();
            if (splineRt) splineRt->detach();
            if (animRt) animRt->detach();
            if (physicsRt) physicsRt->detach();
            state->reset();
        });

    // Hot reload: changes reported by asset-backed providers (GameplayAssetEvents service, emitted from
    // AssetManager::update on the game thread) are queued and applied at the start of the next frame.
    struct ReloadQueue {
        std::vector<GameplayAssetChange> pending;
        ScopedConnection connection;
    };
    auto reload = std::make_shared<ReloadQueue>();
    add(
        scheduler, systems::kAssetHotReload, SystemPhase::PreUpdate, -950, false,
        [=](SystemContext& c) {
            if (reload->pending.empty()) return;
            std::vector<GameplayAssetChange> changes = std::exchange(reload->pending, {});
            bool scripts = false;
            entt::registry& r = c.world.registry();
            for (const GameplayAssetChange& ch : changes) {
                switch (ch.kind) {
                case GameplayAssetKind::Prefab:
                    if (auto* prefabs = c.services.tryGet<IPrefabProvider>()) {
                        auto doc = prefabs->prefab(ch.id.toString());
                        if (!doc && !ch.path.empty()) doc = prefabs->prefab(ch.path);
                        const usize n = doc ? updatePrefabInstances(c.world, *doc) : 0;
                        OX_LOG_INFO("gameplay", "prefab {} reloaded ({} instance(s))", ch.path.empty() ? ch.id.toString() : ch.path, n);
                    }
                    break;
                case GameplayAssetKind::Script: scripts = true; break;
                case GameplayAssetKind::BehaviorTree:
                    if (aiRt) aiRt->reloadBehaviorTree(ch.id);
                    break;
                case GameplayAssetKind::Skeleton:
                case GameplayAssetKind::AnimatorController:
                    if (animRt) animRt->invalidateAssets(ch.id);
                    break;
                case GameplayAssetKind::AnimationClip:
                    if (animRt) animRt->invalidateAssets(ch.id, true);
                    break;
                case GameplayAssetKind::Mesh: {
                    // Patching the collider recreates its body (and shape) at the next fixed step.
                    std::vector<entt::entity> users;
                    for (auto [e, col] : r.view<ColliderComponent>().each()) {
                        if (col.mesh == ch.id) users.push_back(e);
                    }
                    for (auto e : users) r.patch<ColliderComponent>(e, [](ColliderComponent&) {});
                    break;
                }
                default: break;
                }
            }
            if (scripts && scriptRt) scriptRt->reloadChangedScripts();
        },
        [=](World&, Services& s) {
            reload->pending.clear();
            if (auto* events = s.tryGet<GameplayAssetEvents>()) {
                reload->connection = events->changed.connect(
                    [q = std::weak_ptr<ReloadQueue>(reload)](const GameplayAssetChange& change) {
                        if (auto queue = q.lock()) queue->pending.push_back(change);
                    });
            }
        },
        [=] {
            reload->connection = {};
            reload->pending.clear();
        });

    // ---- PreUpdate ----
    if (netRt) add(scheduler, systems::kNetPre, SystemPhase::PreUpdate, -900, false, [=](SystemContext& c) { netRt->preUpdate(c.dt); });
    if (scriptRt) add(scheduler, systems::kScriptPre, SystemPhase::PreUpdate, 0, true, [=](SystemContext& c) { scriptRt->preUpdate(c.dt); });
#if defined(OX_GAMEPLAY_HAS_ASYNC)
    if (coRt && config.tickCoroutines) {
        add(scheduler, systems::kCoroutines, SystemPhase::PreUpdate, 10, false, [=](SystemContext& c) { coRt->tick(c.dt, c.frame); });
    }
#endif

    // ---- FixedUpdate ---- (scripts decide, AI steers, physics integrates)
    if (scriptRt) add(scheduler, systems::kScriptFixed, SystemPhase::FixedUpdate, -100, true, [=](SystemContext& c) { scriptRt->fixedUpdate(c.dt); });
    if (netRt && config.prediction) {
        add(scheduler, systems::kNetPredict, SystemPhase::FixedUpdate, -90, true, [=](SystemContext& c) { netRt->fixedUpdate(c.dt); });
    }
    if (aiRt) {
        add(scheduler, systems::kPerception, SystemPhase::FixedUpdate, -70, true, [=](SystemContext& c) { aiRt->updatePerception(c.dt); });
        add(scheduler, systems::kBehaviorTrees, SystemPhase::FixedUpdate, -60, true, [=](SystemContext& c) { aiRt->updateBehaviorTrees(c.dt); });
        add(scheduler, systems::kNavigation, SystemPhase::FixedUpdate, -50, true, [=](SystemContext& c) { aiRt->updateNavigation(c.dt); });
    }
    if (physicsRt) add(scheduler, systems::kPhysicsStep, SystemPhase::FixedUpdate, 0, true, [=](SystemContext& c) { physicsRt->fixedStep(c.dt); });
#if defined(OX_GAMEPLAY_HAS_ASYNC)
    if (coRt && config.tickCoroutines) {
        add(scheduler, systems::kCoroutinesFixed, SystemPhase::FixedUpdate, 10, false, [=](SystemContext& c) { coRt->fixedTick(c.dt); });
    }
#endif

    // ---- Update ----
    if (scriptRt) add(scheduler, systems::kScriptUpdate, SystemPhase::Update, 0, true, [=](SystemContext& c) { scriptRt->update(c.dt); });
    if (splineRt) add(scheduler, systems::kSplineFollowers, SystemPhase::Update, 50, true, [=](SystemContext& c) { splineRt->updateFollowers(c.dt); });
    if (animRt) add(scheduler, systems::kAnimation, SystemPhase::Update, 100, false, [=](SystemContext& c) { animRt->update(c.dt, c.playing); });

    // ---- PostUpdate ---- (interpolated physics poses before the transform propagation at -1000)
    if (physicsRt) add(scheduler, systems::kPhysicsInterpolate, SystemPhase::PostUpdate, -1100, true, [=](SystemContext& c) { physicsRt->interpolate(c.alpha); });
    if (audioRt) add(scheduler, systems::kAudio, SystemPhase::PostUpdate, 100, false, [=](SystemContext& c) { audioRt->update(c.dt); });
    if (netRt) add(scheduler, systems::kNetPost, SystemPhase::PostUpdate, 200, false, [=](SystemContext& c) { netRt->postUpdate(c.dt); });

#if defined(OX_GAMEPLAY_HAS_WORLD)
    // Terrain, vegetation, sky/time of day, water, wind, buoyancy, streaming + WorldRenderData (Extract).
    if (config.world) {
        WorldSystemsConfig wc = config.worldSystems;
        wc.physics = wc.physics && config.physics;
        wc.bindLua = wc.bindLua && config.scripting;
        addWorldSystems(scheduler, services, wc);
    }
#endif

    // ---- Extract ---- debug visualisation (editor-visible in edit mode too)
    add(scheduler, systems::kDebugDraw, SystemPhase::Extract, 0, false, [=](SystemContext& c) {
        auto* draw = c.services.tryGet<DebugDraw>();
        if (!draw) return;
        if (physicsRt) physicsRt->drawDebug(*draw, c.playing);
        if (splineRt) splineRt->drawDebug(*draw, c.playing);
        if (aiRt) aiRt->drawDebug(*draw, c.playing);
        if (animRt) animRt->drawDebug(*draw);
        if (audioRt) audioRt->drawDebug(*draw);
    });
}

} // namespace ox
