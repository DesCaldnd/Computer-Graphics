#pragma once

#include <oxwald/physics/body.hpp>
#include <oxwald/physics/character.hpp>
#include <oxwald/physics/collision_layers.hpp>
#include <oxwald/physics/constraint.hpp>
#include <oxwald/physics/debug_draw.hpp>
#include <oxwald/physics/events.hpp>
#include <oxwald/physics/job_executor.hpp>
#include <oxwald/physics/query.hpp>
#include <oxwald/physics/shape.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ox::physics {

struct PhysicsWorldDesc {
    u32 maxBodies = 65536;
    u32 maxBodyPairs = 65536;
    u32 maxContactConstraints = 16384;
    u32 numBodyMutexes = 0; // 0 = Jolt default
    // Jolt worker threads when no executor is given; < 0 = hardware_concurrency - 1, 0 = single threaded.
    i32 workerThreads = -1;
    IPhysicsJobExecutor* jobExecutor = nullptr; // not owned, must outlive the world
    usize tempAllocatorBytes = 32ull * 1024 * 1024;
    glm::vec3 gravity{0.f, -9.81f, 0.f};
    CollisionLayers layers = CollisionLayers::makeDefault();
    bool reportPersistContacts = true;

    // Solver settings (JPH::PhysicsSettings, Jolt defaults). Resting bodies may sink up to
    // penetrationSlop into each other — that is intended and keeps stacks stable.
    u32 velocitySteps = 10;
    u32 positionSteps = 2;
    f32 penetrationSlop = 0.02f;
    f32 speculativeContactDistance = 0.02f;
    f32 timeBeforeSleep = 0.5f;
};

// Jolt-backed physics world service. All methods must be called from one thread (the simulation
// thread) and never concurrently with step(); Jolt parallelises internally.
//
// Typical fixed-step use (see FixedStepper/TransformInterpolator):
//   world.moveCharacter(...);   // character input
//   world.step(1.f / 60.f);     // simulate, then events are dispatched
//   for (auto& e : world.contactEvents()) ...
class PhysicsWorld {
public:
    explicit PhysicsWorld(const PhysicsWorldDesc& desc = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // ---- Simulation ----------------------------------------------------------------------------
    // Advances by dt using `collisionSteps` sub-steps (raise for fast objects / large dt).
    // Clears the previous step's events, simulates, checks breakable constraints, dispatches events.
    void step(f32 dt, u32 collisionSteps = 1);
    [[nodiscard]] u64 stepCount() const;
    void setGravity(const glm::vec3& gravity);
    [[nodiscard]] glm::vec3 gravity() const;
    [[nodiscard]] const CollisionLayers& layers() const;
    // Rebuilds the broad-phase trees; call after adding many bodies at once (level load).
    void optimizeBroadPhase();
    [[nodiscard]] ShapeCache& shapeCache();
    [[nodiscard]] u32 bodyCount() const;
    [[nodiscard]] u32 activeBodyCount() const;

    // ---- Bodies -------------------------------------------------------------------------------
    BodyHandle createBody(const BodyDesc& desc);
    void destroyBody(BodyHandle body); // also destroys constraints attached to it
    [[nodiscard]] bool isValid(BodyHandle body) const;

    [[nodiscard]] Transform getTransform(BodyHandle body) const;
    [[nodiscard]] glm::vec3 getPosition(BodyHandle body) const;
    [[nodiscard]] glm::quat getRotation(BodyHandle body) const;
    [[nodiscard]] glm::vec3 getCenterOfMassPosition(BodyHandle body) const;
    // Teleport. For moving kinematic bodies with proper velocities use moveKinematic.
    void setTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation,
                      Activation activation = Activation::Activate);
    void setPosition(BodyHandle body, const glm::vec3& position, Activation activation = Activation::Activate);
    void setRotation(BodyHandle body, const glm::quat& rotation, Activation activation = Activation::Activate);
    // Sets velocities so a kinematic body reaches the target in dt (pushes dynamic bodies correctly).
    void moveKinematic(BodyHandle body, const glm::vec3& targetPosition, const glm::quat& targetRotation, f32 dt);

