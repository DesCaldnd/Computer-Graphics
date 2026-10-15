#pragma once

// Destructible objects: Voronoi pre-fracture (physics::voronoiFracture) at play start, damage/impulse thresholds,
// swap of the intact entity for chunk bodies, structural support graph with collapse, debris budget + fade-out,
// DestructionEvent (Signal + EventBus), Lua `destruction` table. See docs/guide/34-destruction.md.
//
//   auto& d = wall.add<ox::gameplay::DestructibleComponent>();
//   d.chunkCount = 24; d.structural = true;              // frozen chunks held by anchors (bottom of the wall)
//   services.get<ox::gameplay::DestructionRuntime>().applyDamage(wall, 150.f, hitPoint, dir * 300.f);

#include <oxwald/core/events.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/physics/fracture.hpp>
#include <oxwald/scene/world.hpp>

#include <entt/signal/sigh.hpp>

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox {
class DebugDraw;
}

namespace ox::gameplay {

class PhysicsRuntime;
class ScriptRuntime;
class AudioRuntime;
class IMeshColliderProvider;
struct CollisionEvent;

struct PreparedFracture;

// Which chunks of a structural destructible are held in place (and hold their neighbours through the link graph).
enum class DestructionAnchor : u8 {
    None,   // nothing: every chunk falls once the object breaks
    Bottom, // chunks touching the bottom of the object's bounds (local -Y)
    Static, // chunks touching static bodies of other entities (ground, pillars) when the object breaks
};

enum class DestructionCause : u8 { Damage, Impact, Script, Collapse, Replicated };

// Breaks into pre-fractured chunks. The fracture source is `sourceMesh` (mesh triangles through the
// IMeshColliderProvider) or the entity's Collider (Box, ConvexHull, Mesh, Sphere/Capsule/Cylinder approximated by
// hulls). Chunks reuse the entity's MeshRenderer materials (slot 0 = outer surface, slot 1 = cut faces).
struct DestructibleComponent {
    // fracture
    u32 chunkCount = 12;
    u32 seed = 1;
    Uuid sourceMesh;       // optional mesh asset to fracture; empty = Collider shape
    Uuid interiorMaterial; // material of the cut faces; empty = outer material
    f32 interiorUvScale = 1.f;
    f32 planarRatio = 0.6f; // thin objects get prism chunks through their thickness (physics::FractureSettings)
    // damage
    f32 health = 100.f;          // breaks at <= 0
    f32 damageThreshold = 0.f;   // single hits below this are ignored
    f32 impulseThreshold = 0.f;  // contact impulse (N*s) that breaks it at once; 0 = contacts never break it
    f32 impulseDamageScale = 0.f; // damage per N*s of contact impulse (below the threshold)
    // chunks / debris
    f32 density = 0.f;           // kg/m^3; 0 = Collider density
    f32 impactImpulseScale = 1.f; // share of the hit impulse given to the chunks near the hit
    f32 spread = 0.5f;           // radial part of the push (0 = along the impulse only)
    f32 debrisLifetime = 10.f;   // s before a resting chunk fades out; 0 = forever
    f32 fadeDuration = 1.f;      // s (chunks shrink, then are destroyed)
    u32 maxActiveDebris = 0;     // per object; 0 = only the global destruction.MaxActiveDebris budget
    std::string debrisLayer;     // collision layer of chunks; empty = Dynamic
    // structure
    bool structural = false;     // chunks stay frozen (static) until hit or disconnected from the anchors
    DestructionAnchor anchor = DestructionAnchor::Bottom;
    f32 anchorTolerance = 0.05f; // m: distance to the bottom plane / static bodies counted as touching
    f32 breakRadius = 0.6f;      // m: chunks released around a hit; impulse falloff radius
    f32 chunkImpulseThreshold = 0.f; // contact impulse releasing a frozen chunk; 0 = impulseThreshold
    // feedback
    std::string breakSound;      // clip path played at the hit point (AudioRuntime)
    // state
    bool broken = false;         // replicated: a client that sees it set fractures its own copy
    // runtime (not reflected): the prepared fracture, read by the renderer's extract hook to upload chunk meshes
    std::shared_ptr<const PreparedFracture> prepared;
};

// Runtime marker on spawned chunk entities (not saved).
struct DestructionChunkComponent {
    EntityRef owner;
    u32 index = 0;
    bool frozen = false;
    bool anchored = false;
    std::shared_ptr<const PreparedFracture> fracture; // runtime (not reflected): mesh data for the renderer
};

struct DestructionEvent {
    enum class Type : u8 { Fractured, ChunksReleased, Collapsed };
    Type type = Type::Fractured;
    Entity entity; // the destructible (still alive)
    DestructionCause cause = DestructionCause::Damage;
    glm::vec3 point{0.f};
    glm::vec3 impulse{0.f};
    u32 chunks = 0;      // chunks set in motion by this event
    u32 totalChunks = 0; // chunks of the fracture
};

// Pre-fracture shared by every destructible with the same source + settings.
struct PreparedFracture {
    u64 key = 0;
    std::shared_ptr<const physics::FractureResult> result;
    std::vector<Uuid> meshes; // per chunk: runtime mesh id (rendered through the render bridge)
};

// Render contract: chunk meshes the renderer must know (GpuResourceCache::addMesh). Append-only per session.
struct DestructionChunkMesh {
    Uuid id;
    std::shared_ptr<const physics::FractureResult> fracture;
    u32 chunk = 0;
};

class DestructionRuntime {
public:
    DestructionRuntime();
    ~DestructionRuntime();
    DestructionRuntime(const DestructionRuntime&) = delete;
    DestructionRuntime& operator=(const DestructionRuntime&) = delete;

