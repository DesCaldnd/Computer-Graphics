#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/providers.hpp>
#include <oxwald/scene/components.hpp>

#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ox::gameplay {

namespace {

constexpr f32 kPositionEpsilon = 1e-4f;

bool samePosition(const glm::vec3& a, const glm::vec3& b) { return glm::length2(a - b) <= kPositionEpsilon * kPositionEpsilon; }
bool sameRotation(const glm::quat& a, const glm::quat& b) { return std::abs(glm::dot(a, b)) >= 1.f - 1e-6f; }
bool sameScale(const glm::vec3& a, const glm::vec3& b) { return glm::length2(a - b) <= 1e-8f; }

f32 maxAbs(const glm::vec3& v) { return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)}); }

physics::ShapeDesc primitiveDesc(ColliderType type, const glm::vec3& halfExtents, f32 radius, f32 halfHeight,
                                 const std::vector<glm::vec3>& points, const glm::vec3& s) {
    const glm::vec3 as = glm::abs(s);
    switch (type) {
    case ColliderType::Sphere: return physics::ShapeDesc::sphere(radius * maxAbs(s));
    case ColliderType::Capsule: return physics::ShapeDesc::capsule(halfHeight * as.y, radius * std::max(as.x, as.z));
    case ColliderType::Cylinder: return physics::ShapeDesc::cylinder(halfHeight * as.y, radius * std::max(as.x, as.z));
    case ColliderType::ConvexHull: {
        std::vector<glm::vec3> scaled;
        scaled.reserve(points.size());
        for (const auto& p : points) scaled.push_back(p * s);
        return physics::ShapeDesc::convexHull(std::move(scaled));
    }
    case ColliderType::Box:
    default: return physics::ShapeDesc::box(halfExtents * as);
    }
}

physics::Transform poseOf(const Transform& t) { return {t.position, glm::normalize(t.rotation)}; }

class DebugDrawSink final : public physics::PhysicsDebugSink {
public:
    explicit DebugDrawSink(DebugDraw& draw) : m_draw(draw) {}
    void line(const glm::vec3& a, const glm::vec3& b, const physics::Color& c) override { m_draw.line(a, b, c); }
    void text(const glm::vec3& p, std::string_view t, const physics::Color& c) override {
        m_draw.text3D(p, std::string(t), c);
    }

private:
    DebugDraw& m_draw;
};

} // namespace

PhysicsRuntime::PhysicsRuntime(physics::PhysicsWorld& world) : m_physics(world) {}

PhysicsRuntime::~PhysicsRuntime() { detach(); }

// ---- attach / play state -----------------------------------------------------------------------------------

void PhysicsRuntime::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_services = &services;
    m_meshes = services.tryGet<IMeshColliderProvider>();
    m_bus = services.tryGet<EventBus>();
    connectSignals();
}

void PhysicsRuntime::detach() {
    if (!m_world) return;
    destroyAll();
    disconnectSignals();
    m_world = nullptr;
    m_services = nullptr;
}

void PhysicsRuntime::connectSignals() {
    entt::registry& r = m_world->registry();
    auto add = [&](entt::connection c) { m_connections.emplace_back(c); };
    add(r.on_construct<ColliderComponent>().connect<&PhysicsRuntime::onBodyComponentChanged>(*this));
    add(r.on_update<ColliderComponent>().connect<&PhysicsRuntime::onBodyComponentChanged>(*this));
    add(r.on_destroy<ColliderComponent>().connect<&PhysicsRuntime::onBodyComponentDestroyed>(*this));
    add(r.on_construct<RigidBodyComponent>().connect<&PhysicsRuntime::onBodyComponentChanged>(*this));
    add(r.on_update<RigidBodyComponent>().connect<&PhysicsRuntime::onBodyComponentChanged>(*this));
    add(r.on_destroy<RigidBodyComponent>().connect<&PhysicsRuntime::onBodyComponentDestroyed>(*this));
    add(r.on_construct<CharacterControllerComponent>().connect<&PhysicsRuntime::onBodyComponentChanged>(*this));
    add(r.on_update<CharacterControllerComponent>().connect<&PhysicsRuntime::onBodyComponentChanged>(*this));
    add(r.on_destroy<CharacterControllerComponent>().connect<&PhysicsRuntime::onBodyComponentDestroyed>(*this));
    add(r.on_construct<TriggerComponent>().connect<&PhysicsRuntime::onBodyComponentChanged>(*this));
    add(r.on_destroy<TriggerComponent>().connect<&PhysicsRuntime::onBodyComponentDestroyed>(*this));
    add(r.on_construct<JointComponent>().connect<&PhysicsRuntime::onJointChanged>(*this));
    add(r.on_update<JointComponent>().connect<&PhysicsRuntime::onJointChanged>(*this));
    add(r.on_destroy<JointComponent>().connect<&PhysicsRuntime::onJointDestroyed>(*this));
}

