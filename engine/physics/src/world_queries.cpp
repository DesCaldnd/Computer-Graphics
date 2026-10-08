#include "world_impl.hpp"

#include <algorithm>

namespace ox::physics {

using detail::toGlm;
using detail::toHandle;
using detail::toJolt;
using detail::toJoltR;

namespace {

struct Filters {
    detail::MaskBroadPhaseFilter bp;
    detail::MaskObjectLayerFilter obj;
    detail::QueryBodyFilter body;
    Filters(const CollisionLayers& layers, const QueryFilter& f)
        : bp(layers.broadPhaseMaskFor(f.layerMask)), obj(f.layerMask), body(f) {}
};

glm::vec3 safeNormalize(const glm::vec3& v) {
    f32 len = glm::length(v);
    return len > 1e-12f ? v / len : glm::vec3(0.f, -1.f, 0.f);
}

RayHit makeRayHit(const JPH::BodyLockInterface& locks, const JPH::RRayCast& ray, const JPH::RayCastResult& r,
                  f32 maxDistance) {
    RayHit hit;
    hit.body = toHandle(r.mBodyID);
    hit.fraction = r.mFraction;
    hit.distance = r.mFraction * maxDistance;
    hit.subShapeId = r.mSubShapeID2.GetValue();
    JPH::RVec3 point = ray.GetPointOnRay(r.mFraction);
    hit.point = toGlm(point);
    JPH::BodyLockRead lock(locks, r.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& b = lock.GetBody();
        hit.normal = toGlm(b.GetWorldSpaceSurfaceNormal(r.mSubShapeID2, point));
        hit.userData = b.GetUserData();
    }
    return hit;
}

ShapeCastHit makeCastHit(const JPH::BodyLockInterface& locks, const JPH::ShapeCastResult& r, f32 distance) {
    ShapeCastHit hit;
    hit.body = toHandle(r.mBodyID2);
    hit.fraction = r.mFraction;
    hit.distance = r.mFraction * distance;
    hit.point = toGlm(r.mContactPointOn2);
    hit.normal = -safeNormalize(toGlm(r.mPenetrationAxis));
    hit.startedPenetrating = r.mFraction <= 0.f && r.mPenetrationDepth > 0.f;
    JPH::BodyLockRead lock(locks, r.mBodyID2);
    if (lock.Succeeded()) {
        hit.userData = lock.GetBody().GetUserData();
    }
    return hit;
}

} // namespace

std::optional<RayHit> PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance,
                                            const QueryFilter& filter) const {
    const auto& w = *m_impl;
    Filters f(w.layers, filter);
    JPH::RRayCast ray(toJoltR(origin), toJolt(safeNormalize(direction) * maxDistance));
    JPH::RayCastResult result;
    if (!w.system->GetNarrowPhaseQuery().CastRay(ray, result, f.bp, f.obj, f.body)) {
        return std::nullopt;
    }
    return makeRayHit(w.locks(), ray, result, maxDistance);
}

std::vector<RayHit> PhysicsWorld::raycastAll(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance,
                                             const QueryFilter& filter) const {
    const auto& w = *m_impl;
    Filters f(w.layers, filter);
    JPH::RRayCast ray(toJoltR(origin), toJolt(safeNormalize(direction) * maxDistance));
    JPH::RayCastSettings settings;
    settings.SetBackFaceMode(JPH::EBackFaceMode::IgnoreBackFaces);
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    w.system->GetNarrowPhaseQuery().CastRay(ray, settings, collector, f.bp, f.obj, f.body);
    collector.Sort();
    std::vector<RayHit> hits;
    hits.reserve(collector.mHits.size());
    for (const auto& r : collector.mHits) {
        hits.push_back(makeRayHit(w.locks(), ray, r, maxDistance));
    }
    return hits;
}

std::optional<ShapeCastHit> PhysicsWorld::shapeCast(const ShapeRef& shape, const glm::vec3& position,
                                                    const glm::quat& rotation, const glm::vec3& direction,
                                                    f32 distance, const QueryFilter& filter) const {
    if (!shape) {
        return std::nullopt;
    }
    const auto& w = *m_impl;
    Filters f(w.layers, filter);
    const auto* s = static_cast<const JPH::Shape*>(shape.native());
    JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        s, JPH::Vec3::sOne(), JPH::RMat44::sRotationTranslation(toJolt(glm::normalize(rotation)), toJoltR(position)),
        toJolt(safeNormalize(direction) * distance));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    w.system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, f.bp, f.obj, f.body);
    if (!collector.HadHit()) {
        return std::nullopt;
    }
    return makeCastHit(w.locks(), collector.mHit, distance);
}

std::optional<ShapeCastHit> PhysicsWorld::sphereCast(const glm::vec3& origin, f32 radius, const glm::vec3& direction,
                                                     f32 distance, const QueryFilter& filter) const {
    JPH::RefConst<JPH::Shape> s = new JPH::SphereShape(radius);
    return shapeCast(ShapeRef::fromNative(s.GetPtr()), origin, glm::quat(1.f, 0.f, 0.f, 0.f), direction, distance,
                     filter);
}

