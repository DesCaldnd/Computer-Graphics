#include "lua_types.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/gameplay/ai.hpp>
#include <oxwald/gameplay/animation.hpp>
#include <oxwald/gameplay/audio.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/providers.hpp>
#include <oxwald/gameplay/script.hpp>
#include <oxwald/gameplay/spline.hpp>
#include <oxwald/scene/prefab.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>

namespace ox::gameplay {

namespace {
// Marks "Lua code is running": instance destruction requested meanwhile is deferred until the outermost call
// returns (a script must not be destroyed while one of its functions executes).
struct CallGuard {
    explicit CallGuard(u32& depth, std::function<void()> onExit) : m_depth(depth), m_onExit(std::move(onExit)) { ++m_depth; }
    ~CallGuard() {
        if (--m_depth == 0 && m_onExit) m_onExit();
    }
    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;

private:
    u32& m_depth;
    std::function<void()> m_onExit;
};
} // namespace

// ---- ScriptPropertyValue -----------------------------------------------------------------------------------

script::ScriptValue ScriptPropertyValue::toScriptValue() const {
    using T = script::ScriptPropertyType;
    switch (type) {
    case T::Float: return number;
    case T::Int: return static_cast<i64>(number);
    case T::Bool: return boolean;
    case T::String: return text;
    case T::Vec2: return glm::vec2(vector);
    case T::Vec3: return glm::vec3(vector);
    case T::Vec4:
    case T::Color: return vector;
    }
    return {};
}

ScriptPropertyValue ScriptPropertyValue::fromScriptValue(const script::ScriptValue& v, script::ScriptPropertyType type) {
    ScriptPropertyValue out;
    out.type = type;
    std::visit(
        [&](const auto& x) {
            using X = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<X, bool>) out.boolean = x;
            else if constexpr (std::is_same_v<X, i64> || std::is_same_v<X, f64>) out.number = static_cast<f64>(x);
            else if constexpr (std::is_same_v<X, std::string>) out.text = x;
            else if constexpr (std::is_same_v<X, glm::vec2>) out.vector = glm::vec4(x, 0.f, 0.f);
            else if constexpr (std::is_same_v<X, glm::vec3>) out.vector = glm::vec4(x, 0.f);
            else if constexpr (std::is_same_v<X, glm::vec4>) out.vector = x;
        },
        v);
    return out;
}

// ---- runtime -------------------------------------------------------------------------------------------------

ScriptRuntime::ScriptRuntime(script::ScriptVM& vm) : m_vm(vm) {}

ScriptRuntime::~ScriptRuntime() {
    detach();
    if (m_ai) m_ai->scriptActionHook = {};
}