void PhysicsRuntime::disconnectSignals() { m_connections.clear(); }

void PhysicsRuntime::syncPlayState(bool playing) {
    if (playing == m_simulating || !m_world) return;
    if (playing) {
        m_simulating = true;
        createAll();
    } else {
        destroyAll();
    }
}

void PhysicsRuntime::onBodyComponentChanged(entt::registry&, entt::entity e) {
    if (m_simulating) m_pendingBodies.insert(e);
}

void PhysicsRuntime::onBodyComponentDestroyed(entt::registry&, entt::entity e) {
    if (!m_simulating) return;
    destroyBody(e);
    // Removing e.g. the RigidBody of an entity that keeps its collider turns it into a static body.
    m_pendingBodies.insert(e);
}

void PhysicsRuntime::onJointChanged(entt::registry&, entt::entity e) {
    if (m_simulating) m_pendingJoints.insert(e);
}

void PhysicsRuntime::onJointDestroyed(entt::registry&, entt::entity e) {
    if (m_simulating) destroyJoint(e);
}

void PhysicsRuntime::createAll() {
    OX_PROFILE_ZONE();
    entt::registry& r = m_world->registry();
    std::vector<entt::entity> entities;
    for (auto e : r.view<ColliderComponent>()) entities.push_back(e);
    for (auto e : r.view<CharacterControllerComponent>()) {
        if (!r.all_of<ColliderComponent>(e)) entities.push_back(e);
    }
    // Deterministic creation order (body ids are part of the physics state).
    std::sort(entities.begin(), entities.end());
    for (auto e : entities) createBody(m_world->wrap(e));
    for (auto e : r.view<JointComponent>()) createJoint(m_world->wrap(e));
    m_pendingBodies.clear();
    m_pendingJoints.clear();
    if (!entities.empty()) m_physics.optimizeBroadPhase();
}

void PhysicsRuntime::destroyAll() {
    if (!m_simulating) return;
    std::vector<entt::entity> joints;
    for (auto& [e, h] : m_joints) joints.push_back(e);
    for (auto e : joints) destroyJoint(e);
    std::vector<entt::entity> bodies;
    for (auto& [e, rec] : m_bodies) bodies.push_back(e);
    for (auto e : bodies) destroyBody(e);
    m_pendingBodies.clear();
    m_pendingJoints.clear();
    m_simulating = false;
}

// ---- shapes & bodies ---------------------------------------------------------------------------------------

