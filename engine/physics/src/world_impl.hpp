#pragma once

#include "jolt_common.hpp"

#include <oxwald/physics/physics_world.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace ox::physics {

namespace detail {

// --- Layer adapters ----------------------------------------------------------------------------
class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    explicit BroadPhaseLayerInterfaceImpl(const CollisionLayers& layers) : m_layers(layers) {}
    JPH::uint GetNumBroadPhaseLayers() const override { return m_layers.broadPhaseLayerCount(); }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(m_layers.broadPhaseLayer(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        static constexpr const char* kNames[] = {"NonMoving", "Moving", "Sensor", "Debris", "BP4", "BP5", "BP6", "BP7"};
        return kNames[layer.GetValue() & 7];
    }
#endif

private:
    const CollisionLayers& m_layers;
};

class ObjectVsBroadPhaseFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    explicit ObjectVsBroadPhaseFilterImpl(const CollisionLayers& layers) : m_layers(layers) {}
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        return (m_layers.broadPhaseMaskFor(m_layers.collisionMask(layer)) >> bp.GetValue()) & 1u;
    }

private:
    const CollisionLayers& m_layers;
};

class ObjectPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectPairFilterImpl(const CollisionLayers& layers) : m_layers(layers) {}
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return m_layers.collides(a, b); }

private:
    const CollisionLayers& m_layers;
};

// Query filters built from a QueryFilter.
class MaskBroadPhaseFilter final : public JPH::BroadPhaseLayerFilter {
public:
    explicit MaskBroadPhaseFilter(u32 mask) : m_mask(mask) {}
    bool ShouldCollide(JPH::BroadPhaseLayer bp) const override { return (m_mask >> bp.GetValue()) & 1u; }

private:
    u32 m_mask;
};

class MaskObjectLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit MaskObjectLayerFilter(LayerMask mask) : m_mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer < 32 && ((m_mask >> layer) & 1u); }

private:
    LayerMask m_mask;
};

class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(const QueryFilter& f) : m_filter(f) {}
    bool ShouldCollide(const JPH::BodyID& id) const override {
        for (BodyHandle h : m_filter.ignoreBodies) {
            if (h.id == id.GetIndexAndSequenceNumber()) {
                return false;
            }
        }
        return true;
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (body.IsSensor() && !m_filter.includeSensors) {
            return false;
        }
        return !m_filter.predicate || m_filter.predicate(toHandle(body.GetID()), body.GetUserData());
    }

private:
    const QueryFilter& m_filter;
};

// Adapts IPhysicsJobExecutor to Jolt's job system.
class ExecutorJobSystem final : public JPH::JobSystemWithBarrier {
public:
    explicit ExecutorJobSystem(IPhysicsJobExecutor& executor) : m_executor(executor) {
        Init(JPH::cMaxPhysicsBarriers);
    }
    // A worker can still be inside run() — between the job signalling its barrier (which lets Update() return) and
    // Release() calling back into FreeJob() — when the world is destroyed. Wait for those calls to leave.
    ~ExecutorJobSystem() override {
        while (m_running.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    }
    int GetMaxConcurrency() const override { return int(std::max(1u, m_executor.maxConcurrency())); }
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn, JPH::uint32 deps) override {
        Job* job = new Job(name, color, this, fn, deps);
        JobHandle handle(job);
        if (deps == 0) {
            QueueJob(job);
        }
        return handle;
    }

protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        m_running.fetch_add(1, std::memory_order_acq_rel);
        m_executor.submit(&run, job);
    }
    void QueueJobs(Job** jobs, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) {
            QueueJob(jobs[i]);
        }
    }
    void FreeJob(Job* job) override { delete job; }

private:
    static void run(void* ctx) {
        Job* job = static_cast<Job*>(ctx);
        auto* system = static_cast<ExecutorJobSystem*>(job->GetJobSystem());
        job->Execute();
        job->Release();
        system->m_running.fetch_sub(1, std::memory_order_acq_rel);
    }
    IPhysicsJobExecutor& m_executor;
    std::atomic<u32> m_running{0}; // queued or executing run() calls
};