void ScriptRuntime::bindRuntimes(Services& services) {
    m_physics = services.tryGet<PhysicsRuntime>();
    m_animation = services.tryGet<AnimationRuntime>();
    m_splines = services.tryGet<SplineRuntime>();
    m_audio = services.tryGet<AudioRuntime>();
    m_ai = services.tryGet<AIRuntime>();
    m_runtimeConnections.clear();
    if (m_physics) {
        m_runtimeConnections.emplace_back(m_physics->onCollision.connect([this](const CollisionEvent& e) { onCollision(e); }));
        m_runtimeConnections.emplace_back(m_physics->onTrigger.connect([this](const TriggerEvent& e) { onTrigger(e); }));
    }
    if (m_splines) {
        m_runtimeConnections.emplace_back(m_splines->onEvent.connect([this](const SplineFollowerEvent& e) {
            invoke(e.follower, "onSplineEvent", e.name, e.distance);
        }));
    }
    if (m_animation) {
        m_runtimeConnections.emplace_back(m_animation->onEvent.connect([this](const AnimationEvent& e) {
            invoke(e.entity, "onAnimationEvent", e.name, e.payload);
        }));
    }
    if (m_ai) {
        m_runtimeConnections.emplace_back(m_ai->onPerception.connect([this](const PerceptionEvent& e) {
            sol::object src = e.source.valid() ? sol::make_object(m_vm.lua(), lua::wrap(*this, e.source))
                                               : sol::object(sol::lua_nil);
            invoke(e.listener, e.gained ? "onTargetSensed" : "onTargetLost", src, e.sight ? "sight" : "hearing");
        }));
        m_ai->scriptActionHook = [this](Entity e, const std::string& fn) -> ai::BTStatus {
            script::ScriptInstance* inst = instance(e);
            if (!inst) return ai::BTStatus::Failure;
            CallGuard guard(m_callDepth, [this] { processDeferredDestroy(); });
            script::ScriptResult r = inst->invoke(fn);
            if (!r.ok) return ai::BTStatus::Failure;
            const sol::object& v = r.value;
            if (!v.valid() || v.get_type() == sol::type::lua_nil || v.get_type() == sol::type::none) return ai::BTStatus::Success;
            if (v.get_type() == sol::type::boolean) return v.as<bool>() ? ai::BTStatus::Success : ai::BTStatus::Failure;
            if (v.get_type() == sol::type::string && v.as<std::string>() == "running") return ai::BTStatus::Running;
            if (v.get_type() == sol::type::string && v.as<std::string>() == "failure") return ai::BTStatus::Failure;
            return ai::BTStatus::Success;
        };
    }
}

void ScriptRuntime::installApi() {
    if (m_apiInstalled) return;
    m_apiInstalled = true;
    lua::installBindings(*this);
}

void ScriptRuntime::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_services = &services;
#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)
    m_bridge = services.tryGet<script::AsyncBridge>();
    m_coroutines = services.tryGet<CoroutineScheduler>();
#endif
    installApi();
    entt::registry& r = world.registry();
    m_connections.emplace_back(r.on_construct<PendingDestroyTag>().connect<&ScriptRuntime::onPendingDestroy>(*this));
    m_connections.emplace_back(r.on_construct<ScriptComponent>().connect<&ScriptRuntime::onConstructed>(*this));
    m_connections.emplace_back(r.on_update<ScriptComponent>().connect<&ScriptRuntime::onUpdated>(*this));
    m_connections.emplace_back(r.on_destroy<ScriptComponent>().connect<&ScriptRuntime::onDestroyed>(*this));
}

void ScriptRuntime::detach() {
    if (!m_world) return;
    syncPlayState(false);
    m_connections.clear();
    m_world = nullptr;
    m_services = nullptr;
}

void ScriptRuntime::syncPlayState(bool playing) {
    if (playing == m_playing || !m_world) return;
    m_playing = playing;
    if (playing) {
        for (auto e : m_world->registry().view<ScriptComponent>()) m_pending.push_back(e);
        std::sort(m_pending.begin(), m_pending.end());
    } else {
        std::vector<entt::entity> all;
        for (auto& [e, rec] : m_records) all.push_back(e);
        std::sort(all.begin(), all.end());
        for (auto e : all) destroyInstance(e);
        m_records.clear();
        m_pending.clear();
    }
}

void ScriptRuntime::onConstructed(entt::registry&, entt::entity e) {
    if (m_playing) m_pending.push_back(e);
}

void ScriptRuntime::onUpdated(entt::registry& r, entt::entity e) {
    if (!m_playing) return;
    auto it = m_records.find(e);
    if (it == m_records.end()) return;
    const auto& c = r.get<ScriptComponent>(e);
    const std::string key = c.asset.isValid() ? "id:" + c.asset.toString() : c.script;
    if (key != it->second.key) {
        destroyInstance(e);
        m_pending.push_back(e);
        return;
    }
    if (it->second.instance) applyProperties(m_world->wrap(e), *it->second.instance);
}

void ScriptRuntime::onDestroyed(entt::registry&, entt::entity e) {
    if (m_callDepth > 0) m_deferredDestroy.push_back(e);
    else destroyInstance(e);
}