std::optional<physics::ShapeRef> PhysicsRuntime::buildShape(Entity e, const ColliderComponent& c,
                                                            const glm::vec3& worldScale) {
    const glm::vec3 s = c.scale * worldScale;
    physics::ShapeDesc desc;
    switch (c.type) {
    case ColliderType::Box:
    case ColliderType::Sphere:
    case ColliderType::Capsule:
    case ColliderType::Cylinder: desc = primitiveDesc(c.type, c.halfExtents, c.radius, c.halfHeight, {}, s); break;
    case ColliderType::ConvexHull:
    case ColliderType::Mesh: {
        std::shared_ptr<const MeshTriangles> mesh;
        if (c.mesh.isValid()) {
            if (!m_meshes) m_meshes = m_services ? m_services->tryGet<IMeshColliderProvider>() : nullptr;
            mesh = m_meshes ? m_meshes->meshTriangles(c.mesh) : nullptr;
            if (!mesh) {
                OX_LOG_WARN("gameplay", "collider of '{}': mesh {} not available (no IMeshColliderProvider?)",
                            e.name(), c.mesh.toString());
            }
        }
        const auto* rb = e.tryGet<RigidBodyComponent>();
        const bool dynamic = rb && rb->motionType == physics::MotionType::Dynamic && !e.has<NetworkProxyTag>();
        if (c.type == ColliderType::Mesh && mesh && !dynamic) {
            std::vector<glm::vec3> verts;
            verts.reserve(mesh->vertices.size());
            for (const auto& v : mesh->vertices) verts.push_back(v * s);
            desc = physics::ShapeDesc::triangleMesh(std::move(verts), mesh->indices);
        } else {
            if (c.type == ColliderType::Mesh && dynamic) {
                OX_LOG_WARN("gameplay", "'{}': dynamic bodies cannot use triangle meshes, using the convex hull",
                            e.name());
            }
            const std::vector<glm::vec3>& pts = !c.points.empty() || !mesh ? c.points : mesh->vertices;
            if (pts.size() < 4) {
                OX_LOG_WARN("gameplay", "'{}': convex hull needs at least 4 points", e.name());
                return std::nullopt;
            }
            desc = primitiveDesc(ColliderType::ConvexHull, {}, 0.f, 0.f, pts, s);
        }
        break;
    }
    case ColliderType::HeightField:
        if (c.sampleCount < 2 || c.heights.size() != usize(c.sampleCount) * c.sampleCount) {
            OX_LOG_WARN("gameplay", "'{}': height field needs sampleCount^2 heights", e.name());
            return std::nullopt;
        }
        desc = physics::ShapeDesc::heightField(c.heights, c.sampleCount, c.heightFieldOffset * s, c.heightFieldScale * s);
        break;
    case ColliderType::Compound: {
        std::vector<physics::CompoundChild> children;
        for (const auto& ch : c.children) {
            if (ch.type == ColliderType::ConvexHull && ch.points.size() < 4) continue;
            physics::CompoundChild child;
            child.shape = primitiveDesc(ch.type, ch.halfExtents, ch.radius, ch.halfHeight, ch.points, s);
            child.shape.density = c.density;
            child.position = ch.position * s;
            child.rotation = glm::normalize(ch.rotation);
            children.push_back(std::move(child));
        }
        if (children.empty()) {
            OX_LOG_WARN("gameplay", "'{}': compound collider without children", e.name());
            return std::nullopt;
        }
        desc = physics::ShapeDesc::compound(std::move(children));
        break;
    }
    }
    desc.convexRadius = c.convexRadius;
    desc.density = c.density;
    std::string error;
    physics::ShapeRef shape = m_physics.shapeCache().getOrCreate(desc, &error);
    if (!shape) {
        OX_LOG_WARN("gameplay", "'{}': collider shape failed: {}", e.name(), error);
        return std::nullopt;
    }
    const bool offset = glm::length2(c.offsetPosition) > 0.f || !sameRotation(c.offsetRotation, glm::quat(1, 0, 0, 0));
    if (offset) shape = physics::makeRotatedTranslated(shape, c.offsetPosition * s, glm::normalize(c.offsetRotation));
    return shape;
}

void PhysicsRuntime::createBody(Entity e) {
    if (!e.valid() || m_bodies.contains(e.handle())) return;
    const Transform wt = e.worldTransform();
    BodyRecord rec;
    rec.builtScale = wt.scale;
    rec.writtenPosition = wt.position;
    rec.writtenRotation = glm::normalize(wt.rotation);
    rec.current = rec.previous = poseOf(wt);

    if (auto* cc = e.tryGet<CharacterControllerComponent>(); cc && !e.has<RigidBodyComponent>()) {
        physics::CharacterDesc d;
        d.height = cc->height;
        d.radius = cc->radius;
        d.position = wt.position;
        d.rotation = rec.writtenRotation;
        d.maxSlopeAngle = glm::radians(cc->maxSlopeAngle);
        d.maxStepHeight = cc->maxStepHeight;
        d.stickToFloorDistance = cc->stickToFloorDistance;
        d.mass = cc->mass;
        d.maxStrength = cc->maxStrength;
        if (!cc->layer.empty()) {
            if (auto l = m_physics.layers().find(cc->layer)) d.layer = *l;
        }
        d.userData = toRuntimeId(e);
        rec.character = m_physics.createCharacter(d);
        if (!rec.character) {
            OX_LOG_WARN("gameplay", "'{}': character creation failed", e.name());
            return;
        }
        rec.motion = physics::MotionType::Kinematic;
        m_bodies.emplace(e.handle(), rec);
        return;
    }

    const auto* col = e.tryGet<ColliderComponent>();
    if (!col) return;
    auto shape = buildShape(e, *col, wt.scale);
    if (!shape) return;
    const auto* rb = e.tryGet<RigidBodyComponent>();

    physics::BodyDesc d;
    d.shape = *shape;
    d.position = wt.position;
    d.rotation = rec.writtenRotation;
    d.isSensor = col->isSensor || e.has<TriggerComponent>();
    physics::MotionType motion = rb ? rb->motionType : physics::MotionType::Static;
    if (motion == physics::MotionType::Dynamic && e.has<NetworkProxyTag>()) motion = physics::MotionType::Kinematic;
    // Triangle meshes and height fields can't be dynamic.
    if (motion == physics::MotionType::Dynamic && col->type == ColliderType::HeightField) motion = physics::MotionType::Static;
    d.motionType = motion;
    if (rb) {
        if (!rb->layer.empty()) {
            if (auto l = m_physics.layers().find(rb->layer)) d.layer = *l;
            else OX_LOG_WARN("gameplay", "'{}': unknown collision layer '{}'", e.name(), rb->layer);
        }
        d.mass = rb->mass;
        d.inertiaDiagonal = rb->inertiaOverride;
        d.friction = rb->friction;
        d.restitution = rb->restitution;
        d.linearDamping = rb->linearDamping;
        d.angularDamping = rb->angularDamping;
        d.gravityFactor = rb->gravityFactor;
        d.ccd = rb->ccd;
        d.allowSleeping = rb->allowSleeping;
        d.startActive = rb->startActive;
        d.lockAxes = rb->lockAxes;
        d.reportContacts = rb->reportContacts;
        d.linearVelocity = rb->initialLinearVelocity;
        d.angularVelocity = rb->initialAngularVelocity;
        d.allowMotionTypeChange = true;
    }
    d.userData = toRuntimeId(e);
    rec.body = m_physics.createBody(d);
    if (!rec.body) {
        OX_LOG_WARN("gameplay", "'{}': body creation failed", e.name());
        return;
    }
    rec.motion = motion;
    rec.interpolate = motion == physics::MotionType::Dynamic && rb && rb->interpolate;
    rec.current = rec.previous = m_physics.getTransform(rec.body);
    if (auto* rbc = e.tryGet<RigidBodyComponent>()) rbc->bodyId = rec.body.id;
    m_bodies.emplace(e.handle(), rec);
}