    // ---- gameplay API (play mode) ----
    // Subtracts health (hits below damageThreshold ignored); breaks at <= 0. On a frozen chunk entity: releases it
    // (and the chunks around it within the owner's breakRadius). Returns true when something broke / was released.
    bool applyDamage(Entity e, f32 amount, const glm::vec3& point, const glm::vec3& impulse = glm::vec3(0.f),
                     DestructionCause cause = DestructionCause::Damage);
    // Breaks immediately.
    bool fracture(Entity e, const glm::vec3& point, const glm::vec3& impulse = glm::vec3(0.f),
                  DestructionCause cause = DestructionCause::Script);
    // Damage with linear falloff to every destructible / chunk within `radius`, impulse pushes debris outward.
    u32 applyRadialDamage(const glm::vec3& center, f32 radius, f32 damage, f32 impulse = 0.f);

    [[nodiscard]] bool isBroken(Entity e) const;
    [[nodiscard]] std::vector<Entity> chunksOf(Entity destructible) const;
    [[nodiscard]] u32 activeDebrisCount() const;       // moving (dynamic, not fading) chunks
    [[nodiscard]] u32 frozenChunkCount(Entity destructible) const;
    [[nodiscard]] u32 fadingCount() const;

    // Builds (or returns the cached) fracture of the entity. Works in edit mode too (preview, tools).
    std::shared_ptr<const PreparedFracture> prepare(Entity e);
    // Source mesh (entity-local, collider offset/scale applied) that prepare() would fracture.
    [[nodiscard]] std::optional<physics::FractureMesh> sourceMeshOf(Entity e) const;

    // Global debris budget override (0 = cvar destruction.MaxActiveDebris).
    void setMaxActiveDebris(u32 count) { m_maxDebrisOverride = count; }

    Signal<const DestructionEvent&> onDestruction;
    bool debugDraw = false; // link graph of frozen chunks (green = anchored)

    // ---- render bridge ----
    [[nodiscard]] const std::vector<DestructionChunkMesh>& chunkMeshes() const { return m_meshList; }
    [[nodiscard]] u64 meshVersion() const { return m_meshVersion; }

