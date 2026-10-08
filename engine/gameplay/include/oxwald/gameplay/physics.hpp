#pragma once

#include <oxwald/core/events.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/gameplay/events.hpp>
#include <oxwald/physics/physics.hpp>
#include <oxwald/scene/world.hpp>

#include <entt/signal/sigh.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ox {
class DebugDraw;
}

namespace ox::gameplay {

class IMeshColliderProvider;

// ---- components ------------------------------------------------------------------------------------------

// Dynamic/kinematic/static body. Needs a ColliderComponent on the same entity for its shape.
struct RigidBodyComponent {
    physics::MotionType motionType = physics::MotionType::Dynamic;
    std::string layer;              // collision layer name; empty = automatic from motion type / sensor
    f32 mass = 0.f;                 // kg; <= 0: from collider density
    glm::vec3 inertiaOverride{0.f}; // diagonal; zero = from the shape
    f32 friction = 0.5f;
    f32 restitution = 0.f;
    f32 linearDamping = 0.05f;
    f32 angularDamping = 0.05f;
    f32 gravityFactor = 1.f;
    bool ccd = false;
    bool allowSleeping = true;
    bool startActive = true;
    u8 lockAxes = physics::lock::None; // physics::lock bit flags
    bool reportContacts = true;
    bool interpolate = true; // render interpolation between fixed steps
    glm::vec3 initialLinearVelocity{0.f};
    glm::vec3 initialAngularVelocity{0.f};
    // runtime
    u32 bodyId = physics::BodyHandle::kInvalid;

    [[nodiscard]] physics::BodyHandle body() const { return physics::BodyHandle{bodyId}; }
};

enum class ColliderType : u8 { Box, Sphere, Capsule, Cylinder, ConvexHull, Mesh, HeightField, Compound };

// One primitive of a compound collider (local to the collider).
struct ColliderChild {
    ColliderType type = ColliderType::Box; // Box/Sphere/Capsule/Cylinder/ConvexHull
    glm::vec3 halfExtents{0.5f};
    f32 radius = 0.5f;
    f32 halfHeight = 0.5f;
    std::vector<glm::vec3> points; // ConvexHull
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
};

// Shape of the entity's body. Without a RigidBodyComponent the entity gets a static body.
// Entity world scale multiplies `scale`; changing either rebuilds the shape.
struct ColliderComponent {
    ColliderType type = ColliderType::Box;
    glm::vec3 halfExtents{0.5f};
    f32 radius = 0.5f;
    f32 halfHeight = 0.5f; // capsule cylinder part / cylinder
    f32 convexRadius = -1.f;
    f32 density = 1000.f;
    Uuid mesh;                     // Mesh: triangle mesh; ConvexHull without points: hull of the mesh vertices
    std::vector<glm::vec3> points; // ConvexHull points (inline)
    std::vector<f32> heights;      // HeightField samples (sampleCount^2, row-major z then x)
    u32 sampleCount = 0;
    glm::vec3 heightFieldOffset{0.f};
    glm::vec3 heightFieldScale{1.f};
    std::vector<ColliderChild> children; // Compound
    glm::vec3 offsetPosition{0.f};
    glm::quat offsetRotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 scale{1.f};
    bool isSensor = false;
};

// Kinematic collide-and-slide character. The entity position is the character's feet.
struct CharacterControllerComponent {
    f32 height = 1.8f;
    f32 radius = 0.3f;
    f32 maxSlopeAngle = 45.f; // degrees
    f32 maxStepHeight = 0.35f;
    f32 stickToFloorDistance = 0.5f;
    f32 mass = 70.f;
    f32 maxStrength = 100.f;
    f32 jumpSpeed = 5.f;
    f32 airControl = 0.25f;
    std::string layer; // empty = Character
    // runtime (input + state)
    glm::vec3 desiredVelocity{0.f}; // horizontal, world space; written by gameplay code each frame
    bool jump = false;              // consumed by the next fixed step
    physics::GroundState groundState = physics::GroundState::InAir;
    glm::vec3 velocity{0.f};
};

// Runtime tag (not reflected): the character is moved by someone else (network prediction replays inputs through
// PhysicsWorld::moveCharacter itself). The physics step still syncs teleports and writes the character pose back,
// but does not move it from desiredVelocity/jump.
struct ExternalCharacterMotionTag {};

// Marks the entity's collider as a sensor and filters which entities produce trigger events.
struct TriggerComponent {
    std::string requiredTag;  // only entities with this tag (TagComponent) trigger; empty = all
    bool reportStay = false;  // also emit Stay events every fixed step
    bool once = false;        // disable after the first Enter
    // runtime
    u32 overlapCount = 0;
    bool fired = false;
};

// Constraint between this entity's body and `target`'s body (or the world when target is empty).
// Anchors/axes are local to the respective entity.
struct JointComponent {
    physics::ConstraintType type = physics::ConstraintType::Fixed;
    EntityRef target;
    glm::vec3 anchor{0.f};       // local to this entity
    glm::vec3 targetAnchor{0.f}; // local to the target (Point/Distance)
    glm::vec3 axis{0.f, 1.f, 0.f};
    bool limitsEnabled = false;
    f32 limitMin = -3.14159265f;
    f32 limitMax = 3.14159265f;
    physics::MotorMode motorMode = physics::MotorMode::Off;
    f32 motorTarget = 0.f;
    f32 motorMaxForce = 1000.f;
    f32 minDistance = -1.f;
    f32 maxDistance = -1.f;
    f32 springFrequency = 0.f;
    f32 springDamping = 0.f;
    f32 coneHalfAngle = 30.f; // degrees
    f32 breakForce = 0.f;     // 0 = unbreakable
    f32 breakTorque = 0.f;
    // runtime
    bool broken = false;
};

// ---- runtime -------------------------------------------------------------------------------------------

struct PhysicsRaycastHit {
    Entity entity;
    physics::BodyHandle body;
    glm::vec3 point{0.f};
    glm::vec3 normal{0.f};
    f32 distance = 0.f;
};

// Binds ECS components to a physics::PhysicsWorld (service). Bodies only exist in play mode; in edit mode the
// colliders are visualised from component data. Registered as a service by addGameplaySystems().
class PhysicsRuntime {
public:
    explicit PhysicsRuntime(physics::PhysicsWorld& world);
    ~PhysicsRuntime();
    PhysicsRuntime(const PhysicsRuntime&) = delete;
    PhysicsRuntime& operator=(const PhysicsRuntime&) = delete;