void PhysicsRuntime::destroyBody(entt::entity e) {
    auto it = m_bodies.find(e);
    if (it == m_bodies.end()) return;
    // Joints attached to this body die with it (Jolt destroys them); forget their handles.
    for (auto jt = m_joints.begin(); jt != m_joints.end();) {
        if (!m_physics.isValid(jt->second) || jt->first == e) {
            m_pendingJoints.insert(jt->first);
            jt = m_joints.erase(jt);
        } else {
            ++jt;
        }
    }
    if (it->second.character) m_physics.destroyCharacter(it->second.character);
    if (it->second.body && m_physics.isValid(it->second.body)) m_physics.destroyBody(it->second.body);
    if (m_world && m_world->valid(e)) {
        if (auto* rb = m_world->registry().try_get<RigidBodyComponent>(e)) rb->bodyId = physics::BodyHandle::kInvalid;
    }
    m_bodies.erase(it);
    // Joints whose target body is gone must be recreated (they now point at nothing).
    for (auto jt = m_joints.begin(); jt != m_joints.end();) {
        if (!m_physics.isValid(jt->second)) {
            m_pendingJoints.insert(jt->first);
            jt = m_joints.erase(jt);
        } else {
            ++jt;
        }
    }
}

void PhysicsRuntime::createJoint(Entity e) {
    if (!e.valid() || m_joints.contains(e.handle())) return;
    auto* j = e.tryGet<JointComponent>();
    if (!j || j->broken) return;
    const physics::BodyHandle a = bodyOf(e);
    if (!a) return;
    physics::ConstraintDesc d;
    d.type = j->type;
    d.bodyA = a;
    const Transform wa = e.worldTransform();
    d.pointA = wa.transformPoint(j->anchor);
    d.pointB = j->targetAnchor; // world space when attached to the world
    if (j->target.valid()) {
        const Entity t = m_world->resolve(j->target);
        if (!t.valid()) return;
        d.bodyB = bodyOf(t);
        if (!d.bodyB) return;
        d.pointB = t.worldTransform().transformPoint(j->targetAnchor);
    }
    if (d.type != physics::ConstraintType::Point && d.type != physics::ConstraintType::Distance) d.pointB = d.pointA;
    d.axis = glm::normalize(wa.rotation * (glm::length2(j->axis) > 0.f ? j->axis : glm::vec3(0, 1, 0)));
    d.limitsEnabled = j->limitsEnabled;
    d.limitMin = j->limitMin;
    d.limitMax = j->limitMax;
    d.motorMode = j->motorMode;
    d.motorTarget = j->motorTarget;
    d.motorMaxForce = j->motorMaxForce;
    d.minDistance = j->minDistance;
    d.maxDistance = j->maxDistance;
    d.springFrequency = j->springFrequency;
    d.springDamping = j->springDamping;
    d.coneHalfAngle = glm::radians(j->coneHalfAngle);
    d.breakForce = j->breakForce > 0.f ? j->breakForce : std::numeric_limits<f32>::infinity();
    d.breakTorque = j->breakTorque > 0.f ? j->breakTorque : std::numeric_limits<f32>::infinity();
    const physics::ConstraintHandle h = m_physics.createConstraint(d);
    if (h) m_joints.emplace(e.handle(), h);
}

