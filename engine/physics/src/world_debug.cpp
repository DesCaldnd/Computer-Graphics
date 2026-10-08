// Collider wireframes drawn from shape data. The vcpkg Jolt build has no JPH_DEBUG_RENDERER, so
// JPH::DebugRenderer is unavailable; this covers the same needs without it.
#include "world_impl.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#include <cmath>
#include <format>

namespace ox::physics {

using detail::toGlm;

namespace {

constexpr int kCircleSegments = 24;
constexpr f32 kTwoPi = 6.28318530718f;

struct Drawer {
    PhysicsDebugSink& sink;
    const DebugDrawOptions& opt;

    void line(const glm::mat4& m, glm::vec3 a, glm::vec3 b, const Color& c) const {
        sink.line(glm::vec3(m * glm::vec4(a, 1.f)), glm::vec3(m * glm::vec4(b, 1.f)), c);
    }

    // Arc in the plane spanned by u,v (unit axes) around center.
    void arc(const glm::mat4& m, glm::vec3 center, glm::vec3 u, glm::vec3 v, f32 r, f32 from, f32 to, int segs,
             const Color& c) const {
        glm::vec3 prev = center + r * (std::cos(from) * u + std::sin(from) * v);
        for (int i = 1; i <= segs; ++i) {
            f32 t = from + (to - from) * f32(i) / f32(segs);
            glm::vec3 p = center + r * (std::cos(t) * u + std::sin(t) * v);
            line(m, prev, p, c);
            prev = p;
        }
    }
    void circle(const glm::mat4& m, glm::vec3 center, glm::vec3 u, glm::vec3 v, f32 r, const Color& c) const {
        arc(m, center, u, v, r, 0.f, kTwoPi, kCircleSegments, c);
    }