    [[nodiscard]] physics::PhysicsWorld& physicsWorld() { return m_physics; }
    [[nodiscard]] World* world() const { return m_world; }
    [[nodiscard]] bool simulating() const { return m_simulating; }

    [[nodiscard]] Entity entityOf(physics::BodyHandle body) const;
    [[nodiscard]] physics::BodyHandle bodyOf(Entity e) const;
    [[nodiscard]] physics::CharacterHandle characterOf(Entity e) const;

    std::optional<PhysicsRaycastHit> raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance,
                                             Entity ignore = {}, bool includeSensors = false) const;
    std::vector<Entity> overlapSphere(const glm::vec3& center, f32 radius, bool includeSensors = false) const;
    // Number of non-sensor bodies hit between a and b, ignoring bodies of entities rejected by `ignore`.
    u32 countHits(const glm::vec3& a, const glm::vec3& b, const std::function<bool(Entity)>& ignore = {}) const;

    // Body helpers (no-ops for entities without a body).
    void addForce(Entity e, const glm::vec3& force);
    void addImpulse(Entity e, const glm::vec3& impulse);
    void addImpulseAtPoint(Entity e, const glm::vec3& impulse, const glm::vec3& point);
    void addTorque(Entity e, const glm::vec3& torque);
    [[nodiscard]] glm::vec3 linearVelocity(Entity e) const;
    void setLinearVelocity(Entity e, const glm::vec3& v);
    [[nodiscard]] glm::vec3 angularVelocity(Entity e) const;
    void setAngularVelocity(Entity e, const glm::vec3& v);
    // Moves the body (and the entity) immediately, resetting interpolation.
    void teleport(Entity e, const glm::vec3& position, const glm::quat& rotation);

    Signal<const CollisionEvent&> onCollision;
    Signal<const TriggerEvent&> onTrigger;
    Signal<const JointBrokenEvent&> onJointBroken;

    // Debug draw of bodies (play) / colliders from components (edit), when a DebugDraw service exists.
    bool debugDraw = false;
    physics::DebugDrawOptions debugOptions;

    // ---- driven by the gameplay systems ----
    void attach(World& world, Services& services);
    void detach();
    void syncPlayState(bool playing);
    void processPending();
    void fixedStep(f32 dt);
    void interpolate(f32 alpha);
    void drawDebug(DebugDraw& draw, bool playing);

private:
    struct BodyRecord {
        physics::BodyHandle body;
        physics::CharacterHandle character;
        physics::MotionType motion = physics::MotionType::Static;
        bool interpolate = false;
        physics::Transform previous;
        physics::Transform current;
        glm::vec3 builtScale{1.f};
        glm::vec3 writtenPosition{0.f};
        glm::quat writtenRotation{1.f, 0.f, 0.f, 0.f};
    };

    void onBodyComponentChanged(entt::registry& r, entt::entity e);
    void onBodyComponentDestroyed(entt::registry& r, entt::entity e);
    void onJointChanged(entt::registry& r, entt::entity e);
    void onJointDestroyed(entt::registry& r, entt::entity e);
    void connectSignals();
    void disconnectSignals();
    void createAll();
    void destroyAll();
    void createBody(Entity e);
    void destroyBody(entt::entity e);
    void createJoint(Entity e);
    void destroyJoint(entt::entity e);
    void syncBodiesIn(f32 dt);
    void syncBodiesOut();
    void dispatchEvents();
    void writePose(Entity e, BodyRecord& rec, const physics::Transform& pose);
    std::optional<physics::ShapeRef> buildShape(Entity e, const ColliderComponent& c, const glm::vec3& worldScale);

    physics::PhysicsWorld& m_physics;
    World* m_world = nullptr;
    Services* m_services = nullptr;
    IMeshColliderProvider* m_meshes = nullptr;
    EventBus* m_bus = nullptr;
    bool m_simulating = false;
    std::unordered_map<entt::entity, BodyRecord> m_bodies;
    std::unordered_map<entt::entity, physics::ConstraintHandle> m_joints;
    std::unordered_set<entt::entity> m_pendingBodies;
    std::unordered_set<entt::entity> m_pendingJoints;
    std::vector<entt::scoped_connection> m_connections;
};

} // namespace ox::gameplay