void PhysicsRuntime::destroyJoint(entt::entity e) {
    auto it = m_joints.find(e);
    if (it == m_joints.end()) return;
    if (m_physics.isValid(it->second)) m_physics.destroyConstraint(it->second);
    m_joints.erase(it);
}

void PhysicsRuntime::processPending() {
    if (!m_simulating || !m_world) return;
    if (!m_pendingBodies.empty()) {
        std::vector<entt::entity> pending(m_pendingBodies.begin(), m_pendingBodies.end());
        m_pendingBodies.clear();
        std::sort(pending.begin(), pending.end());
        entt::registry& r = m_world->registry();
        for (auto e : pending) {
            glm::vec3 lin{0.f}, ang{0.f};
            bool hadDynamic = false;
            if (auto it = m_bodies.find(e); it != m_bodies.end()) {
                if (it->second.body && it->second.motion == physics::MotionType::Dynamic) {
                    lin = m_physics.getLinearVelocity(it->second.body);
                    ang = m_physics.getAngularVelocity(it->second.body);
                    hadDynamic = true;
                }
                destroyBody(e);
            }
            if (!r.valid(e) || r.all_of<PendingDestroyTag>(e)) continue;
            if (!r.any_of<ColliderComponent, CharacterControllerComponent>(e)) continue;
            createBody(m_world->wrap(e));
            if (hadDynamic) {
                auto it = m_bodies.find(e);
                if (it != m_bodies.end() && it->second.body && it->second.motion == physics::MotionType::Dynamic) {
                    m_physics.setLinearVelocity(it->second.body, lin);
                    m_physics.setAngularVelocity(it->second.body, ang);
                }
            }
            if (r.all_of<JointComponent>(e)) m_pendingJoints.insert(e);
        }
    }
    if (!m_pendingJoints.empty()) {
        std::vector<entt::entity> pending(m_pendingJoints.begin(), m_pendingJoints.end());
        m_pendingJoints.clear();
        for (auto e : pending) {
            destroyJoint(e);
            if (m_world->valid(e)) createJoint(m_world->wrap(e));
        }
    }
}

// ---- simulation ----------------------------------------------------------------------------------------------

void PhysicsRuntime::writePose(Entity e, BodyRecord& rec, const physics::Transform& pose) {
    Transform wt = e.worldTransform();
    wt.position = pose.position;
    wt.rotation = pose.rotation;
    e.setWorldTransform(wt);
    rec.writtenPosition = pose.position;
    rec.writtenRotation = pose.rotation;
}

void PhysicsRuntime::syncBodiesIn(f32 dt) {
    entt::registry& r = m_world->registry();
    for (auto& [handle, rec] : m_bodies) {
        if (!r.valid(handle)) continue;
        const Entity e = m_world->wrap(handle);
        const Transform wt = e.worldTransform();
        const glm::quat rot = glm::normalize(wt.rotation);

        if (rec.character) {
            auto& cc = r.get<CharacterControllerComponent>(handle);
            if (!samePosition(wt.position, rec.writtenPosition)) {
                m_physics.setCharacterTransform(rec.character, wt.position, rot);
            }
            if (r.all_of<ExternalCharacterMotionTag>(handle)) continue;
            physics::CharacterMoveInput in;
            in.desiredVelocity = cc.desiredVelocity;
            in.jump = cc.jump;
            in.jumpSpeed = cc.jumpSpeed;
            in.airControl = cc.airControl;
            m_physics.moveCharacter(rec.character, dt, in);
            cc.jump = false;
            continue;
        }
        if (!rec.body) continue;

        if (!sameScale(wt.scale, rec.builtScale)) {
            if (const auto* col = r.try_get<ColliderComponent>(handle)) {
                if (auto shape = buildShape(e, *col, wt.scale)) {
                    m_physics.setShape(rec.body, *shape, rec.motion == physics::MotionType::Dynamic);
                }
            }
            rec.builtScale = wt.scale;
        }

        switch (rec.motion) {
        case physics::MotionType::Static:
            if (!samePosition(wt.position, rec.current.position) || !sameRotation(rot, rec.current.rotation)) {
                m_physics.setTransform(rec.body, wt.position, rot, physics::Activation::DontActivate);
                rec.current = rec.previous = {wt.position, rot};
                rec.writtenPosition = wt.position;
                rec.writtenRotation = rot;
            }
            break;
        case physics::MotionType::Kinematic: {
            const physics::Transform cur = m_physics.getTransform(rec.body);
            if (!samePosition(wt.position, cur.position) || !sameRotation(rot, cur.rotation) ||
                glm::length2(m_physics.getLinearVelocity(rec.body)) > 0.f ||
                glm::length2(m_physics.getAngularVelocity(rec.body)) > 0.f) {
                m_physics.moveKinematic(rec.body, wt.position, rot, dt);
            }
            rec.writtenPosition = wt.position;
            rec.writtenRotation = rot;
            break;
        }
        case physics::MotionType::Dynamic:
            if (!samePosition(wt.position, rec.writtenPosition) || !sameRotation(rot, rec.writtenRotation)) {
                // Moved by gameplay code: teleport.
                m_physics.setTransform(rec.body, wt.position, rot);
                rec.current = rec.previous = {wt.position, rot};
                rec.writtenPosition = wt.position;
                rec.writtenRotation = rot;
            }
            break;
        }
    }
}

