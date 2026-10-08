#pragma once

#include <oxwald/core/events.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/gameplay/events.hpp>
#include <oxwald/scene/world.hpp>
#include <oxwald/script/script_instance.hpp>
#include <oxwald/script/script_vm.hpp>

#include <entt/signal/sigh.hpp>

#if defined(OX_GAMEPLAY_HAS_ASYNC) && defined(OX_SCRIPT_HAS_ASYNC)
#define OX_GAMEPLAY_SCRIPT_ASYNC 1
#include <oxwald/async/async.hpp>
#include <oxwald/script/async_bridge.hpp>
#endif

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::gameplay {

class PhysicsRuntime;
class AnimationRuntime;
class SplineRuntime;
class AudioRuntime;
class AIRuntime;
class IPrefabProvider;
class IScriptSourceProvider;

// ---- components ------------------------------------------------------------------------------------------

// Reflectable form of a script property value (script::ScriptValue is a variant).
struct ScriptPropertyValue {
    script::ScriptPropertyType type = script::ScriptPropertyType::Float;
    f64 number = 0.0;           // Float / Int
    bool boolean = false;       // Bool
    std::string text;           // String
    glm::vec4 vector{0.f};      // Vec2 / Vec3 / Vec4 / Color

    [[nodiscard]] script::ScriptValue toScriptValue() const;
    [[nodiscard]] static ScriptPropertyValue fromScriptValue(const script::ScriptValue& v,
                                                             script::ScriptPropertyType type);
    static ScriptPropertyValue makeNumber(f64 v) { return {script::ScriptPropertyType::Float, v}; }
};

// A Lua script attached to the entity. `script` is a file path (relative to the VM search roots) or a name
// resolved through IScriptSourceProvider; `asset` (when set) wins. Instances exist only in play mode.
struct ScriptComponent {
    std::string script;
    Uuid asset;
    std::map<std::string, ScriptPropertyValue> properties; // overrides of the script's declared properties
    bool enabled = true;
};

// ---- runtime -------------------------------------------------------------------------------------------

// Drives script instances (onCreate/onStart/onUpdate/onFixedUpdate/onDestroy) and installs the Lua entity API
// (`self.entity`, entity:get("Transform"), scene.find/spawn/destroy, physics/audio/ai/spline/animation tables).
// See docs/dev/modules/gameplay.md for the API reference.
class ScriptRuntime {
public:
    explicit ScriptRuntime(script::ScriptVM& vm);
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    [[nodiscard]] script::ScriptVM& vm() { return m_vm; }
    [[nodiscard]] World* world() const { return m_world; }
    [[nodiscard]] script::ScriptInstance* instance(Entity e) const;
    // Lua `self` table of the entity's script (invalid table when none).
    [[nodiscard]] sol::table self(Entity e) const;
    void sendEvent(Entity e, std::string_view name, const sol::object& payload);
    void sendEvent(Entity e, std::string_view name, const script::ScriptValue& payload = {});
    // Spawns a prefab by name/path through IPrefabProvider (or a document file). Returns the instance root.
    Entity spawnPrefab(std::string_view nameOrPath, const glm::vec3* position = nullptr,
                       const glm::quat* rotation = nullptr, Entity parent = {});

#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)
    // Lua `await` bridge (service, created by the runtime) and the coroutine scheduler; null when absent.
    [[nodiscard]] script::AsyncBridge* bridge() const { return m_bridge; }
    [[nodiscard]] CoroutineScheduler* coroutines() const { return m_coroutines; }
    // Runs `function(self)` of the entity's script as a script coroutine (may wait()/await()); the future
    // completes with its first return value.
    Future<sol::main_object> startScriptCoroutine(Entity e, std::string_view function);
    // physics.raycastAsync: resolved after the frame's fixed steps (Update phase).
    Future<sol::main_object> raycastAsync(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance);
#endif

    [[nodiscard]] PhysicsRuntime* physics() const { return m_physics; }
    [[nodiscard]] AnimationRuntime* animation() const { return m_animation; }
    [[nodiscard]] SplineRuntime* splines() const { return m_splines; }
    [[nodiscard]] AudioRuntime* audio() const { return m_audio; }
    [[nodiscard]] AIRuntime* ai() const { return m_ai; }

    // ---- driven by the gameplay systems ----
    // Connects to the other runtimes' signals (call once all runtimes exist).
    void bindRuntimes(Services& services);
    void attach(World& world, Services& services);
    void detach();
    void syncPlayState(bool playing);
    void preUpdate(f32 dt);
    void fixedUpdate(f32 dt);
    void update(f32 dt);

private:
    struct Record {
        std::unique_ptr<script::ScriptInstance> instance;
        std::string key; // script/asset the instance was created from
        bool failed = false;
    };
    void installApi();
    void onConstructed(entt::registry& r, entt::entity e);
    void onUpdated(entt::registry& r, entt::entity e);
    void onDestroyed(entt::registry& r, entt::entity e);
    void onPendingDestroy(entt::registry& r, entt::entity e);
    void processDeferredDestroy();
    void createPending();
    void createInstance(Entity e);
    void destroyInstance(entt::entity e);
    void applyProperties(Entity e, script::ScriptInstance& inst);
    std::shared_ptr<script::ScriptAsset> resolveAsset(const ScriptComponent& c, std::string& key);
    template <class... Args>
    void invoke(Entity e, std::string_view fn, Args&&... args);
    void onCollision(const CollisionEvent& ev);
    void onTrigger(const TriggerEvent& ev);

    script::ScriptVM& m_vm;
    World* m_world = nullptr;
    Services* m_services = nullptr;
    PhysicsRuntime* m_physics = nullptr;
    AnimationRuntime* m_animation = nullptr;
    SplineRuntime* m_splines = nullptr;
    AudioRuntime* m_audio = nullptr;
    AIRuntime* m_ai = nullptr;
    std::unordered_map<entt::entity, Record> m_records;
    std::vector<entt::entity> m_pending;
    std::unordered_map<std::string, std::shared_ptr<script::ScriptAsset>> m_assets;
    std::vector<entt::scoped_connection> m_connections;
    std::vector<ScopedConnection> m_runtimeConnections;
    std::vector<entt::entity> m_deferredDestroy;
    u32 m_callDepth = 0; // > 0 while Lua code runs (instances must not be destroyed under it)
    bool m_playing = false;
    bool m_apiInstalled = false;
#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)
    struct PendingRaycast {
        std::shared_ptr<Promise<sol::main_object>> promise;
        glm::vec3 origin{0.f};
        glm::vec3 direction{0.f};
        f32 maxDistance = 0.f;
    };
    script::AsyncBridge* m_bridge = nullptr;
    CoroutineScheduler* m_coroutines = nullptr;
    std::vector<PendingRaycast> m_raycasts;
#endif
};

} // namespace ox::gameplay