void ScriptRuntime::onPendingDestroy(entt::registry&, entt::entity e) {
    // Entity::destroy(): run onDestroy now (components still alive) unless Lua is executing.
    if (!m_records.contains(e)) return;
    if (m_callDepth > 0) m_deferredDestroy.push_back(e);
    else destroyInstance(e);
}

void ScriptRuntime::processDeferredDestroy() {
    while (!m_deferredDestroy.empty()) {
        std::vector<entt::entity> list;
        list.swap(m_deferredDestroy);
        for (auto e : list) destroyInstance(e);
    }
}

void ScriptRuntime::destroyInstance(entt::entity e) {
    auto it = m_records.find(e);
    if (it == m_records.end()) return;
    // Move out first: onDestroy may touch the entity/components and trigger more signals.
    std::unique_ptr<script::ScriptInstance> inst = std::move(it->second.instance);
    m_records.erase(it);
    if (inst) {
        ++m_callDepth;
        inst->destroy();
        --m_callDepth;
    }
}

std::shared_ptr<script::ScriptAsset> ScriptRuntime::resolveAsset(const ScriptComponent& c, std::string& key) {
    auto* provider = m_services ? m_services->tryGet<IScriptSourceProvider>() : nullptr;
    std::optional<ScriptSource> src;
    if (c.asset.isValid()) {
        key = "id:" + c.asset.toString();
        if (provider) src = provider->scriptById(c.asset);
        if (!src) {
            OX_LOG_WARN("gameplay", "script asset {} not found", c.asset.toString());
            return nullptr;
        }
    } else {
        key = c.script;
        if (c.script.empty()) return nullptr;
        if (provider) src = provider->scriptByName(c.script);
    }
    if (src) {
        if (src->source.empty() && !src->path.empty()) return m_vm.loadScript(src->path);
        auto it = m_assets.find(key);
        if (it != m_assets.end() && it->second->source() == src->source) return it->second;
        auto asset = m_vm.loadScriptFromString(src->name.empty() ? key : src->name, src->source);
        m_assets[key] = asset;
        return asset;
    }
    // A file path: absolute, relative to the working directory or to one of the VM search roots.
    namespace fs = std::filesystem;
    fs::path path(c.script);
    if (!fs::exists(path)) {
        for (const auto& root : m_vm.config().searchRoots) {
            if (fs::exists(root / path)) {
                path = root / path;
                break;
            }
        }
    }
    return m_vm.loadScript(path);
}

void ScriptRuntime::applyProperties(Entity e, script::ScriptInstance& inst) {
    const auto& c = e.get<ScriptComponent>();
    for (const auto& [name, value] : c.properties) {
        if (!inst.setProperty(name, value.toScriptValue())) {
            OX_LOG_WARN("gameplay", "script of '{}': property '{}' rejected", e.name(), name);
        }
    }
}

void ScriptRuntime::createInstance(Entity e) {
    if (m_records.contains(e.handle())) return;
    const auto& c = e.get<ScriptComponent>();
    Record rec;
    auto asset = resolveAsset(c, rec.key);
    if (!asset || !asset->valid()) {
        OX_LOG_WARN("gameplay", "script '{}' of '{}' could not be loaded", rec.key, e.name());
        rec.failed = true;
        m_records.emplace(e.handle(), std::move(rec));
        return;
    }
    const lua::LuaEntity handle = lua::wrap(*this, e);
    rec.instance = m_vm.createInstance(asset, [handle](script::ScriptInstance&, sol::table& self) { self["entity"] = handle; });
    script::ScriptInstance* inst = rec.instance.get();
    m_records.emplace(e.handle(), std::move(rec));
    if (!inst) return;
    applyProperties(e, *inst);
    CallGuard guard(m_callDepth, [this] { processDeferredDestroy(); });
    inst->create();
}

