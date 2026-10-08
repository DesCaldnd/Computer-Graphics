#pragma once

#include <oxwald/ai/behavior_tree.hpp>
#include <oxwald/ai/nav_crowd.hpp>
#include <oxwald/ai/nav_query.hpp>
#include <oxwald/ai/nav_tile_cache.hpp>
#include <oxwald/ai/navmesh.hpp>
#include <oxwald/ai/perception.hpp>
#include <oxwald/core/events.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/gameplay/events.hpp>
#include <oxwald/scene/world.hpp>

#include <entt/signal/sigh.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox {
class DebugDraw;
}

namespace ox::gameplay {

class IMeshColliderProvider;
class PhysicsRuntime;
class AnimationRuntime;
class AudioRuntime;

// ---- components ------------------------------------------------------------------------------------------

// Navmesh build settings + bake sources. The first surface in the world is used. The baked mesh is stored in
// `bakedData` (saved with the scene); without data and with bakeOnStart, play mode bakes on start.
struct NavMeshSurfaceComponent {
    f32 cellSize = 0.3f;
    f32 cellHeight = 0.2f;
    f32 agentHeight = 2.0f;
    f32 agentRadius = 0.5f;
    f32 agentMaxClimb = 0.9f;
    f32 agentMaxSlope = 45.f;
    i32 regionMinSize = 8;
    i32 regionMergeSize = 20;
    f32 edgeMaxLen = 12.f;
    f32 edgeMaxError = 1.3f;
    i32 vertsPerPoly = 6;
    f32 detailSampleDist = 6.f;
    f32 detailSampleMaxError = 1.f;
    bool tiled = false;
    i32 tileSize = 48;
    bool includeStaticColliders = true;
    bool includeMeshes = true;       // MeshRenderer meshes of static entities (IMeshColliderProvider)
    bool onlyChildren = false;       // gather only from this entity's subtree
    bool bakeOnStart = true;
    bool dynamicObstacles = false;   // tile cache (NavObstacleComponent carving); not serialisable
    bool drawInEditor = true;
    std::vector<u8> bakedData;       // NavMesh::serialize()
    // runtime
    bool baked = false;
};

// Crowd agent. Moves the entity (or drives its CharacterController) towards `destination`.
struct NavAgentComponent {
    f32 radius = 0.5f;
    f32 height = 2.f;
    f32 maxSpeed = 3.5f;
    f32 maxAcceleration = 8.f;
    f32 separationWeight = 2.f;
    u8 avoidanceQuality = 3;
    f32 stoppingDistance = 0.3f;
    bool updatePosition = true;
    bool updateRotation = true;
    bool driveCharacterController = true;
    // runtime (destination is SaveGame)
    glm::vec3 destination{0.f};
    bool hasDestination = false;
    bool reached = false;
    glm::vec3 velocity{0.f};
};

enum class NavObstacleShape : u8 { Cylinder, Box };

// Carves the navmesh when the surface uses dynamicObstacles (tile cache).
struct NavObstacleComponent {
    NavObstacleShape shape = NavObstacleShape::Cylinder;
    f32 radius = 0.5f;
    f32 height = 2.f;
    glm::vec3 halfExtents{0.5f, 1.f, 0.5f};
    f32 moveThreshold = 0.2f; // re-carve after moving this far
};

enum class BlackboardEntryType : u8 { Bool, Int, Float, String, Vec3, Entity };

struct BlackboardEntry {
    std::string key;
    BlackboardEntryType type = BlackboardEntryType::Float;
    bool boolValue = false;
    i32 intValue = 0;
    f32 floatValue = 0.f;
    std::string stringValue;
    glm::vec3 vec3Value{0.f};
    EntityRef entityValue; // stored in the blackboard as a runtime entity id (u64)
};

// Behaviour tree: asset (IBehaviorTreeProvider) or inline JSON (BTFactory format) + blackboard init.
struct BehaviorTreeComponent {
    Uuid tree;
    std::string treeJson;
    std::vector<BlackboardEntry> blackboard;
    bool enabled = true;
    f32 tickInterval = 0.f; // 0 = every fixed step
    bool restartOnFinish = true;
    // runtime
    ai::BTStatus status = ai::BTStatus::Idle;
};

struct PerceptionComponent {
    u8 team = 1;
    bool listener = true; // senses (sight/hearing)
    bool source = true;   // perceivable by others
    bool visible = true;
    ai::SightConfig sight;
    ai::HearingConfig hearing;
    bool detectHostile = true;
    bool detectNeutral = false;
    bool detectFriendly = false;
    // runtime
    EntityRef target; // best hostile currently sensed
    bool targetVisible = false;
};

// ---- runtime -------------------------------------------------------------------------------------------

// Blackboard ids for entities (see toRuntimeId); "self" is set for every tree.
[[nodiscard]] inline u64 toBlackboardId(const Entity& e) { return toRuntimeId(e); }

// Context handed to behaviour tree nodes through BTContext::user.
struct BTAgentContext {
    class AIRuntime* ai = nullptr;
    World* world = nullptr;
    entt::entity entity = entt::null;
    [[nodiscard]] Entity self() const { return world ? world->wrap(entity) : Entity{}; }
};

// Collects static geometry of the world for navmesh baking (colliders, meshes).
[[nodiscard]] ai::NavMeshInput gatherNavMeshInput(World& world, const NavMeshSurfaceComponent& settings,
                                                  Entity surface, IMeshColliderProvider* meshes);
[[nodiscard]] ai::NavMeshBuildSettings toBuildSettings(const NavMeshSurfaceComponent& c);

class AIRuntime {
public:
    AIRuntime();
    ~AIRuntime();
    AIRuntime(const AIRuntime&) = delete;
    AIRuntime& operator=(const AIRuntime&) = delete;