    void box(const glm::mat4& m, glm::vec3 lo, glm::vec3 hi, const Color& c) const {
        glm::vec3 p[8];
        for (int i = 0; i < 8; ++i) {
            p[i] = {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
        }
        static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                              {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& e : kEdges) {
            line(m, p[e[0]], p[e[1]], c);
        }
    }

    void capsule(const glm::mat4& m, f32 halfHeight, f32 r, const Color& c) const {
        const glm::vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
        glm::vec3 top{0, halfHeight, 0}, bottom{0, -halfHeight, 0};
        circle(m, top, X, Z, r, c);
        circle(m, bottom, X, Z, r, c);
        for (glm::vec3 d : {X, -X, Z, -Z}) {
            line(m, top + d * r, bottom + d * r, c);
        }
        const f32 pi = kTwoPi * 0.5f;
        arc(m, top, X, Y, r, 0.f, pi, kCircleSegments / 2, c);
        arc(m, top, Z, Y, r, 0.f, pi, kCircleSegments / 2, c);
        arc(m, bottom, X, Y, r, pi, 2.f * pi, kCircleSegments / 2, c);
        arc(m, bottom, Z, Y, r, pi, 2.f * pi, kCircleSegments / 2, c);
    }

    // Leaf shape in its center-of-mass space, `m` maps COM space (incl. scale) to world.
    void leaf(const JPH::TransformedShape& ts, const Color& c) const {
        const JPH::Shape* shape = ts.mShape;
        glm::mat4 m = glm::translate(glm::mat4(1.f), toGlm(ts.mShapePositionCOM)) * glm::mat4_cast(toGlm(ts.mShapeRotation)) *
                      glm::scale(glm::mat4(1.f), toGlm(ts.GetShapeScale()));
        const glm::vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
        switch (shape->GetSubType()) {
        case JPH::EShapeSubType::Box: {
            glm::vec3 h = toGlm(static_cast<const JPH::BoxShape*>(shape)->GetHalfExtent());
            box(m, -h, h, c);
            return;
        }
        case JPH::EShapeSubType::Sphere: {
            f32 r = static_cast<const JPH::SphereShape*>(shape)->GetRadius();
            circle(m, {}, X, Y, r, c);
            circle(m, {}, Y, Z, r, c);
            circle(m, {}, X, Z, r, c);
            return;
        }
        case JPH::EShapeSubType::Capsule: {
            const auto* cap = static_cast<const JPH::CapsuleShape*>(shape);
            capsule(m, cap->GetHalfHeightOfCylinder(), cap->GetRadius(), c);
            return;
        }
        case JPH::EShapeSubType::Cylinder: {
            const auto* cyl = static_cast<const JPH::CylinderShape*>(shape);
            f32 h = cyl->GetHalfHeight(), r = cyl->GetRadius();
            circle(m, {0, h, 0}, X, Z, r, c);
            circle(m, {0, -h, 0}, X, Z, r, c);
            for (glm::vec3 d : {X, -X, Z, -Z}) {
                line(m, glm::vec3(0, h, 0) + d * r, glm::vec3(0, -h, 0) + d * r, c);
            }
            return;
        }
        case JPH::EShapeSubType::ConvexHull: {
            const auto* hull = static_cast<const JPH::ConvexHullShape*>(shape);
            for (JPH::uint f = 0; f < hull->GetNumFaces(); ++f) {
                JPH::uint idx[256];
                JPH::uint n = hull->GetFaceVertices(f, 256, idx);
                for (JPH::uint i = 0; i < n; ++i) {
                    line(m, toGlm(hull->GetPoint(idx[i])), toGlm(hull->GetPoint(idx[(i + 1) % n])), c);
                }
            }
            return;
        }
        default: triangles(ts, c); return;
        }
    }

    // Generic path (meshes, height fields, other convex types): Jolt's triangle iterator, world space.
    void triangles(const JPH::TransformedShape& ts, const Color& c) const {
        JPH::Shape::GetTrianglesContext ctx;
        ts.GetTrianglesStart(ctx, JPH::AABox::sBiggest(), JPH::RVec3::sZero());
        constexpr int kBatch = 256;
        JPH::Float3 verts[kBatch * 3];
        u32 total = 0;
        for (;;) {
            int n = ts.GetTrianglesNext(ctx, kBatch, verts);
            if (n == 0) {
                break;
            }
            for (int i = 0; i < n && total < opt.maxTrianglesPerBody; ++i, ++total) {
                glm::vec3 a{verts[i * 3].x, verts[i * 3].y, verts[i * 3].z};
                glm::vec3 b{verts[i * 3 + 1].x, verts[i * 3 + 1].y, verts[i * 3 + 1].z};
                glm::vec3 d{verts[i * 3 + 2].x, verts[i * 3 + 2].y, verts[i * 3 + 2].z};
                if (opt.filledTriangles) {
                    sink.triangle(a, b, d, c);
                } else {
                    sink.line(a, b, c);
                    sink.line(b, d, c);
                    sink.line(d, a, c);
                }
            }
            if (total >= opt.maxTrianglesPerBody) {
                break;
            }
        }
    }

    void shape(const JPH::TransformedShape& root, const Color& c) const {
        JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
        root.CollectTransformedShapes(JPH::AABox::sBiggest(), leaves);
        for (const auto& ts : leaves.mHits) {
            leaf(ts, c);
        }
    }
};

} // namespace

void PhysicsWorld::debugDraw(PhysicsDebugSink& sink, const DebugDrawOptions& opt) const {
    const auto& w = *m_impl;
    Drawer d{sink, opt};

    JPH::BodyIDVector ids;
    w.system->GetBodies(ids);
    for (const JPH::BodyID& id : ids) {
        JPH::BodyLockRead lock(w.locks(), id);
        if (!lock.Succeeded()) {
            continue;
        }
        const JPH::Body& body = lock.GetBody();
        Color color = body.IsSensor()     ? opt.sensorColor
                      : body.IsStatic()   ? opt.staticColor
                      : body.IsKinematic() ? opt.kinematicColor
                                           : opt.dynamicColor;
        if (opt.colorSleeping && !body.IsStatic() && !body.IsActive()) {
            color = opt.sleepingColor;
        }
        if (opt.shapes) {
            d.shape(body.GetTransformedShape(), color);
        }
        if (opt.aabbs) {
            const JPH::AABox& b = body.GetWorldSpaceBounds();
            d.box(glm::mat4(1.f), toGlm(b.mMin), toGlm(b.mMax), opt.aabbColor);
        }
        glm::vec3 com = toGlm(body.GetCenterOfMassPosition());
        if (opt.centerOfMass && !body.IsStatic()) {
            glm::mat4 m = glm::translate(glm::mat4(1.f), com) * glm::mat4_cast(toGlm(body.GetRotation()));
            d.line(m, {}, {0.2f, 0, 0}, Color{1, 0, 0, 1});
            d.line(m, {}, {0, 0.2f, 0}, Color{0, 1, 0, 1});
            d.line(m, {}, {0, 0, 0.2f}, Color{0, 0, 1, 1});
        }
        if (opt.velocities && !body.IsStatic()) {
            sink.line(com, com + toGlm(body.GetLinearVelocity()) * opt.velocityScale, opt.velocityColor);
        }
        if (opt.labels) {
            sink.text(com, std::format("#{} ud={}", id.GetIndex(), body.GetUserData()), color);
        }
    }

    if (opt.contacts) {
        for (const auto& e : w.contactEvents) {
            for (u32 i = 0; i < e.pointCount; ++i) {
                sink.line(e.points[i], e.points[i] + e.normal * opt.contactNormalLength, opt.contactColor);
            }
        }
    }

    if (opt.constraints) {
        for (const auto& slot : w.constraints) {
            if (!slot.constraint) {
                continue;
            }
            const auto* c = slot.constraint.GetPtr();
            glm::vec3 a = toGlm(c->GetBody1()->GetCenterOfMassTransform() * c->GetConstraintToBody1Matrix().GetTranslation());
            glm::vec3 b = toGlm(c->GetBody2()->GetCenterOfMassTransform() * c->GetConstraintToBody2Matrix().GetTranslation());
            Color col = c->GetEnabled() ? opt.constraintColor : opt.sleepingColor;
            if (!c->GetBody1()->GetID().IsInvalid()) { // not Body::sFixedToWorld
                sink.line(toGlm(c->GetBody1()->GetCenterOfMassPosition()), a, col);
            }
            sink.line(toGlm(c->GetBody2()->GetCenterOfMassPosition()), b, col);
            d.box(glm::mat4(1.f), a - glm::vec3(0.03f), a + glm::vec3(0.03f), col);
        }
    }

    if (opt.characters) {
        for (const auto& slot : w.characters) {
            if (!slot.character || !slot.character->GetInnerBodyID().IsInvalid()) {
                continue; // characters with an inner body were drawn as kinematic bodies
            }
            d.shape(slot.character->GetTransformedShape(), opt.characterColor);
        }
        for (const auto& slot : w.characters) {
            if (slot.character && slot.character->IsSupported()) {
                glm::vec3 p = toGlm(slot.character->GetGroundPosition());
                sink.line(p, p + toGlm(slot.character->GetGroundNormal()) * 0.3f, opt.characterColor);
            }
        }
    }
}

} // namespace ox::physics