void ScriptRuntime::createPending() {
    if (!m_playing || m_pending.empty() || !m_world) return;
    std::vector<entt::entity> pending;
    pending.swap(m_pending);
    entt::registry& r = m_world->registry();
    for (auto e : pending) {
        if (!r.valid(e) || !r.all_of<ScriptComponent>(e) || r.all_of<PendingDestroyTag>(e)) continue;
        createInstance(m_world->wrap(e));
    }
}

void ScriptRuntime::preUpdate(f32 dt) {
    if (!m_playing) return;
    {
        CallGuard guard(m_callDepth, [this] { processDeferredDestroy(); });
#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)
        if (m_bridge) m_bridge->update();
#endif
        m_vm.update(dt);
    }
    createPending();
}

void ScriptRuntime::fixedUpdate(f32 dt) {
    if (!m_playing || !m_world) return;
    OX_PROFILE_ZONE_N("ScriptRuntime::fixedUpdate");
    createPending();
    std::vector<entt::entity> entities;
    entities.reserve(m_records.size());
    for (auto& [e, rec] : m_records) entities.push_back(e);
    std::sort(entities.begin(), entities.end());
    entt::registry& r = m_world->registry();
    CallGuard guard(m_callDepth, [this] { processDeferredDestroy(); });
    for (auto e : entities) {
        auto it = m_records.find(e);
        if (it == m_records.end() || !it->second.instance || !r.valid(e)) continue;
        const auto* c = r.try_get<ScriptComponent>(e);
        if (!c || !c->enabled || !m_world->wrap(e).activeInHierarchy()) continue;
        it->second.instance->fixedUpdate(dt);
    }
}

void ScriptRuntime::update(f32 dt) {
    if (!m_playing || !m_world) return;
    OX_PROFILE_ZONE_N("ScriptRuntime::update");
    createPending();
    std::vector<entt::entity> entities;
    entities.reserve(m_records.size());
    for (auto& [e, rec] : m_records) entities.push_back(e);
    std::sort(entities.begin(), entities.end());
    entt::registry& r = m_world->registry();
    CallGuard guard(m_callDepth, [this] { processDeferredDestroy(); });
    for (auto e : entities) {
        auto it = m_records.find(e);
        if (it == m_records.end() || !it->second.instance || !r.valid(e)) continue;
        const auto* c = r.try_get<ScriptComponent>(e);
        if (!c || !c->enabled || !m_world->wrap(e).activeInHierarchy()) continue;
        it->second.instance->update(dt);
    }
#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)
    if (!m_raycasts.empty()) {
        std::vector<PendingRaycast> pending;
        pending.swap(m_raycasts);
        for (auto& rc : pending) {
            sol::main_object result = sol::main_object(m_vm.lua(), sol::lua_nil);
            if (m_physics) {
                if (auto hit = m_physics->raycast(rc.origin, rc.direction, rc.maxDistance)) {
                    sol::table t = m_vm.lua().create_table();
                    if (hit->entity.valid()) t["entity"] = lua::wrap(*this, hit->entity);
                    t["point"] = hit->point;
                    t["normal"] = hit->normal;
                    t["distance"] = hit->distance;
                    result = sol::main_object(t);
                }
            }
            rc.promise->setValue(std::move(result));
        }
    }
#endif
}

#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)
Future<sol::main_object> ScriptRuntime::startScriptCoroutine(Entity e, std::string_view function) {
    script::ScriptInstance* inst = instance(e);
    if (!m_bridge || !inst) return makeErrorFuture<sol::main_object>("no script instance or AsyncBridge");
    CallGuard guard(m_callDepth, [this] { processDeferredDestroy(); });
    return m_bridge->invoke(*inst, function);
}

Future<sol::main_object> ScriptRuntime::raycastAsync(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance) {
    auto promise = std::make_shared<Promise<sol::main_object>>();
    Future<sol::main_object> f = promise->future();
    m_raycasts.push_back({std::move(promise), origin, direction, maxDistance});
    return f;
}
#endif