// --- Contact buffering ---------------------------------------------------------------------------
enum class RawKind : u8 { Added = 0, Persisted = 1, Removed = 2 };

struct SubPairKey {
    u32 body1, sub1, body2, sub2; // body1 < body2
    auto operator<=>(const SubPairKey&) const = default;
};

struct RawContact {
    RawKind kind;
    SubPairKey key;
    bool sensor = false;
    bool body1IsTrigger = false;
    u64 userData1 = 0, userData2 = 0;
    glm::vec3 normal{0.f};
    f32 penetration = 0.f;
    f32 impulse = 0.f;
    u32 pointCount = 0;
    std::array<glm::vec3, kMaxContactEventPoints> points{};
};

struct PairState {
    u32 subCount = 0;
    bool sensor = false;
    bool body1IsTrigger = false;
    u64 userData1 = 0, userData2 = 0;
    u64 begunStep = 0;
    u64 persistStep = ~0ull;
};

enum BodyFlags : u8 { kFlagReportContacts = 1 << 0 };

class ContactListenerImpl final : public JPH::ContactListener {
public:
    const std::vector<u8>* bodyFlags = nullptr;
    bool reportPersist = true;

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                        JPH::ContactSettings& s) override {
        record(RawKind::Added, b1, b2, m, s);
    }
    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                            JPH::ContactSettings& s) override {
        record(RawKind::Persisted, b1, b2, m, s);
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        RawContact r;
        r.kind = RawKind::Removed;
        u32 b1 = pair.GetBody1ID().GetIndexAndSequenceNumber(), b2 = pair.GetBody2ID().GetIndexAndSequenceNumber();
        u32 s1 = pair.GetSubShapeID1().GetValue(), s2 = pair.GetSubShapeID2().GetValue();
        r.key = b1 < b2 ? SubPairKey{b1, s1, b2, s2} : SubPairKey{b2, s2, b1, s1};
        std::lock_guard lock(m_mutex);
        m_raw.push_back(r);
    }

    std::vector<RawContact> take() {
        std::lock_guard lock(m_mutex);
        std::vector<RawContact> out;
        out.swap(m_raw);
        return out;
    }

private:
    bool reports(const JPH::Body& b) const {
        u32 idx = b.GetID().GetIndex();
        return bodyFlags && idx < bodyFlags->size() && ((*bodyFlags)[idx] & kFlagReportContacts);
    }

    void record(RawKind kind, const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                JPH::ContactSettings& s) {
        bool sensor = b1.IsSensor() || b2.IsSensor();
        if (!sensor && !reports(b1) && !reports(b2)) {
            return;
        }
        RawContact r;
        r.kind = kind;
        r.sensor = sensor;
        u32 id1 = b1.GetID().GetIndexAndSequenceNumber(), id2 = b2.GetID().GetIndexAndSequenceNumber();
        bool swap = id1 > id2;
        const JPH::Body& a = swap ? b2 : b1;
        const JPH::Body& b = swap ? b1 : b2;
        r.key = swap ? SubPairKey{id2, m.mSubShapeID2.GetValue(), id1, m.mSubShapeID1.GetValue()}
                     : SubPairKey{id1, m.mSubShapeID1.GetValue(), id2, m.mSubShapeID2.GetValue()};
        r.body1IsTrigger = a.IsSensor();
        r.userData1 = a.GetUserData();
        r.userData2 = b.GetUserData();
        // Persisted events are always recorded (they keep pair tracking alive) but only carry data when reported.
        if (!sensor && (kind == RawKind::Added || reportPersist)) {
            JPH::Vec3 n = m.mWorldSpaceNormal; // body1 → body2 in Jolt's order
            r.normal = toGlm(swap ? -n : n);
            r.penetration = m.mPenetrationDepth;
            r.pointCount = std::min<u32>(kMaxContactEventPoints, u32(m.mRelativeContactPointsOn1.size()));
            for (u32 i = 0; i < r.pointCount; ++i) {
                r.points[i] = toGlm(swap ? m.GetWorldSpaceContactPointOn1(i) : m.GetWorldSpaceContactPointOn2(i));
            }
            if (kind == RawKind::Added) {
                JPH::CollisionEstimationResult est;
                JPH::EstimateCollisionResponse(b1, b2, m, est, s.mCombinedFriction, s.mCombinedRestitution);
                for (const auto& imp : est.mImpulses) {
                    r.impulse += imp.mContactImpulse;
                }
            }
        }
        std::lock_guard lock(m_mutex);
        m_raw.push_back(r);
    }

    std::mutex m_mutex;
    std::vector<RawContact> m_raw;
};

} // namespace detail

