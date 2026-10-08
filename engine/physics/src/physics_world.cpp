#include "world_impl.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>

#include <algorithm>
#include <cstring>
#include <thread>

namespace ox::physics {

using detail::toGlm;
using detail::toHandle;
using detail::toJolt;
using detail::toJoltR;

// =================================================================================================
// Impl
// =================================================================================================
PhysicsWorld::Impl::Impl(const PhysicsWorldDesc& desc) : layers(desc.layers), reportPersist(desc.reportPersistContacts) {
    detail::ensureJoltInitialized();
    OX_ASSERT(layers.layerCount() > 0, "CollisionLayers must define at least one layer");
    OX_ASSERT(layers.broadPhaseLayerCount() <= broadphase::kMaxLayers, "too many broad-phase layers");

    tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(u32(desc.tempAllocatorBytes));
    if (desc.jobExecutor) {
        jobSystem = std::make_unique<detail::ExecutorJobSystem>(*desc.jobExecutor);
    } else {
        int threads = desc.workerThreads;
        if (threads < 0) {
            threads = std::max(0, int(std::thread::hardware_concurrency()) - 1);
        }
        jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
    }

    system = std::make_unique<JPH::PhysicsSystem>();
    system->Init(desc.maxBodies, desc.numBodyMutexes, desc.maxBodyPairs, desc.maxContactConstraints, bpInterface,
                 objVsBp, objPair);
    system->SetGravity(toJolt(desc.gravity));
    JPH::PhysicsSettings settings = system->GetPhysicsSettings();
    settings.mNumVelocitySteps = std::max(1u, desc.velocitySteps);
    settings.mNumPositionSteps = desc.positionSteps;
    settings.mPenetrationSlop = desc.penetrationSlop;
    settings.mSpeculativeContactDistance = desc.speculativeContactDistance;
    settings.mTimeBeforeSleep = desc.timeBeforeSleep;
    system->SetPhysicsSettings(settings);
    bodyFlags.assign(desc.maxBodies, 0);
    contactListener.bodyFlags = &bodyFlags;
    contactListener.reportPersist = reportPersist;
    system->SetContactListener(&contactListener);
}

PhysicsWorld::Impl::~Impl() {
    // Constraints and characters reference bodies; release them before the system goes away.
    for (auto& c : constraints) {
        if (c.constraint) {
            system->RemoveConstraint(c.constraint);
        }
    }
    constraints.clear();
    characters.clear();

    JPH::BodyIDVector ids;
    system->GetBodies(ids);
    if (!ids.empty()) {
        auto& bi = bodies();
        JPH::BodyIDVector added;
        for (const auto& id : ids) {
            if (bi.IsAdded(id)) {
                added.push_back(id);
            }
        }
        if (!added.empty()) {
            bi.RemoveBodies(added.data(), int(added.size()));
        }
        bi.DestroyBodies(ids.data(), int(ids.size()));
    }
    system.reset();
}

void PhysicsWorld::Impl::setBodyFlags(const JPH::BodyID& id, u8 flags) {
    if (id.GetIndex() < bodyFlags.size()) {
        bodyFlags[id.GetIndex()] = flags;
    }
}

void PhysicsWorld::Impl::processContacts() {
    using detail::RawKind;
    std::vector<detail::RawContact> raw = contactListener.take();
    // Jolt invokes callbacks from worker threads in arbitrary order; sort to make dispatch deterministic.
    std::sort(raw.begin(), raw.end(), [](const detail::RawContact& a, const detail::RawContact& b) {
        return std::tie(a.kind, a.key) < std::tie(b.kind, b.key);
    });

    auto pairKey = [](const detail::SubPairKey& k) { return (u64(k.body1) << 32) | k.body2; };
    auto makeContact = [](ContactEventType type, const detail::SubPairKey& k, const detail::PairState& p) {
        ContactEvent e;
        e.type = type;
        e.bodyA = BodyHandle{k.body1};
        e.bodyB = BodyHandle{k.body2};
        e.userDataA = p.userData1;
        e.userDataB = p.userData2;
        return e;
    };
    auto makeTrigger = [](TriggerEventType type, u64 key, const detail::PairState& p) {
        TriggerEvent e;
        e.type = type;
        BodyHandle b1{u32(key >> 32)}, b2{u32(key & 0xffffffffu)};
        e.trigger = p.body1IsTrigger ? b1 : b2;
        e.other = p.body1IsTrigger ? b2 : b1;
        e.triggerUserData = p.body1IsTrigger ? p.userData1 : p.userData2;
        e.otherUserData = p.body1IsTrigger ? p.userData2 : p.userData1;
        return e;
    };
    auto fillContact = [](ContactEvent& e, const detail::RawContact& r) {
        e.normal = r.normal;
        e.penetration = r.penetration;
        e.pointCount = r.pointCount;
        e.points = r.points;
        e.normalImpulse = r.impulse;
    };

    const JPH::BodyInterface& noLock = system->GetBodyInterfaceNoLock();
    auto isActive = [&](u32 id) { return noLock.IsActive(JPH::BodyID(id)); };

    for (const auto& r : raw) {
        u64 pk = pairKey(r.key);
        switch (r.kind) {
        case RawKind::Added:
        case RawKind::Persisted: {
            auto [sub, inserted] = subPairs.try_emplace(r.key, stepCount);
            sub->second = stepCount;
            auto& p = pairs[pk];
            if (inserted && p.subCount++ == 0) {
                p.sensor = r.sensor;
                p.body1IsTrigger = r.body1IsTrigger;
                p.userData1 = r.userData1;
                p.userData2 = r.userData2;
                p.begunStep = stepCount;
                if (p.sensor) {
                    triggerEvents.push_back(makeTrigger(TriggerEventType::Enter, pk, p));
                } else {
                    ContactEvent e = makeContact(ContactEventType::Begin, r.key, p);
                    fillContact(e, r);
                    contactEvents.push_back(e);
                }
            } else if (r.kind == RawKind::Persisted && reportPersist && !p.sensor && p.begunStep != stepCount &&
                       p.persistStep != stepCount) {
                p.persistStep = stepCount;
                ContactEvent e = makeContact(ContactEventType::Persist, r.key, p);
                fillContact(e, r);
                e.normalImpulse = 0.f;
                contactEvents.push_back(e);
            }
            break;
        }
        case RawKind::Removed: {
            auto it = subPairs.find(r.key);
            // Jolt drops contacts of bodies that fall asleep; keep them (frozen) so resting objects don't
            // produce End/Exit. Stale frozen contacts are swept below once a body wakes up.
            if (it != subPairs.end() && (isActive(r.key.body1) || isActive(r.key.body2))) {
                removeSubPair(it);
            }
            break;
        }
        }
    }

    for (auto it = subPairs.begin(); it != subPairs.end();) {
        if (it->second != stepCount && (isActive(it->first.body1) || isActive(it->first.body2))) {
            auto next = std::next(it);
            removeSubPair(it);
            it = next;
        } else {
            ++it;
        }
    }

    for (const auto& [pk, p] : pairs) {
        if (p.sensor && p.begunStep != stepCount) {
            triggerEvents.push_back(makeTrigger(TriggerEventType::Stay, pk, p));
        }
    }
}

// Removes a sub-shape contact; emits End/Exit when it was the last one of its body pair.
bool PhysicsWorld::Impl::removeSubPair(std::map<detail::SubPairKey, u64>::iterator sub) {
    detail::SubPairKey key = sub->first;
    subPairs.erase(sub);
    u64 pk = (u64(key.body1) << 32) | key.body2;
    auto it = pairs.find(pk);
    if (it == pairs.end() || --it->second.subCount > 0) {
        return false;
    }
    const auto& p = it->second;
    if (p.sensor) {
        TriggerEvent e;
        e.type = TriggerEventType::Exit;
        e.trigger = BodyHandle{p.body1IsTrigger ? key.body1 : key.body2};
        e.other = BodyHandle{p.body1IsTrigger ? key.body2 : key.body1};
        e.triggerUserData = p.body1IsTrigger ? p.userData1 : p.userData2;
        e.otherUserData = p.body1IsTrigger ? p.userData2 : p.userData1;
        triggerEvents.push_back(e);
    } else {
        ContactEvent e;
        e.type = ContactEventType::End;
        e.bodyA = BodyHandle{key.body1};
        e.bodyB = BodyHandle{key.body2};
        e.userDataA = p.userData1;
        e.userDataB = p.userData2;
        contactEvents.push_back(e);
    }
    pairs.erase(it);
    return true;
}

void PhysicsWorld::Impl::purgeBodyPairs(u32 bodyId) {
    for (auto it = pairs.begin(); it != pairs.end();) {
        u32 b1 = u32(it->first >> 32), b2 = u32(it->first & 0xffffffffu);
        if (b1 != bodyId && b2 != bodyId) {
            ++it;
            continue;
        }
        const auto& p = it->second;
        if (p.sensor) {
            TriggerEvent e;
            e.type = TriggerEventType::Exit;
            e.trigger = BodyHandle{p.body1IsTrigger ? b1 : b2};
            e.other = BodyHandle{p.body1IsTrigger ? b2 : b1};
            e.triggerUserData = p.body1IsTrigger ? p.userData1 : p.userData2;
            e.otherUserData = p.body1IsTrigger ? p.userData2 : p.userData1;
            pendingTriggerEvents.push_back(e);
        } else {
            ContactEvent e;
            e.type = ContactEventType::End;
            e.bodyA = BodyHandle{b1};
            e.bodyB = BodyHandle{b2};
            e.userDataA = p.userData1;
            e.userDataB = p.userData2;
            pendingContactEvents.push_back(e);
        }
        it = pairs.erase(it);
    }
    for (auto it = subPairs.begin(); it != subPairs.end();) {
        if (it->first.body1 == bodyId || it->first.body2 == bodyId) {
            it = subPairs.erase(it);
        } else {
            ++it;
        }
    }
}

void PhysicsWorld::Impl::dispatchEvents() {
    if (contactCallback) {
        for (const auto& e : contactEvents) {
            contactCallback(e);
        }
    }
    if (triggerCallback) {
        for (const auto& e : triggerEvents) {
            triggerCallback(e);
        }
    }
    if (brokenCallback) {
        for (const auto& e : brokenEvents) {
            brokenCallback(e);
        }
    }
}

PhysicsWorld::Impl::CharacterSlot* PhysicsWorld::Impl::character(CharacterHandle h) {
    if (h.index >= characters.size() || characters[h.index].generation != h.generation ||
        !characters[h.index].character) {
        return nullptr;
    }
    return &characters[h.index];
}
const PhysicsWorld::Impl::CharacterSlot* PhysicsWorld::Impl::character(CharacterHandle h) const {
    return const_cast<Impl*>(this)->character(h);
}
PhysicsWorld::Impl::ConstraintSlot* PhysicsWorld::Impl::constraint(ConstraintHandle h) {
    if (h.index >= constraints.size() || constraints[h.index].generation != h.generation ||
        !constraints[h.index].constraint) {
        return nullptr;
    }
    return &constraints[h.index];
}
const PhysicsWorld::Impl::ConstraintSlot* PhysicsWorld::Impl::constraint(ConstraintHandle h) const {
    return const_cast<Impl*>(this)->constraint(h);
}

// =================================================================================================
// World
// =================================================================================================
PhysicsWorld::PhysicsWorld(const PhysicsWorldDesc& desc) : m_impl(std::make_unique<Impl>(desc)) {}
PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::step(f32 dt, u32 collisionSteps) {
    auto& w = *m_impl;
    collisionSteps = std::max(1u, collisionSteps);
    w.contactEvents.clear();
    w.triggerEvents.clear();
    w.brokenEvents.clear();
    w.contactEvents.swap(w.pendingContactEvents);
    w.triggerEvents.swap(w.pendingTriggerEvents);

    ++w.stepCount;
    w.lastSubStepDt = dt / f32(collisionSteps);
    JPH::EPhysicsUpdateError err = w.system->Update(dt, int(collisionSteps), w.tempAllocator.get(), w.jobSystem.get());
    if (err != JPH::EPhysicsUpdateError::None) {
        OX_LOG_WARN("physics", "PhysicsSystem::Update reported error flags 0x{:x} (raise PhysicsWorldDesc limits)",
                    u32(err));
    }
    w.processContacts();
    w.checkBreakableConstraints();
    w.dispatchEvents();
}

u64 PhysicsWorld::stepCount() const { return m_impl->stepCount; }
void PhysicsWorld::setGravity(const glm::vec3& g) { m_impl->system->SetGravity(toJolt(g)); }
glm::vec3 PhysicsWorld::gravity() const { return toGlm(m_impl->system->GetGravity()); }
const CollisionLayers& PhysicsWorld::layers() const { return m_impl->layers; }
void PhysicsWorld::optimizeBroadPhase() { m_impl->system->OptimizeBroadPhase(); }
ShapeCache& PhysicsWorld::shapeCache() { return m_impl->shapeCache; }
u32 PhysicsWorld::bodyCount() const { return m_impl->system->GetNumBodies(); }
u32 PhysicsWorld::activeBodyCount() const {
    return m_impl->system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
}
void* PhysicsWorld::nativeSystem() const { return m_impl->system.get(); }

// ---- Bodies ---------------------------------------------------------------------------------------
namespace {
JPH::EMotionType toJolt(MotionType t) {
    switch (t) {
    case MotionType::Static: return JPH::EMotionType::Static;
    case MotionType::Kinematic: return JPH::EMotionType::Kinematic;
    default: return JPH::EMotionType::Dynamic;
    }
}
JPH::EActivation toJolt(Activation a) {
    return a == Activation::Activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
}
} // namespace

BodyHandle PhysicsWorld::createBody(const BodyDesc& desc) {
    auto& w = *m_impl;
    if (!desc.shape) {
        OX_LOG_ERROR("physics", "createBody: no shape");
        return {};
    }
    const auto* shape = static_cast<const JPH::Shape*>(desc.shape.native());
    ObjectLayer layer = desc.layer;
    if (layer == layers::Auto) {
        layer = desc.isSensor                               ? layers::Trigger
                : desc.motionType == MotionType::Static    ? layers::Static
                : desc.motionType == MotionType::Kinematic ? layers::Kinematic
                                                           : layers::Dynamic;
    }
    if (layer >= w.layers.layerCount()) {
        OX_LOG_ERROR("physics", "createBody: object layer {} not defined", layer);
        return {};
    }

    JPH::BodyCreationSettings s(shape, toJoltR(desc.position), toJolt(glm::normalize(desc.rotation)),
                                toJolt(desc.motionType), JPH::ObjectLayer(layer));
    s.mLinearVelocity = toJolt(desc.linearVelocity);
    s.mAngularVelocity = toJolt(desc.angularVelocity);
    s.mUserData = desc.userData;
    s.mIsSensor = desc.isSensor;
    // Static/kinematic sensors must also see kinematic bodies (e.g. character inner bodies).
    s.mCollideKinematicVsNonDynamic = desc.isSensor;
    s.mFriction = desc.friction;
    s.mRestitution = desc.restitution;
    s.mLinearDamping = desc.linearDamping;
    s.mAngularDamping = desc.angularDamping;
    s.mGravityFactor = desc.gravityFactor;
    s.mMaxLinearVelocity = desc.maxLinearVelocity;
    s.mMaxAngularVelocity = desc.maxAngularVelocity;
    s.mMotionQuality = desc.ccd ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    s.mAllowSleeping = desc.allowSleeping;
    s.mAllowDynamicOrKinematic = desc.allowMotionTypeChange || desc.motionType != MotionType::Static;

    u8 allowed = u8(~desc.lockAxes) & u8(JPH::EAllowedDOFs::All);
    if (desc.motionType == MotionType::Dynamic && allowed == 0) {
        OX_LOG_WARN("physics", "createBody: all DOFs locked on a dynamic body, unlocking rotation");
        allowed = u8(JPH::EAllowedDOFs::RotationX | JPH::EAllowedDOFs::RotationY | JPH::EAllowedDOFs::RotationZ);
    }
    s.mAllowedDOFs = JPH::EAllowedDOFs(allowed);

    // Mesh/height-field shapes have no mass properties; moving ones need explicit values.
    JPH::EShapeSubType leaf = shape->GetSubType();
    if (leaf == JPH::EShapeSubType::Scaled || leaf == JPH::EShapeSubType::OffsetCenterOfMass ||
        leaf == JPH::EShapeSubType::RotatedTranslated) {
        leaf = static_cast<const JPH::DecoratedShape*>(shape)->GetInnerShape()->GetSubType();
    }
    bool massless = leaf == JPH::EShapeSubType::Mesh || leaf == JPH::EShapeSubType::HeightField;
    if (desc.motionType != MotionType::Static && (massless || desc.mass > 0.f)) {
        f32 mass = desc.mass > 0.f ? desc.mass : 1.f;
        if (massless && desc.motionType == MotionType::Dynamic && desc.mass <= 0.f) {
            OX_LOG_WARN("physics", "createBody: dynamic mesh/height-field body without mass, using 1 kg");
        }
        s.mMassPropertiesOverride.mMass = mass;
        if (desc.inertiaDiagonal != glm::vec3(0.f) || massless) {
            glm::vec3 inertia = desc.inertiaDiagonal;
            if (inertia == glm::vec3(0.f)) {
                // Solid box inertia from the local bounds.
                JPH::Vec3 e = shape->GetLocalBounds().GetExtent() * 2.f;
                f32 k = mass / 12.f;
                inertia = {k * (e.GetY() * e.GetY() + e.GetZ() * e.GetZ()), k * (e.GetX() * e.GetX() + e.GetZ() * e.GetZ()),
                           k * (e.GetX() * e.GetX() + e.GetY() * e.GetY())};
            }
            s.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            s.mMassPropertiesOverride.mInertia = JPH::Mat44::sScale(toJolt(inertia));
        } else {
            s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        }
    }

    JPH::EActivation activation = desc.motionType != MotionType::Static && desc.startActive
                                      ? JPH::EActivation::Activate
                                      : JPH::EActivation::DontActivate;
    JPH::BodyID id = w.bodies().CreateAndAddBody(s, activation);
    if (id.IsInvalid()) {
        OX_LOG_ERROR("physics", "createBody: body limit reached ({})", w.system->GetMaxBodies());
        return {};
    }
    w.setBodyFlags(id, desc.reportContacts ? detail::kFlagReportContacts : 0);
    return toHandle(id);
}

void PhysicsWorld::destroyBody(BodyHandle body) {
    auto& w = *m_impl;
    if (!isValid(body)) {
        return;
    }
    for (u32 i = 0; i < w.constraints.size(); ++i) {
        auto& c = w.constraints[i];
        if (c.constraint && (c.a == body || c.b == body)) {
            destroyConstraint(ConstraintHandle{i, c.generation});
        }
    }
    JPH::BodyID id = toJolt(body);
    w.bodies().RemoveBody(id);
    w.bodies().DestroyBody(id);
    w.setBodyFlags(id, 0);
    w.purgeBodyPairs(body.id);
}

bool PhysicsWorld::isValid(BodyHandle body) const {
    if (!body.valid()) {
        return false;
    }
    JPH::BodyLockRead lock(m_impl->locks(), toJolt(body));
    return lock.Succeeded();
}

Transform PhysicsWorld::getTransform(BodyHandle body) const {
    JPH::BodyLockRead lock(m_impl->locks(), toJolt(body));
    if (!lock.Succeeded()) {
        return {};
    }
    const JPH::Body& b = lock.GetBody();
    return {toGlm(b.GetPosition()), toGlm(b.GetRotation())};
}
glm::vec3 PhysicsWorld::getPosition(BodyHandle body) const { return toGlm(m_impl->bodies().GetPosition(toJolt(body))); }
glm::quat PhysicsWorld::getRotation(BodyHandle body) const { return toGlm(m_impl->bodies().GetRotation(toJolt(body))); }
glm::vec3 PhysicsWorld::getCenterOfMassPosition(BodyHandle body) const {
    return toGlm(m_impl->bodies().GetCenterOfMassPosition(toJolt(body)));
}
void PhysicsWorld::setTransform(BodyHandle body, const glm::vec3& p, const glm::quat& r, Activation a) {
    m_impl->bodies().SetPositionAndRotation(toJolt(body), toJoltR(p), toJolt(glm::normalize(r)), toJolt(a));
}
void PhysicsWorld::setPosition(BodyHandle body, const glm::vec3& p, Activation a) {
    m_impl->bodies().SetPosition(toJolt(body), toJoltR(p), toJolt(a));
}
void PhysicsWorld::setRotation(BodyHandle body, const glm::quat& r, Activation a) {
    m_impl->bodies().SetRotation(toJolt(body), toJolt(glm::normalize(r)), toJolt(a));
}
void PhysicsWorld::moveKinematic(BodyHandle body, const glm::vec3& p, const glm::quat& r, f32 dt) {
    m_impl->bodies().MoveKinematic(toJolt(body), toJoltR(p), toJolt(glm::normalize(r)), dt);
}

glm::vec3 PhysicsWorld::getLinearVelocity(BodyHandle body) const {
    return toGlm(m_impl->bodies().GetLinearVelocity(toJolt(body)));
}
glm::vec3 PhysicsWorld::getAngularVelocity(BodyHandle body) const {
    return toGlm(m_impl->bodies().GetAngularVelocity(toJolt(body)));
}
void PhysicsWorld::setLinearVelocity(BodyHandle body, const glm::vec3& v) {
    m_impl->bodies().SetLinearVelocity(toJolt(body), toJolt(v));
}
void PhysicsWorld::setAngularVelocity(BodyHandle body, const glm::vec3& v) {
    m_impl->bodies().SetAngularVelocity(toJolt(body), toJolt(v));
}
glm::vec3 PhysicsWorld::getPointVelocity(BodyHandle body, const glm::vec3& p) const {
    return toGlm(m_impl->bodies().GetPointVelocity(toJolt(body), toJoltR(p)));
}

void PhysicsWorld::addForce(BodyHandle body, const glm::vec3& f) { m_impl->bodies().AddForce(toJolt(body), toJolt(f)); }
void PhysicsWorld::addForceAtPoint(BodyHandle body, const glm::vec3& f, const glm::vec3& p) {
    m_impl->bodies().AddForce(toJolt(body), toJolt(f), toJoltR(p));
}
void PhysicsWorld::addTorque(BodyHandle body, const glm::vec3& t) { m_impl->bodies().AddTorque(toJolt(body), toJolt(t)); }
void PhysicsWorld::addImpulse(BodyHandle body, const glm::vec3& i) { m_impl->bodies().AddImpulse(toJolt(body), toJolt(i)); }
void PhysicsWorld::addImpulseAtPoint(BodyHandle body, const glm::vec3& i, const glm::vec3& p) {
    m_impl->bodies().AddImpulse(toJolt(body), toJolt(i), toJoltR(p));
}
void PhysicsWorld::addAngularImpulse(BodyHandle body, const glm::vec3& i) {
    m_impl->bodies().AddAngularImpulse(toJolt(body), toJolt(i));
}

void PhysicsWorld::activate(BodyHandle body) { m_impl->bodies().ActivateBody(toJolt(body)); }
void PhysicsWorld::deactivate(BodyHandle body) { m_impl->bodies().DeactivateBody(toJolt(body)); }
bool PhysicsWorld::isActive(BodyHandle body) const { return m_impl->bodies().IsActive(toJolt(body)); }

MotionType PhysicsWorld::getMotionType(BodyHandle body) const {
    switch (m_impl->bodies().GetMotionType(toJolt(body))) {
    case JPH::EMotionType::Static: return MotionType::Static;
    case JPH::EMotionType::Kinematic: return MotionType::Kinematic;
    default: return MotionType::Dynamic;
    }
}
void PhysicsWorld::setMotionType(BodyHandle body, MotionType type, Activation a) {
    m_impl->bodies().SetMotionType(toJolt(body), toJolt(type), toJolt(a));
}
ObjectLayer PhysicsWorld::getLayer(BodyHandle body) const {
    return ObjectLayer(m_impl->bodies().GetObjectLayer(toJolt(body)));
}
void PhysicsWorld::setLayer(BodyHandle body, ObjectLayer layer) {
    OX_ASSERT(layer < m_impl->layers.layerCount(), "layer {} not defined", layer);
    m_impl->bodies().SetObjectLayer(toJolt(body), JPH::ObjectLayer(layer));
}
bool PhysicsWorld::isSensor(BodyHandle body) const {
    JPH::BodyLockRead lock(m_impl->locks(), toJolt(body));
    return lock.Succeeded() && lock.GetBody().IsSensor();
}

void PhysicsWorld::setFriction(BodyHandle body, f32 v) { m_impl->bodies().SetFriction(toJolt(body), v); }
f32 PhysicsWorld::getFriction(BodyHandle body) const { return m_impl->bodies().GetFriction(toJolt(body)); }
void PhysicsWorld::setRestitution(BodyHandle body, f32 v) { m_impl->bodies().SetRestitution(toJolt(body), v); }
f32 PhysicsWorld::getRestitution(BodyHandle body) const { return m_impl->bodies().GetRestitution(toJolt(body)); }
void PhysicsWorld::setGravityFactor(BodyHandle body, f32 v) { m_impl->bodies().SetGravityFactor(toJolt(body), v); }
f32 PhysicsWorld::getGravityFactor(BodyHandle body) const { return m_impl->bodies().GetGravityFactor(toJolt(body)); }

void PhysicsWorld::setDamping(BodyHandle body, f32 linear, f32 angular) {
    JPH::BodyLockWrite lock(m_impl->locks(), toJolt(body));
    if (lock.Succeeded() && !lock.GetBody().IsStatic()) {
        auto* mp = lock.GetBody().GetMotionProperties();
        mp->SetLinearDamping(linear);
        mp->SetAngularDamping(angular);
    }
}

f32 PhysicsWorld::getMass(BodyHandle body) const {
    JPH::BodyLockRead lock(m_impl->locks(), toJolt(body));
    if (!lock.Succeeded() || lock.GetBody().IsStatic()) {
        return 0.f;
    }
    f32 inv = lock.GetBody().GetMotionProperties()->GetInverseMassUnchecked();
    return inv > 0.f ? 1.f / inv : 0.f;
}

void PhysicsWorld::setCcd(BodyHandle body, bool enabled) {
    m_impl->bodies().SetMotionQuality(toJolt(body),
                                      enabled ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
}

void PhysicsWorld::setShape(BodyHandle body, const ShapeRef& shape, bool updateMass, Activation a) {
    if (!shape) {
        return;
    }
    m_impl->bodies().SetShape(toJolt(body), static_cast<const JPH::Shape*>(shape.native()), updateMass, toJolt(a));
}
ShapeRef PhysicsWorld::getShape(BodyHandle body) const {
    JPH::RefConst<JPH::Shape> s = m_impl->bodies().GetShape(toJolt(body));
    return ShapeRef::fromNative(s.GetPtr());
}
Aabb PhysicsWorld::getWorldBounds(BodyHandle body) const {
    JPH::BodyLockRead lock(m_impl->locks(), toJolt(body));
    if (!lock.Succeeded()) {
        return {};
    }
    const JPH::AABox& b = lock.GetBody().GetWorldSpaceBounds();
    return {toGlm(b.mMin), toGlm(b.mMax)};
}

void PhysicsWorld::setUserData(BodyHandle body, u64 v) { m_impl->bodies().SetUserData(toJolt(body), v); }
u64 PhysicsWorld::getUserData(BodyHandle body) const { return m_impl->bodies().GetUserData(toJolt(body)); }

// ---- Events ---------------------------------------------------------------------------------------
std::span<const ContactEvent> PhysicsWorld::contactEvents() const { return m_impl->contactEvents; }
std::span<const TriggerEvent> PhysicsWorld::triggerEvents() const { return m_impl->triggerEvents; }
std::span<const ConstraintBrokenEvent> PhysicsWorld::constraintBrokenEvents() const { return m_impl->brokenEvents; }
void PhysicsWorld::setContactCallback(std::function<void(const ContactEvent&)> cb) {
    m_impl->contactCallback = std::move(cb);
}
void PhysicsWorld::setTriggerCallback(std::function<void(const TriggerEvent&)> cb) {
    m_impl->triggerCallback = std::move(cb);
}
void PhysicsWorld::setConstraintBrokenCallback(std::function<void(const ConstraintBrokenEvent&)> cb) {
    m_impl->brokenCallback = std::move(cb);
}

// ---- Snapshot -------------------------------------------------------------------------------------
namespace {
constexpr u32 kSnapshotMagic = 0x5350584fu; // "OXPS"
constexpr u32 kSnapshotVersion = 1;

constexpr u64 kJoltVersionId = [] {
    using JPH::uint64;
    return u64(JPH_VERSION_ID);
}();

template <class T>
void put(JPH::StateRecorderImpl& r, const T& v) {
    r.WriteBytes(&v, sizeof(T));
}
template <class T>
bool get(JPH::StateRecorderImpl& r, T& v) {
    r.ReadBytes(&v, sizeof(T));
    return !r.IsFailed();
}
} // namespace

std::vector<u8> PhysicsWorld::saveState() const {
    const auto& w = *m_impl;
    JPH::StateRecorderImpl rec;
    put(rec, kSnapshotMagic);
    put(rec, kSnapshotVersion);
    put(rec, kJoltVersionId);
    put(rec, w.stepCount);
    w.system->SaveState(rec, JPH::EStateRecorderState::All);

    u32 liveCharacters = 0;
    for (const auto& c : w.characters) {
        liveCharacters += c.character ? 1u : 0u;
    }
    put(rec, liveCharacters);
    for (u32 i = 0; i < w.characters.size(); ++i) {
        if (w.characters[i].character) {
            put(rec, i);
            w.characters[i].character->SaveState(rec);
        }
    }

    put(rec, u32(w.subPairs.size()));
    for (const auto& [k, lastSeen] : w.subPairs) {
        put(rec, k);
        put(rec, lastSeen);
    }
    put(rec, u32(w.pairs.size()));
    for (const auto& [key, p] : w.pairs) {
        put(rec, key);
        put(rec, p.subCount);
        put(rec, u8((p.sensor ? 1 : 0) | (p.body1IsTrigger ? 2 : 0)));
        put(rec, p.userData1);
        put(rec, p.userData2);
        put(rec, p.begunStep);
        put(rec, p.persistStep);
    }

    std::string data = rec.GetData();
    return std::vector<u8>(data.begin(), data.end());
}

bool PhysicsWorld::restoreState(std::span<const u8> data) {
    auto& w = *m_impl;
    JPH::StateRecorderImpl rec;
    rec.WriteBytes(data.data(), data.size());

    u32 magic = 0, version = 0;
    u64 versionId = 0, stepCount = 0;
    if (!get(rec, magic) || !get(rec, version) || !get(rec, versionId) || magic != kSnapshotMagic ||
        version != kSnapshotVersion || versionId != kJoltVersionId) {
        OX_LOG_ERROR("physics", "restoreState: invalid or incompatible snapshot");
        return false;
    }
    get(rec, stepCount);
    if (!w.system->RestoreState(rec)) {
        OX_LOG_ERROR("physics", "restoreState: Jolt state mismatch (different bodies/constraints?)");
        return false;
    }

    u32 liveCharacters = 0;
    get(rec, liveCharacters);
    for (u32 n = 0; n < liveCharacters; ++n) {
        u32 index = 0;
        get(rec, index);
        if (index >= w.characters.size() || !w.characters[index].character) {
            OX_LOG_ERROR("physics", "restoreState: character {} missing", index);
            return false;
        }
        w.characters[index].character->RestoreState(rec);
    }

    std::map<detail::SubPairKey, u64> subPairs;
    std::map<u64, detail::PairState> pairs;
    u32 count = 0;
    get(rec, count);
    for (u32 i = 0; i < count; ++i) {
        detail::SubPairKey k{};
        u64 lastSeen = 0;
        get(rec, k);
        get(rec, lastSeen);
        subPairs.emplace(k, lastSeen);
    }
    get(rec, count);
    for (u32 i = 0; i < count; ++i) {
        u64 key = 0;
        detail::PairState p;
        u8 flags = 0;
        get(rec, key);
        get(rec, p.subCount);
        get(rec, flags);
        get(rec, p.userData1);
        get(rec, p.userData2);
        get(rec, p.begunStep);
        get(rec, p.persistStep);
        p.sensor = flags & 1;
        p.body1IsTrigger = flags & 2;
        pairs.emplace(key, p);
    }
    if (rec.IsFailed()) {
        OX_LOG_ERROR("physics", "restoreState: truncated snapshot");
        return false;
    }
    w.subPairs = std::move(subPairs);
    w.pairs = std::move(pairs);
    w.stepCount = stepCount;
    w.pendingContactEvents.clear();
    w.pendingTriggerEvents.clear();
    return true;
}

} // namespace ox::physics