    [[nodiscard]] glm::vec3 getLinearVelocity(BodyHandle body) const;
    [[nodiscard]] glm::vec3 getAngularVelocity(BodyHandle body) const;
    void setLinearVelocity(BodyHandle body, const glm::vec3& velocity);
    void setAngularVelocity(BodyHandle body, const glm::vec3& velocity);
    [[nodiscard]] glm::vec3 getPointVelocity(BodyHandle body, const glm::vec3& worldPoint) const;

    // Forces/torques are accumulated until the next step; impulses apply immediately. All wake the body.
    void addForce(BodyHandle body, const glm::vec3& force);
    void addForceAtPoint(BodyHandle body, const glm::vec3& force, const glm::vec3& worldPoint);
    void addTorque(BodyHandle body, const glm::vec3& torque);
    void addImpulse(BodyHandle body, const glm::vec3& impulse);
    void addImpulseAtPoint(BodyHandle body, const glm::vec3& impulse, const glm::vec3& worldPoint);
    void addAngularImpulse(BodyHandle body, const glm::vec3& impulse);

    void activate(BodyHandle body);
    void deactivate(BodyHandle body); // put to sleep
    [[nodiscard]] bool isActive(BodyHandle body) const;

    [[nodiscard]] MotionType getMotionType(BodyHandle body) const;
    void setMotionType(BodyHandle body, MotionType type, Activation activation = Activation::Activate);
    [[nodiscard]] ObjectLayer getLayer(BodyHandle body) const;
    void setLayer(BodyHandle body, ObjectLayer layer);
    [[nodiscard]] bool isSensor(BodyHandle body) const;

    void setFriction(BodyHandle body, f32 friction);
    [[nodiscard]] f32 getFriction(BodyHandle body) const;
    void setRestitution(BodyHandle body, f32 restitution);
    [[nodiscard]] f32 getRestitution(BodyHandle body) const;
    void setGravityFactor(BodyHandle body, f32 factor);
    [[nodiscard]] f32 getGravityFactor(BodyHandle body) const;
    void setDamping(BodyHandle body, f32 linear, f32 angular);
    [[nodiscard]] f32 getMass(BodyHandle body) const; // 0 for static bodies
    void setCcd(BodyHandle body, bool enabled);

    void setShape(BodyHandle body, const ShapeRef& shape, bool updateMassProperties = true,
                  Activation activation = Activation::Activate);
    [[nodiscard]] ShapeRef getShape(BodyHandle body) const;
    [[nodiscard]] Aabb getWorldBounds(BodyHandle body) const;

    void setUserData(BodyHandle body, u64 userData);
    [[nodiscard]] u64 getUserData(BodyHandle body) const;

    // ---- Events (valid until the next step) ---------------------------------------------------
    [[nodiscard]] std::span<const ContactEvent> contactEvents() const;
    [[nodiscard]] std::span<const TriggerEvent> triggerEvents() const;
    [[nodiscard]] std::span<const ConstraintBrokenEvent> constraintBrokenEvents() const;
    // Optional callbacks, invoked on the step() caller's thread after the simulation.
    void setContactCallback(std::function<void(const ContactEvent&)> callback);
    void setTriggerCallback(std::function<void(const TriggerEvent&)> callback);
    void setConstraintBrokenCallback(std::function<void(const ConstraintBrokenEvent&)> callback);