void PhysicsRuntime::syncBodiesOut() {
    entt::registry& r = m_world->registry();
    for (auto& [handle, rec] : m_bodies) {
        if (!r.valid(handle)) continue;
        const Entity e = m_world->wrap(handle);
        if (rec.character) {
            const physics::CharacterState s = m_physics.getCharacterState(rec.character);
            auto& cc = r.get<CharacterControllerComponent>(handle);
            cc.groundState = s.groundState;
            cc.velocity = s.linearVelocity;
            Transform wt = e.worldTransform();
            wt.position = s.position;
            e.setWorldTransform(wt);
            rec.writtenPosition = s.position;
            continue;
        }
        if (!rec.body || rec.motion != physics::MotionType::Dynamic) continue;
        rec.previous = rec.current;
        rec.current = m_physics.getTransform(rec.body);
        if (!rec.interpolate) writePose(e, rec, rec.current);
    }
}

void PhysicsRuntime::interpolate(f32 alpha) {
    if (!m_simulating || !m_world) return;
    entt::registry& r = m_world->registry();
    for (auto& [handle, rec] : m_bodies) {
        if (!rec.interpolate || !rec.body || !r.valid(handle)) continue;
        const Entity e = m_world->wrap(handle);
        const Transform wt = e.worldTransform();
        const glm::quat rot = glm::normalize(wt.rotation);
        if (!samePosition(wt.position, rec.writtenPosition) || !sameRotation(rot, rec.writtenRotation)) {
            // Teleported after the last fixed step (e.g. by a script in Update): keep the new pose.
            m_physics.setTransform(rec.body, wt.position, rot);
            rec.current = rec.previous = {wt.position, rot};
            rec.writtenPosition = wt.position;
            rec.writtenRotation = rot;
            continue;
        }
        writePose(e, rec, physics::TransformInterpolator::interpolate(rec.previous, rec.current, alpha));
    }
}

void PhysicsRuntime::fixedStep(f32 dt) {
    if (!m_simulating || !m_world) return;
    OX_PROFILE_ZONE_N("PhysicsRuntime::fixedStep");
    processPending();
    syncBodiesIn(dt);
    m_physics.step(dt);
    syncBodiesOut();
    dispatchEvents();
}

void PhysicsRuntime::dispatchEvents() {
    entt::registry& r = m_world->registry();
    for (const physics::ContactEvent& c : m_physics.contactEvents()) {
        CollisionEvent ev;
        ev.phase = static_cast<ContactPhase>(c.type);
        ev.a = entityFromRuntimeId(*m_world, c.userDataA);
        ev.b = entityFromRuntimeId(*m_world, c.userDataB);
        if (!ev.a.valid() && !ev.b.valid()) continue;
        ev.normal = c.normal;
        ev.point = c.pointCount > 0 ? c.points[0] : glm::vec3(0.f);
        ev.impulse = c.normalImpulse;
        onCollision.emit(ev);
        if (m_bus) m_bus->publish(ev);
    }
    for (const physics::TriggerEvent& t : m_physics.triggerEvents()) {
        TriggerEvent ev;
        ev.phase = static_cast<TriggerPhase>(t.type);
        ev.trigger = entityFromRuntimeId(*m_world, t.triggerUserData);
        ev.other = entityFromRuntimeId(*m_world, t.otherUserData);
        if (!ev.trigger.valid()) continue;
        if (auto* tc = r.try_get<TriggerComponent>(ev.trigger.handle())) {
            if (!tc->requiredTag.empty()) {
                const auto* tags = ev.other.valid() ? ev.other.tryGet<TagComponent>() : nullptr;
                if (!tags || !tags->has(tc->requiredTag)) continue;
            }
            if (ev.phase == TriggerPhase::Stay && !tc->reportStay) continue;
            if (ev.phase == TriggerPhase::Enter) {
                if (tc->once && tc->fired) continue;
                tc->fired = true;
                ++tc->overlapCount;
            } else if (ev.phase == TriggerPhase::Exit) {
                if (tc->overlapCount > 0) --tc->overlapCount;
                if (tc->once) continue;
            }
        } else if (ev.phase == TriggerPhase::Stay) {
            continue;
        }
        onTrigger.emit(ev);
        if (m_bus) m_bus->publish(ev);
    }
    for (const physics::ConstraintBrokenEvent& b : m_physics.constraintBrokenEvents()) {
        for (auto& [e, h] : m_joints) {
            if (h != b.constraint) continue;
            if (auto* j = r.try_get<JointComponent>(e)) j->broken = true;
            JointBrokenEvent ev{m_world->wrap(e)};
            onJointBroken.emit(ev);
            if (m_bus) m_bus->publish(ev);
        }
    }
}