    // Bakes the navmesh of `surface` (also usable in edit mode: fills bakedData).
    bool bake(Entity surface);
    [[nodiscard]] const ai::NavMesh* navMesh() const;
    [[nodiscard]] ai::NavQuery* query() const { return m_query.get(); }
    [[nodiscard]] ai::NavCrowd* crowd() const { return m_crowd.get(); }
    [[nodiscard]] ai::PerceptionSystem& perception() { return m_perception; }
    // Node/action/condition registry used for every BehaviorTreeComponent (gameplay nodes pre-registered:
    // MoveTo, PlayAnimation, PlaySound, IsTargetVisible, ScriptAction; built-ins: Wait, SetBlackboard, ...).
    [[nodiscard]] ai::BTFactory& factory() { return m_factory; }

    bool moveTo(Entity agent, const glm::vec3& destination);
    void stop(Entity agent);
    [[nodiscard]] bool reached(Entity agent) const;
    [[nodiscard]] ai::BehaviorTree* behaviorTree(Entity e) const;
    [[nodiscard]] ai::Blackboard* blackboard(Entity e) const;
    void reportNoise(const glm::vec3& position, f32 loudness, f32 radius, Entity instigator = {},
                     const std::string& tag = {});
    [[nodiscard]] std::vector<glm::vec3> findPath(const glm::vec3& from, const glm::vec3& to) const;

    // Hook used by the ScriptAction node (installed by the script runtime): call `function` on the entity's
    // script; return Success/Failure/Running.
    std::function<ai::BTStatus(Entity, const std::string& function)> scriptActionHook;

    Signal<const PerceptionEvent&> onPerception;
    bool debugDraw = false;

    // ---- driven by the gameplay systems ----
    void attach(World& world, Services& services);
    void detach();
    void syncPlayState(bool playing);
    void updatePerception(f32 dt);
    void updateBehaviorTrees(f32 dt);
    void updateNavigation(f32 dt);
    void drawDebug(DebugDraw& draw, bool playing);

    [[nodiscard]] PhysicsRuntime* physics() const { return m_physics; }
    [[nodiscard]] AnimationRuntime* animation() const { return m_animation; }
    [[nodiscard]] AudioRuntime* audio() const { return m_audio; }
    [[nodiscard]] World* world() const { return m_world; }

private:
    struct AgentRecord {
        ai::NavAgent agent;
    };
    struct TreeRecord {
        std::unique_ptr<ai::BehaviorTree> tree;
        std::unique_ptr<BTAgentContext> context;
        f32 accumulator = 0.f;
        bool failed = false;
        Uuid treeId;
        std::string treeJson;
    };
    struct ObstacleRecord {
        ai::NavObstacleId id = 0;
        glm::vec3 position{0.f};
    };
    void registerGameplayNodes();
    void onAgentDestroyed(entt::registry& r, entt::entity e);
    void onTreeChanged(entt::registry& r, entt::entity e);
    void onTreeDestroyed(entt::registry& r, entt::entity e);
    void onPerceptionDestroyed(entt::registry& r, entt::entity e);
    void onObstacleDestroyed(entt::registry& r, entt::entity e);
    bool loadSurface(Entity surface, bool allowBake);
    void resetNavigation();
    void createCrowd();
    AgentRecord* ensureAgent(Entity e);
    TreeRecord* ensureTree(Entity e);
    void updateObstacles();
    [[nodiscard]] const ai::NavMesh* activeMesh() const;

    World* m_world = nullptr;
    Services* m_services = nullptr;
    PhysicsRuntime* m_physics = nullptr;
    AnimationRuntime* m_animation = nullptr;
    AudioRuntime* m_audio = nullptr;
    EventBus* m_bus = nullptr;
    std::unique_ptr<ai::NavMesh> m_mesh;
    std::unique_ptr<ai::NavTileCache> m_tileCache;
    std::unique_ptr<ai::NavQuery> m_query;
    std::unique_ptr<ai::NavCrowd> m_crowd;
    ai::PerceptionSystem m_perception;
    ai::BTFactory m_factory;
    std::unordered_map<entt::entity, AgentRecord> m_agents;
    std::unordered_map<entt::entity, TreeRecord> m_trees;
    std::unordered_map<entt::entity, ai::PerceptionListenerId> m_listeners;
    std::unordered_map<entt::entity, ObstacleRecord> m_obstacles;
    std::vector<entt::scoped_connection> m_connections;
    std::vector<PerceptionEvent> m_perceptionEvents;
    bool m_playing = false;
    bool m_surfaceChecked = false;
};

} // namespace ox::gameplay