    // ---- Queries ------------------------------------------------------------------------------
    // direction need not be normalized.
    [[nodiscard]] std::optional<RayHit> raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance,
                                                const QueryFilter& filter = {}) const;
    // All hits sorted by distance (one per body/sub-shape).
    [[nodiscard]] std::vector<RayHit> raycastAll(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance,
                                                 const QueryFilter& filter = {}) const;
    // Sweeps `shape` from (position, rotation) along direction*distance; closest hit.
    [[nodiscard]] std::optional<ShapeCastHit> shapeCast(const ShapeRef& shape, const glm::vec3& position,
                                                        const glm::quat& rotation, const glm::vec3& direction,
                                                        f32 distance, const QueryFilter& filter = {}) const;
    [[nodiscard]] std::optional<ShapeCastHit> sphereCast(const glm::vec3& origin, f32 radius, const glm::vec3& direction,
                                                         f32 distance, const QueryFilter& filter = {}) const;
    [[nodiscard]] std::optional<ShapeCastHit> boxCast(const glm::vec3& center, const glm::vec3& halfExtents,
                                                      const glm::quat& rotation, const glm::vec3& direction,
                                                      f32 distance, const QueryFilter& filter = {}) const;
    // Capsule along local Y.
    [[nodiscard]] std::optional<ShapeCastHit> capsuleCast(const glm::vec3& center, f32 halfHeight, f32 radius,
                                                          const glm::quat& rotation, const glm::vec3& direction,
                                                          f32 distance, const QueryFilter& filter = {}) const;
    [[nodiscard]] std::vector<BodyHandle> overlapShape(const ShapeRef& shape, const glm::vec3& position,
                                                       const glm::quat& rotation, const QueryFilter& filter = {}) const;
    [[nodiscard]] std::vector<BodyHandle> overlapSphere(const glm::vec3& center, f32 radius,
                                                        const QueryFilter& filter = {}) const;
    [[nodiscard]] std::vector<BodyHandle> overlapBox(const glm::vec3& center, const glm::vec3& halfExtents,
                                                     const glm::quat& rotation, const QueryFilter& filter = {}) const;
    // Broad-phase only: bodies whose world AABB intersects the box.
    [[nodiscard]] std::vector<BodyHandle> overlapAabb(const Aabb& box, const QueryFilter& filter = {}) const;
    [[nodiscard]] std::optional<ClosestPointResult> closestPoint(const glm::vec3& point, f32 maxDistance,
                                                                 const QueryFilter& filter = {}) const;

    // ---- Character controllers ----------------------------------------------------------------
    CharacterHandle createCharacter(const CharacterDesc& desc);
    void destroyCharacter(CharacterHandle character);
    [[nodiscard]] bool isValid(CharacterHandle character) const;
    // Typical per-fixed-step update: ground/jump/air-control velocity logic + gravity, then
    // collide-and-slide with stair stepping and floor sticking. Call before step().
    void moveCharacter(CharacterHandle character, f32 dt, const CharacterMoveInput& input);
    // Low level: set the velocity yourself, then update (with stairs/stick-to-floor).
    void setCharacterVelocity(CharacterHandle character, const glm::vec3& velocity);
    void updateCharacter(CharacterHandle character, f32 dt);
    void setCharacterTransform(CharacterHandle character, const glm::vec3& position, const glm::quat& rotation);
    // Changing the shape (crouch); fails (returns false) if the new shape would penetrate.
    bool setCharacterShape(CharacterHandle character, const ShapeRef& shape, f32 maxPenetration = 0.05f);
    [[nodiscard]] CharacterState getCharacterState(CharacterHandle character) const;
    [[nodiscard]] BodyHandle getCharacterInnerBody(CharacterHandle character) const;

    // ---- Constraints --------------------------------------------------------------------------
    ConstraintHandle createConstraint(const ConstraintDesc& desc);
    void destroyConstraint(ConstraintHandle constraint);
    [[nodiscard]] bool isValid(ConstraintHandle constraint) const;
    void setConstraintEnabled(ConstraintHandle constraint, bool enabled);
    [[nodiscard]] bool isConstraintEnabled(ConstraintHandle constraint) const;
    // Hinge (rad, rad/s) and slider (m, m/s) motors.
    void setMotor(ConstraintHandle constraint, MotorMode mode, f32 target);
    // Hinge: current angle (rad). Slider: current position (m). Distance: current length. Else 0.
    [[nodiscard]] f32 getJointValue(ConstraintHandle constraint) const;

    // ---- Debug draw ---------------------------------------------------------------------------
    void debugDraw(PhysicsDebugSink& sink, const DebugDrawOptions& options = {}) const;

    // ---- Snapshot -----------------------------------------------------------------------------
    // Serialises body states, velocities, contact cache, constraint state, characters and event
    // tracking. Restore requires the same set of bodies/constraints/characters (created in the same
    // order) — it does not create or destroy objects. Same binary + same inputs → identical results.
    [[nodiscard]] std::vector<u8> saveState() const;
    bool restoreState(std::span<const u8> data);

    // Escape hatch to the underlying JPH::PhysicsSystem* (requires including Jolt yourself).
    [[nodiscard]] void* nativeSystem() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace ox::physics