// ---- queries & helpers -----------------------------------------------------------------------------------

Entity PhysicsRuntime::entityOf(physics::BodyHandle body) const {
    if (!m_world || !body || !m_physics.isValid(body)) return {};
    return entityFromRuntimeId(*m_world, m_physics.getUserData(body));
}

physics::BodyHandle PhysicsRuntime::bodyOf(Entity e) const {
    if (!e.valid()) return {};
    auto it = m_bodies.find(e.handle());
    if (it == m_bodies.end()) return {};
    if (it->second.character) return m_physics.getCharacterInnerBody(it->second.character);
    return it->second.body;
}

physics::CharacterHandle PhysicsRuntime::characterOf(Entity e) const {
    if (!e.valid()) return {};
    auto it = m_bodies.find(e.handle());
    return it == m_bodies.end() ? physics::CharacterHandle{} : it->second.character;
}

std::optional<PhysicsRaycastHit> PhysicsRuntime::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                                         f32 maxDistance, Entity ignore, bool includeSensors) const {
    if (!m_simulating || glm::length2(direction) <= 0.f) return std::nullopt;
    physics::QueryFilter filter;
    filter.includeSensors = includeSensors;
    physics::BodyHandle ignored[1] = {bodyOf(ignore)};
    if (ignored[0]) filter.ignoreBodies = ignored;
    auto hit = m_physics.raycast(origin, direction, maxDistance, filter);
    if (!hit) return std::nullopt;
    PhysicsRaycastHit out;
    out.body = hit->body;
    out.entity = m_world ? entityFromRuntimeId(*m_world, hit->userData) : Entity{};
    out.point = hit->point;
    out.normal = hit->normal;
    out.distance = hit->distance;
    return out;
}

std::vector<Entity> PhysicsRuntime::overlapSphere(const glm::vec3& center, f32 radius, bool includeSensors) const {
    std::vector<Entity> out;
    if (!m_simulating || !m_world) return out;
    physics::QueryFilter filter;
    filter.includeSensors = includeSensors;
    for (physics::BodyHandle b : m_physics.overlapSphere(center, radius, filter)) {
        if (Entity e = entityOf(b); e.valid()) out.push_back(e);
    }
    return out;
}

u32 PhysicsRuntime::countHits(const glm::vec3& a, const glm::vec3& b, const std::function<bool(Entity)>& ignore) const {
    if (!m_simulating || !m_world) return 0;
    const glm::vec3 d = b - a;
    const f32 len = glm::length(d);
    if (len < 1e-4f) return 0;
    u32 hits = 0;
    std::unordered_set<u32> seen;
    for (const physics::RayHit& h : m_physics.raycastAll(a, d, len)) {
        if (!seen.insert(h.body.id).second) continue;
        const Entity e = entityFromRuntimeId(*m_world, h.userData);
        if (ignore && e.valid() && ignore(e)) continue;
        ++hits;
    }
    return hits;
}