std::optional<ShapeCastHit> PhysicsWorld::boxCast(const glm::vec3& center, const glm::vec3& halfExtents,
                                                  const glm::quat& rotation, const glm::vec3& direction, f32 distance,
                                                  const QueryFilter& filter) const {
    f32 minHalf = std::min({halfExtents.x, halfExtents.y, halfExtents.z});
    JPH::RefConst<JPH::Shape> s =
        new JPH::BoxShape(toJolt(halfExtents), std::min(JPH::cDefaultConvexRadius, minHalf));
    return shapeCast(ShapeRef::fromNative(s.GetPtr()), center, rotation, direction, distance, filter);
}

std::optional<ShapeCastHit> PhysicsWorld::capsuleCast(const glm::vec3& center, f32 halfHeight, f32 radius,
                                                      const glm::quat& rotation, const glm::vec3& direction,
                                                      f32 distance, const QueryFilter& filter) const {
    JPH::RefConst<JPH::Shape> s = new JPH::CapsuleShape(halfHeight, radius);
    return shapeCast(ShapeRef::fromNative(s.GetPtr()), center, rotation, direction, distance, filter);
}

std::vector<BodyHandle> PhysicsWorld::overlapShape(const ShapeRef& shape, const glm::vec3& position,
                                                   const glm::quat& rotation, const QueryFilter& filter) const {
    std::vector<BodyHandle> out;
    if (!shape) {
        return out;
    }
    const auto& w = *m_impl;
    Filters f(w.layers, filter);
    const auto* s = static_cast<const JPH::Shape*>(shape.native());
    JPH::RMat44 com = JPH::RMat44::sRotationTranslation(toJolt(glm::normalize(rotation)), toJoltR(position))
                          .PreTranslated(s->GetCenterOfMass());
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    w.system->GetNarrowPhaseQuery().CollideShape(s, JPH::Vec3::sOne(), com, settings, JPH::RVec3::sZero(), collector,
                                                 f.bp, f.obj, f.body);
    for (const auto& hit : collector.mHits) {
        out.push_back(toHandle(hit.mBodyID2));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<BodyHandle> PhysicsWorld::overlapSphere(const glm::vec3& center, f32 radius,
                                                    const QueryFilter& filter) const {
    JPH::RefConst<JPH::Shape> s = new JPH::SphereShape(radius);
    return overlapShape(ShapeRef::fromNative(s.GetPtr()), center, glm::quat(1.f, 0.f, 0.f, 0.f), filter);
}

std::vector<BodyHandle> PhysicsWorld::overlapBox(const glm::vec3& center, const glm::vec3& halfExtents,
                                                 const glm::quat& rotation, const QueryFilter& filter) const {
    f32 minHalf = std::min({halfExtents.x, halfExtents.y, halfExtents.z});
    JPH::RefConst<JPH::Shape> s =
        new JPH::BoxShape(toJolt(halfExtents), std::min(JPH::cDefaultConvexRadius, minHalf));
    return overlapShape(ShapeRef::fromNative(s.GetPtr()), center, rotation, filter);
}

std::vector<BodyHandle> PhysicsWorld::overlapAabb(const Aabb& box, const QueryFilter& filter) const {
    const auto& w = *m_impl;
    Filters f(w.layers, filter);
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    w.system->GetBroadPhaseQuery().CollideAABox(JPH::AABox(toJolt(box.min), toJolt(box.max)), collector, f.bp, f.obj);
    std::vector<BodyHandle> out;
    for (const JPH::BodyID& id : collector.mHits) {
        if (!f.body.ShouldCollide(id)) {
            continue;
        }
        JPH::BodyLockRead lock(w.locks(), id);
        if (lock.Succeeded() && f.body.ShouldCollideLocked(lock.GetBody())) {
            out.push_back(toHandle(id));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<ClosestPointResult> PhysicsWorld::closestPoint(const glm::vec3& point, f32 maxDistance,
                                                             const QueryFilter& filter) const {
    const auto& w = *m_impl;
    Filters f(w.layers, filter);
    constexpr f32 kProbeRadius = 1e-3f;
    JPH::RefConst<JPH::Shape> probe = new JPH::SphereShape(kProbeRadius);
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = maxDistance;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    w.system->GetNarrowPhaseQuery().CollideShape(probe, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(toJoltR(point)),
                                                 settings, JPH::RVec3::sZero(), collector, f.bp, f.obj, f.body);
    const JPH::CollideShapeResult* best = nullptr;
    for (const auto& hit : collector.mHits) {
        if (!best || hit.mPenetrationDepth > best->mPenetrationDepth) {
            best = &hit;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    ClosestPointResult result;
    result.body = toHandle(best->mBodyID2);
    result.point = toGlm(best->mContactPointOn2);
    result.distance = std::max(0.f, kProbeRadius - best->mPenetrationDepth);
    if (result.distance > maxDistance) {
        return std::nullopt;
    }
    JPH::BodyLockRead lock(w.locks(), best->mBodyID2);
    if (lock.Succeeded()) {
        result.userData = lock.GetBody().GetUserData();
    }
    return result;
}

} // namespace ox::physics