    // ---- driven by the gameplay systems ----
    void attach(World& world, Services& services);
    void detach();
    void syncPlayState(bool playing);
    void fixedUpdate(f32 dt);
    void drawDebug(DebugDraw& draw) const;

private:
    struct ChunkRecord {
        entt::entity entity = entt::null;
        entt::entity owner = entt::null;
        u32 index = 0;
        f32 volume = 0.f;
        f32 age = 0.f;
        bool frozen = false;
        bool anchored = false;
        bool fading = false;
        f32 fadeTime = 0.f;
        f32 fadeDuration = 1.f; // configured fade, then the running fade's duration
        f32 lifetime = 0.f;
        u32 ownerLimit = 0;
        glm::vec3 fadeScale{1.f};
        u64 order = 0; // spawn order (oldest first when over budget)
    };
    struct OwnerRecord {
        std::shared_ptr<const PreparedFracture> fracture;
        std::vector<entt::entity> chunks; // by chunk index (null when gone)
        bool fractured = false;
        // anchor == Static: static bodies (entity, or the owner of a frozen chunk) it rested on at play start
        std::vector<std::pair<entt::entity, glm::vec3>> supports;
        bool supportsRecorded = false;
    };
    struct PendingContact {
        entt::entity a = entt::null, b = entt::null;
        glm::vec3 point{0.f}, normal{0.f};
        f32 impulse = 0.f;
    };

    bool fractureAt(Entity e, const glm::vec3& point, const glm::vec3& impulse, DestructionCause cause, f32 releaseRadius);
    void onCollision(const CollisionEvent& ev);
    void onDestructibleChanged(entt::registry& r, entt::entity e);
    void processContacts();
    void processReplicated();
    void updateDebris(f32 dt);
    void enforceBudget();
    void recordSupports(Entity e, OwnerRecord& owner);
    void checkSupports();
    // Supports (entity, or owner entity of a frozen chunk) of the world AABB [mn, mx]: static bodies other than
    // `self` and its own chunks.
    [[nodiscard]] std::vector<std::pair<entt::entity, glm::vec3>> staticSupports(const glm::vec3& mn, const glm::vec3& mx,
                                                                                entt::entity self, f32 tolerance) const;
    void startFade(ChunkRecord& rec, f32 duration);
    bool releaseChunks(entt::entity owner, const std::vector<u32>& indices, const glm::vec3& point,
                       const glm::vec3& impulse, DestructionCause cause, bool collapse);
    u32 collapseDisconnected(entt::entity owner, const glm::vec3& point);
    Entity spawnChunk(Entity intact, const DestructibleComponent& d, const std::shared_ptr<const PreparedFracture>& f, u32 index, bool frozen,
                      const glm::vec3& linear, const glm::vec3& angular);
    void emit(const DestructionEvent& ev);
    void registerMeshes(const PreparedFracture& f);
    [[nodiscard]] u32 budget() const;

    World* m_world = nullptr;
    Services* m_services = nullptr;
    PhysicsRuntime* m_physics = nullptr;
    EventBus* m_bus = nullptr;
    bool m_playing = false;
    u64 m_spawnCounter = 0;
    u32 m_maxDebrisOverride = 0;
    std::unordered_map<u64, std::shared_ptr<const PreparedFracture>> m_cache;
    std::unordered_map<entt::entity, OwnerRecord> m_owners;
    std::unordered_map<entt::entity, ChunkRecord> m_chunks;
    std::vector<PendingContact> m_contacts;
    std::vector<DestructionChunkMesh> m_meshList;
    std::unordered_map<u64, bool> m_meshRegistered;
    u64 m_meshVersion = 0;
    u32 m_supportCheck = 0; // fixed steps until supports are re-evaluated (chunk bodies exist one step after a break)
    ScopedConnection m_collisionConnection;
    std::vector<entt::scoped_connection> m_connections;
};

// Registers the Lua `destruction` table on the runtime's VM (called by addGameplaySystems).
void bindDestructionLua(ScriptRuntime& scripts, DestructionRuntime& destruction);

} // namespace ox::gameplay