struct PhysicsWorld::Impl {
    explicit Impl(const PhysicsWorldDesc& desc);
    ~Impl();

    // Order matters: the layer adapters reference `layers`, the system references the adapters.
    CollisionLayers layers;
    detail::BroadPhaseLayerInterfaceImpl bpInterface{layers};
    detail::ObjectVsBroadPhaseFilterImpl objVsBp{layers};
    detail::ObjectPairFilterImpl objPair{layers};
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystem> jobSystem;
    std::unique_ptr<JPH::PhysicsSystem> system;
    detail::ContactListenerImpl contactListener;
    ShapeCache shapeCache;
    std::vector<u8> bodyFlags;
    bool reportPersist = true;

    // Events of the last step + events generated between steps (destroyBody) for the next one.
    std::vector<ContactEvent> contactEvents, pendingContactEvents;
    std::vector<TriggerEvent> triggerEvents, pendingTriggerEvents;
    std::vector<ConstraintBrokenEvent> brokenEvents;
    std::function<void(const ContactEvent&)> contactCallback;
    std::function<void(const TriggerEvent&)> triggerCallback;
    std::function<void(const ConstraintBrokenEvent&)> brokenCallback;

    // Sorted containers → deterministic iteration (Stay events, snapshots).
    std::map<detail::SubPairKey, u64> subPairs; // → step in which the sub-shape contact was last reported
    std::map<u64, detail::PairState> pairs;

    struct CharacterSlot {
        JPH::Ref<JPH::CharacterVirtual> character;
        u32 generation = 0;
        f32 maxStepHeight = 0.f;
        f32 stickToFloorDistance = 0.f;
        ObjectLayer layer = layers::Character;
    };
    std::vector<CharacterSlot> characters;
    std::vector<u32> freeCharacters;
    JPH::CharacterVsCharacterCollisionSimple charVsChar;

    struct ConstraintSlot {
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        u32 generation = 0;
        ConstraintType type = ConstraintType::Fixed;
        f32 breakForce = 0.f;
        f32 breakTorque = 0.f;
        BodyHandle a, b;
    };
    std::vector<ConstraintSlot> constraints;
    std::vector<u32> freeConstraints;

    u64 stepCount = 0;
    f32 lastSubStepDt = 0.f;

    JPH::BodyInterface& bodies() { return system->GetBodyInterface(); }
    const JPH::BodyInterface& bodies() const { return system->GetBodyInterface(); }
    const JPH::BodyLockInterface& locks() const { return system->GetBodyLockInterface(); }

    void processContacts();
    void purgeBodyPairs(u32 bodyId);
    bool removeSubPair(std::map<detail::SubPairKey, u64>::iterator it);
    void checkBreakableConstraints();
    void dispatchEvents();
    void setBodyFlags(const JPH::BodyID& id, u8 flags);

    CharacterSlot* character(CharacterHandle h);
    const CharacterSlot* character(CharacterHandle h) const;
    ConstraintSlot* constraint(ConstraintHandle h);
    const ConstraintSlot* constraint(ConstraintHandle h) const;
};

} // namespace ox::physics