script::ScriptInstance* ScriptRuntime::instance(Entity e) const {
    if (!e.valid()) return nullptr;
    auto it = m_records.find(e.handle());
    return it == m_records.end() ? nullptr : it->second.instance.get();
}

sol::table ScriptRuntime::self(Entity e) const {
    script::ScriptInstance* inst = instance(e);
    return inst ? inst->self() : sol::table();
}

void ScriptRuntime::sendEvent(Entity e, std::string_view name, const sol::object& payload) {
    if (auto* inst = instance(e)) {
        CallGuard guard(m_callDepth, [this] { processDeferredDestroy(); });
        inst->sendEvent(name, payload);
    }
}

void ScriptRuntime::sendEvent(Entity e, std::string_view name, const script::ScriptValue& payload) {
    if (auto* inst = instance(e)) {
        CallGuard guard(m_callDepth, [this] { processDeferredDestroy(); });
        inst->sendEvent(name, payload);
    }
}

template <class... Args>
void ScriptRuntime::invoke(Entity e, std::string_view fn, Args&&... args) {
    script::ScriptInstance* inst = instance(e);
    if (!inst || inst->state() == script::ScriptInstance::State::Destroyed) return;
    const auto* c = e.tryGet<ScriptComponent>();
    if (!c || !c->enabled) return;
    CallGuard guard(m_callDepth, [this] { processDeferredDestroy(); });
    inst->invoke(fn, std::forward<Args>(args)...);
}

void ScriptRuntime::onCollision(const CollisionEvent& ev) {
    if (!m_playing) return;
    const char* fn = ev.phase == ContactPhase::Begin     ? "onCollisionEnter"
                     : ev.phase == ContactPhase::Persist ? "onCollisionStay"
                                                         : "onCollisionExit";
    auto info = [&](const glm::vec3& normal) {
        sol::table t = m_vm.lua().create_table();
        t["normal"] = normal;
        t["point"] = ev.point;
        t["impulse"] = ev.impulse;
        return t;
    };
    if (ev.a.valid() && instance(ev.a)) invoke(ev.a, fn, lua::wrap(*this, ev.b), info(ev.normal));
    if (ev.b.valid() && instance(ev.b)) invoke(ev.b, fn, lua::wrap(*this, ev.a), info(-ev.normal));
}

void ScriptRuntime::onTrigger(const TriggerEvent& ev) {
    if (!m_playing) return;
    const char* fn = ev.phase == TriggerPhase::Enter ? "onTriggerEnter"
                     : ev.phase == TriggerPhase::Stay ? "onTriggerStay"
                                                      : "onTriggerExit";
    if (ev.trigger.valid() && instance(ev.trigger)) invoke(ev.trigger, fn, lua::wrap(*this, ev.other));
    if (ev.other.valid() && instance(ev.other)) invoke(ev.other, fn, lua::wrap(*this, ev.trigger));
}

Entity ScriptRuntime::spawnPrefab(std::string_view nameOrPath, const glm::vec3* position, const glm::quat* rotation,
                                  Entity parent) {
    if (!m_world) return {};
    std::shared_ptr<const serial::Document> doc;
    if (auto* provider = m_services ? m_services->tryGet<IPrefabProvider>() : nullptr) doc = provider->prefab(nameOrPath);
    if (!doc) {
        auto loaded = serial::loadDocument(std::filesystem::path(nameOrPath));
        if (!loaded) {
            OX_LOG_WARN("gameplay", "spawn: prefab '{}' not found", nameOrPath);
            return {};
        }
        doc = std::make_shared<serial::Document>(std::move(*loaded));
    }
    auto root = instantiatePrefab(*m_world, *doc, parent);
    if (!root) {
        OX_LOG_WARN("gameplay", "spawn: '{}': {}", nameOrPath, root.error().message);
        return {};
    }
    if (position) root->setWorldPosition(*position);
    if (rotation) root->setWorldRotation(glm::normalize(*rotation));
    return *root;
}

} // namespace ox::gameplay