void PhysicsRuntime::addForce(Entity e, const glm::vec3& f) {
    if (auto b = bodyOf(e)) m_physics.addForce(b, f);
}
void PhysicsRuntime::addImpulse(Entity e, const glm::vec3& i) {
    if (auto b = bodyOf(e)) m_physics.addImpulse(b, i);
}
void PhysicsRuntime::addImpulseAtPoint(Entity e, const glm::vec3& i, const glm::vec3& p) {
    if (auto b = bodyOf(e)) m_physics.addImpulseAtPoint(b, i, p);
}
void PhysicsRuntime::addTorque(Entity e, const glm::vec3& t) {
    if (auto b = bodyOf(e)) m_physics.addTorque(b, t);
}
glm::vec3 PhysicsRuntime::linearVelocity(Entity e) const {
    if (auto ch = characterOf(e)) return m_physics.getCharacterState(ch).linearVelocity;
    auto b = bodyOf(e);
    return b ? m_physics.getLinearVelocity(b) : glm::vec3(0.f);
}
void PhysicsRuntime::setLinearVelocity(Entity e, const glm::vec3& v) {
    if (auto ch = characterOf(e)) return m_physics.setCharacterVelocity(ch, v);
    if (auto b = bodyOf(e)) m_physics.setLinearVelocity(b, v);
}
glm::vec3 PhysicsRuntime::angularVelocity(Entity e) const {
    auto b = bodyOf(e);
    return b && !characterOf(e) ? m_physics.getAngularVelocity(b) : glm::vec3(0.f);
}
void PhysicsRuntime::setAngularVelocity(Entity e, const glm::vec3& v) {
    if (characterOf(e)) return;
    if (auto b = bodyOf(e)) m_physics.setAngularVelocity(b, v);
}

void PhysicsRuntime::teleport(Entity e, const glm::vec3& position, const glm::quat& rotation) {
    if (!e.valid()) return;
    Transform wt = e.worldTransform();
    wt.position = position;
    wt.rotation = glm::normalize(rotation);
    e.setWorldTransform(wt);
    auto it = m_bodies.find(e.handle());
    if (it == m_bodies.end()) return;
    BodyRecord& rec = it->second;
    if (rec.character) m_physics.setCharacterTransform(rec.character, position, wt.rotation);
    else if (rec.body) m_physics.setTransform(rec.body, position, wt.rotation);
    rec.current = rec.previous = {position, wt.rotation};
    rec.writtenPosition = position;
    rec.writtenRotation = wt.rotation;
}

// ---- debug draw ----------------------------------------------------------------------------------------------

void PhysicsRuntime::drawDebug(DebugDraw& draw, bool playing) {
    if (!debugDraw || !m_world) return;
    if (playing && m_simulating) {
        DebugDrawSink sink(draw);
        m_physics.debugDraw(sink, debugOptions);
        return;
    }
    // Edit mode: wireframes from component data.
    auto view = m_world->registry().view<ColliderComponent>();
    for (auto handle : view) {
        const Entity e = m_world->wrap(handle);
        const auto& c = view.get<ColliderComponent>(handle);
        const Transform wt = e.worldTransform();
        const glm::vec3 s = c.scale * wt.scale;
        const glm::quat rot = glm::normalize(wt.rotation * c.offsetRotation);
        const glm::vec3 center = wt.position + wt.rotation * (c.offsetPosition * s);
        const DebugColor color = c.isSensor || e.has<TriggerComponent>() ? debug_color::kYellow
                                 : e.has<RigidBodyComponent>()           ? debug_color::kGreen
                                                                         : debug_color::kGray;
        const glm::vec3 as = glm::abs(s);
        const glm::vec3 up = rot * glm::vec3(0, 1, 0);
        switch (c.type) {
        case ColliderType::Box: draw.box(center, c.halfExtents * as, rot, color); break;
        case ColliderType::Sphere: draw.sphere(center, c.radius * maxAbs(s), color); break;
        case ColliderType::Capsule: {
            const f32 hh = c.halfHeight * as.y;
            draw.capsule(center - up * hh, center + up * hh, c.radius * std::max(as.x, as.z), color);
            break;
        }
        case ColliderType::Cylinder: {
            const f32 hh = c.halfHeight * as.y;
            draw.cylinder(center - up * hh, center + up * hh, c.radius * std::max(as.x, as.z), color);
            break;
        }
        case ColliderType::Compound:
            for (const auto& ch : c.children) {
                const glm::vec3 cc = center + rot * (ch.position * s);
                const glm::quat cr = rot * ch.rotation;
                if (ch.type == ColliderType::Sphere) draw.sphere(cc, ch.radius * maxAbs(s), color);
                else draw.box(cc, ch.halfExtents * as, cr, color);
            }
            break;
        default: draw.axes(Transform{center, rot, glm::vec3(1.f)}, 0.5f); break;
        }
    }
    for (auto handle : m_world->registry().view<CharacterControllerComponent>()) {
        const Entity e = m_world->wrap(handle);
        const auto& cc = e.get<CharacterControllerComponent>();
        const glm::vec3 feet = e.worldPosition();
        draw.capsule(feet + glm::vec3(0, cc.radius, 0), feet + glm::vec3(0, cc.height - cc.radius, 0), cc.radius,
                     debug_color::kMagenta);
    }
}

} // namespace ox::gameplay
