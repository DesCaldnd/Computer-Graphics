#include <oxwald/core/math.hpp>

#include <gtest/gtest.h>

#include <set>

using namespace ox;

namespace {

::testing::AssertionResult vecNear(glm::vec3 a, glm::vec3 b, f32 eps = 1e-4f) {
    if (nearlyEqual(a, b, eps)) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "(" << a.x << "," << a.y << "," << a.z << ") vs (" << b.x << "," << b.y
                                         << "," << b.z << ")";
}

Transform sampleTransform() {
    Transform t;
    t.position = {1.0f, -2.0f, 3.5f};
    t.rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3{1.0f, 2.0f, -0.5f}));
    t.scale = {2.0f, 0.5f, 3.0f};
    return t;
}

} // namespace

TEST(Math, Helpers) {
    EXPECT_TRUE(nearlyEqual(0.1f + 0.2f, 0.3f));
    EXPECT_FALSE(nearlyEqual(1.0f, 1.01f));
    EXPECT_TRUE(nearlyEqual(1e6f, 1e6f + 1.0f)); // relative tolerance
    EXPECT_FLOAT_EQ(saturate(-1.0f), 0.0f);
    EXPECT_FLOAT_EQ(saturate(2.0f), 1.0f);
    EXPECT_FLOAT_EQ(saturate(0.25f), 0.25f);
    EXPECT_FLOAT_EQ(remap(5.0f, 0.0f, 10.0f, 100.0f, 200.0f), 150.0f);
    EXPECT_FLOAT_EQ(remap(1.0f, 1.0f, 1.0f, 3.0f, 4.0f), 3.0f);
    EXPECT_FLOAT_EQ(remapClamped(20.0f, 0.0f, 10.0f, 0.0f, 1.0f), 1.0f);
    EXPECT_NEAR(toDegrees(kPi), 180.0f, 1e-4f);
    EXPECT_TRUE(nearlyEqual(glm::quat{1, 0, 0, 0}, glm::quat{-1, 0, 0, 0}));
}

TEST(Math, TransformMatrixRoundTrip) {
    const Transform t = sampleTransform();
    const glm::mat4 m = t.toMatrix();
    const glm::mat4 expected = glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation) *
                               glm::scale(glm::mat4(1.0f), t.scale);
    EXPECT_TRUE(nearlyEqual(m, expected, 1e-5f));

    const Transform back = Transform::fromMatrix(m);
    EXPECT_TRUE(nearlyEqual(back, t, 1e-4f));

    const glm::vec3 p{0.3f, -1.0f, 2.0f};
    EXPECT_TRUE(vecNear(t.transformPoint(p), glm::vec3(m * glm::vec4(p, 1.0f))));
    EXPECT_TRUE(vecNear(t.transformVector(p), glm::vec3(m * glm::vec4(p, 0.0f))));
}

TEST(Math, TransformComposeMatchesMatrixProduct) {
    Transform parent;
    parent.position = {5, 0, -1};
    parent.rotation = glm::angleAxis(1.1f, glm::vec3{0, 1, 0});
    parent.scale = glm::vec3{2.0f};
    const Transform child = sampleTransform();

    const Transform world = parent * child;
    EXPECT_TRUE(nearlyEqual(world.toMatrix(), parent.toMatrix() * child.toMatrix(), 1e-4f));
    EXPECT_EQ(compose(parent, child), world);
    EXPECT_TRUE(nearlyEqual(Transform{} * child, child));
}

TEST(Math, TransformInverse) {
    Transform t;
    t.position = {1, 2, 3};
    t.rotation = glm::angleAxis(0.4f, glm::normalize(glm::vec3{1, 1, 0}));
    t.scale = glm::vec3{1.5f};
    const Transform inv = t.inverse();
    EXPECT_TRUE(nearlyEqual(t * inv, Transform{}, 1e-4f));
    EXPECT_TRUE(nearlyEqual(inv.toMatrix(), glm::inverse(t.toMatrix()), 1e-4f));
    const glm::vec3 p{-3, 0.5f, 7};
    EXPECT_TRUE(vecNear(inv.transformPoint(t.transformPoint(p)), p));
}

TEST(Math, TransformAxesAndLerp) {
    Transform t;
    EXPECT_TRUE(vecNear(t.forward(), {0, 0, -1}));
    EXPECT_TRUE(vecNear(t.right(), {1, 0, 0}));
    EXPECT_TRUE(vecNear(t.up(), {0, 1, 0}));
    t.rotation = glm::angleAxis(kHalfPi, glm::vec3{0, 1, 0}); // yaw left 90 degrees
    EXPECT_TRUE(vecNear(t.forward(), {-1, 0, 0}));
    EXPECT_TRUE(vecNear(t.right(), {0, 0, -1}));

    Transform a;
    Transform b;
    b.position = {10, 0, 0};
    b.rotation = glm::angleAxis(kHalfPi, glm::vec3{0, 0, 1});
    b.scale = glm::vec3{3.0f};
    const Transform mid = Transform::lerp(a, b, 0.5f);
    EXPECT_TRUE(vecNear(mid.position, {5, 0, 0}));
    EXPECT_TRUE(vecNear(mid.scale, glm::vec3{2.0f}));
    EXPECT_TRUE(nearlyEqual(mid.rotation, glm::angleAxis(kHalfPi * 0.5f, glm::vec3{0, 0, 1}), 1e-4f));
    // Shortest path even when the quaternions are in opposite hemispheres.
    Transform bNeg = b;
    bNeg.rotation = -b.rotation;
    EXPECT_TRUE(nearlyEqual(Transform::lerp(a, bNeg, 0.5f).rotation, mid.rotation, 1e-4f));
}

TEST(Math, DecomposeNegativeDeterminant) {
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3{1, 2, 3}) *
                        glm::mat4_cast(glm::angleAxis(0.5f, glm::vec3{0, 0, 1})) *
                        glm::scale(glm::mat4(1.0f), glm::vec3{-2, 3, 4});
    glm::vec3 t, s;
    glm::quat r;
    ASSERT_TRUE(decompose(m, t, r, s));
    EXPECT_TRUE(vecNear(t, {1, 2, 3}));
    EXPECT_LT(s.x * s.y * s.z, 0.0f);
    Transform rebuilt{t, r, s};
    EXPECT_TRUE(nearlyEqual(rebuilt.toMatrix(), m, 1e-4f));

    glm::mat4 degenerate = glm::scale(glm::mat4(1.0f), glm::vec3{0, 1, 1});
    EXPECT_FALSE(decompose(degenerate, t, r, s));
}

TEST(Math, LookAndFromToRotation) {
    const glm::vec3 dirs[] = {{1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {1, 1, 1}, {0, 1, 0}, {0, -1, 0}, {-3, 0.2f, 5}};
    for (glm::vec3 d : dirs) {
        const glm::quat q = lookRotation(d);
        EXPECT_TRUE(vecNear(q * glm::vec3{0, 0, -1}, glm::normalize(d))) << d.x << "," << d.y << "," << d.z;
        EXPECT_NEAR(glm::length(q), 1.0f, 1e-5f);
    }
    // Up is respected when not parallel to forward.
    const glm::quat q = lookRotation({1, 0, 0}, {0, 1, 0});
    EXPECT_TRUE(vecNear(q * glm::vec3{0, 1, 0}, {0, 1, 0}));

    // Matches glm::lookAt's camera orientation.
    const glm::mat4 view = glm::lookAtRH(glm::vec3{0}, glm::vec3{1, 2, -3}, glm::vec3{0, 1, 0});
    const glm::quat fromView = glm::quat_cast(glm::transpose(glm::mat3(view)));
    EXPECT_TRUE(nearlyEqual(lookRotation({1, 2, -3}), fromView, 1e-4f));

    for (glm::vec3 from : dirs) {
        for (glm::vec3 to : dirs) {
            const glm::quat r = fromToRotation(from, to);
            EXPECT_TRUE(vecNear(r * glm::normalize(from), glm::normalize(to)));
        }
    }
}

TEST(Math, ReversedZProjections) {
    const f32 n = 0.1f, f = 100.0f;
    const glm::mat4 p = perspectiveReversedZ(toRadians(60.0f), 16.0f / 9.0f, n, f);
    const auto depth = [&](const glm::mat4& m, f32 z) {
        const glm::vec4 c = m * glm::vec4{0, 0, z, 1};
        return c.z / c.w;
    };
    EXPECT_NEAR(depth(p, -n), 1.0f, 1e-5f);
    EXPECT_NEAR(depth(p, -f), 0.0f, 1e-5f);
    EXPECT_GT(depth(p, -1.0f), depth(p, -10.0f));

    const glm::mat4 inf = perspectiveInfiniteReversedZ(toRadians(60.0f), 1.0f, n);
    EXPECT_NEAR(depth(inf, -n), 1.0f, 1e-5f);
    EXPECT_NEAR(depth(inf, -1e7f), 0.0f, 1e-5f);
    EXPECT_GT(depth(inf, -10.0f), 0.0f);

    const glm::mat4 o = orthoReversedZ(-1, 1, -1, 1, n, f);
    EXPECT_NEAR(depth(o, -n), 1.0f, 1e-5f);
    EXPECT_NEAR(depth(o, -f), 0.0f, 1e-5f);
}

TEST(Math, AABBBasics) {
    AABB box;
    EXPECT_FALSE(box.valid());
    EXPECT_FLOAT_EQ(box.surfaceArea(), 0.0f);
    box.expand(glm::vec3{1, 2, 3});
    EXPECT_TRUE(box.valid());
    box.expand(glm::vec3{-1, 0, 0});
    EXPECT_TRUE(vecNear(box.center(), {0, 1, 1.5f}));
    EXPECT_TRUE(vecNear(box.size(), {2, 2, 3}));
    EXPECT_TRUE(vecNear(box.extents(), {1, 1, 1.5f}));
    EXPECT_FLOAT_EQ(box.surfaceArea(), 2.0f * (4 + 6 + 6));
    EXPECT_TRUE(box.contains(glm::vec3{0, 1, 1}));
    EXPECT_FALSE(box.contains(glm::vec3{0, 3, 1}));

    AABB other = AABB::fromCenterExtents({5, 5, 5}, glm::vec3{1});
    EXPECT_FALSE(box.intersects(other));
    other = AABB::fromCenterExtents({1.5f, 1, 1}, glm::vec3{0.5f});
    EXPECT_TRUE(box.intersects(other));
    AABB merged = box;
    merged.expand(AABB{});
    EXPECT_EQ(merged, box);
    merged.expand(AABB::fromCenterExtents({10, 0, 0}, glm::vec3{1}));
    EXPECT_FLOAT_EQ(merged.max.x, 11.0f);
}

TEST(Math, AABBTransformed) {
    const AABB box = AABB::fromCenterExtents({1, 0, 0}, {1, 2, 3});
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3{0, 10, 0}) *
                        glm::mat4_cast(glm::angleAxis(kHalfPi, glm::vec3{0, 1, 0}));
    const AABB t = box.transformed(m);
    // Rotating 90 degrees about Y maps x->-z, z->x.
    EXPECT_TRUE(vecNear(t.center(), {0, 10, -1}));
    EXPECT_TRUE(vecNear(t.extents(), {3, 2, 1}));
    // Every transformed corner is inside the result.
    for (u32 i = 0; i < 8; ++i) {
        const glm::vec3 c{(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                          (i & 4) ? box.max.z : box.min.z};
        const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1));
        EXPECT_TRUE(AABB::fromMinMax(t.min - 1e-4f, t.max + 1e-4f).contains(w));
    }
    EXPECT_FALSE(AABB{}.transformed(m).valid());
}

TEST(Math, PlaneSphereObb) {
    const Plane p = Plane::fromPointNormal({0, 2, 0}, {0, 5, 0});
    EXPECT_FLOAT_EQ(p.signedDistance({0, 5, 0}), 3.0f);
    EXPECT_FLOAT_EQ(p.signedDistance({3, 0, 1}), -2.0f);
    const Plane q = Plane::fromPoints({0, 0, 0}, {1, 0, 0}, {0, 1, 0}); // CCW seen from +Z
    EXPECT_TRUE(vecNear(q.normal, {0, 0, 1}));
    const Plane scaled{glm::vec3{0, 2, 0}, -4};
    EXPECT_FLOAT_EQ(scaled.normalized().d, -2.0f);
    const Plane degenerate = Plane{glm::vec3{0}, 0}.normalized();
    EXPECT_FLOAT_EQ(degenerate.signedDistance({1, 2, 3}), 1.0f);

    const Sphere s{{0, 0, 0}, 1.0f};
    EXPECT_TRUE(s.contains({0.5f, 0.5f, 0}));
    EXPECT_TRUE(s.intersects(Sphere{{1.5f, 0, 0}, 0.6f}));
    EXPECT_FALSE(s.intersects(Sphere{{3, 0, 0}, 0.6f}));
    EXPECT_TRUE(s.intersects(AABB::fromCenterExtents({1.5f, 0, 0}, glm::vec3{0.6f})));
    EXPECT_FALSE(s.intersects(AABB::fromCenterExtents({1.5f, 1.5f, 0}, glm::vec3{0.6f})));

    OBB obb{{0, 0, 0}, {2, 1, 1}, glm::angleAxis(kHalfPi, glm::vec3{0, 0, 1})};
    EXPECT_TRUE(obb.contains({0, 1.9f, 0}));
    EXPECT_FALSE(obb.contains({1.9f, 0, 0}));
    EXPECT_TRUE(vecNear(obb.bounds().extents(), {1, 2, 1}));
}

TEST(Math, FrustumStandardAndReversed) {
    const glm::mat4 view = glm::lookAtRH(glm::vec3{0, 0, 0}, glm::vec3{0, 0, -1}, glm::vec3{0, 1, 0});
    const f32 fov = toRadians(90.0f);
    const glm::mat4 standard = glm::perspectiveRH_ZO(fov, 1.0f, 0.1f, 100.0f) * view;
    const glm::mat4 reversed = perspectiveReversedZ(fov, 1.0f, 0.1f, 100.0f) * view;
    const glm::mat4 infinite = perspectiveInfiniteReversedZ(fov, 1.0f, 0.1f) * view;

    struct Case {
        glm::mat4 vp;
        bool rev;
        bool infiniteFar;
    };
    for (const Case& c : {Case{standard, false, false}, Case{reversed, true, false}, Case{infinite, true, true}}) {
        const Frustum f = Frustum::fromViewProj(c.vp, c.rev);
        EXPECT_TRUE(f.contains({0, 0, -10}));
        EXPECT_FALSE(f.contains({0, 0, 10}));        // behind
        EXPECT_FALSE(f.contains({0, 0, -0.05f}));    // before near
        EXPECT_FALSE(f.contains({20, 0, -10}));      // right of a 90 degree frustum
        EXPECT_TRUE(f.contains({9, 9, -10}));        // inside the corner
        EXPECT_EQ(f.contains({0, 0, -200}), c.infiniteFar);
        EXPECT_TRUE(f.intersects(AABB::fromCenterExtents({0, 0, -50}, glm::vec3{1})));
        EXPECT_TRUE(f.intersects(AABB::fromCenterExtents({10.5f, 0, -10}, glm::vec3{1}))); // straddles right plane
        EXPECT_FALSE(f.intersects(AABB::fromCenterExtents({0, 0, 5}, glm::vec3{1})));
        EXPECT_EQ(f.intersects(AABB::fromCenterExtents({0, 0, -300}, glm::vec3{1})), c.infiniteFar);
        EXPECT_TRUE(f.intersects(Sphere{{11, 0, -10}, 1.5f}));
        EXPECT_FALSE(f.intersects(Sphere{{15, 0, -10}, 1.0f}));
        // Normals are normalised and point inward (the frustum's center is inside every plane).
        for (const Plane& p : f.planes) {
            const f32 len = glm::length(p.normal);
            EXPECT_TRUE(nearlyEqual(len, 1.0f, 1e-4f) || (c.infiniteFar && len == 0.0f));
            EXPECT_GT(p.signedDistance({0, 0, -5}), 0.0f);
        }
    }
    // Plane order: left plane has +X normal.
    EXPECT_GT(Frustum::fromViewProj(reversed).planes[Frustum::Left].normal.x, 0.5f);
    EXPECT_LT(Frustum::fromViewProj(reversed).planes[Frustum::Near].normal.z, -0.99f);
    EXPECT_GT(Frustum::fromViewProj(standard, false).planes[Frustum::Far].normal.z, 0.99f);
}

TEST(Math, FrustumCorners) {
    const glm::mat4 vp = perspectiveReversedZ(toRadians(90.0f), 1.0f, 1.0f, 10.0f);
    const auto c = Frustum::corners(glm::inverse(vp), true);
    EXPECT_TRUE(vecNear(c[0], {-1, -1, -1}, 1e-3f));
    EXPECT_TRUE(vecNear(c[2], {1, 1, -1}, 1e-3f));
    EXPECT_TRUE(vecNear(c[4], {-10, -10, -10}, 1e-2f));
    EXPECT_TRUE(vecNear(c[6], {10, 10, -10}, 1e-2f));
    const glm::mat4 vpStd = glm::perspectiveRH_ZO(toRadians(90.0f), 1.0f, 1.0f, 10.0f);
    const auto cs = Frustum::corners(glm::inverse(vpStd), false);
    for (u32 i = 0; i < 8; ++i) {
        EXPECT_TRUE(vecNear(cs[i], c[i], 1e-2f));
    }
}

TEST(Math, RayIntersections) {
    const Ray r{{0, 0, 5}, {0, 0, -1}};
    EXPECT_TRUE(vecNear(r.at(2.0f), {0, 0, 3}));

    const AABB box = AABB::fromCenterExtents({0, 0, 0}, glm::vec3{1});
    auto t = intersectRayAABB(r, box);
    ASSERT_TRUE(t);
    EXPECT_NEAR(*t, 4.0f, 1e-5f);
    EXPECT_FALSE(intersectRayAABB(Ray{{0, 0, 5}, {0, 0, 1}}, box));
    EXPECT_FALSE(intersectRayAABB(Ray{{2, 0, 5}, {0, 0, -1}}, box)); // parallel, outside slab
    EXPECT_NEAR(*intersectRayAABB(Ray{{0, 0, 0}, {1, 0, 0}}, box), 0.0f, 1e-6f); // inside
    EXPECT_TRUE(intersectRayAABB(Ray{{1, 0, 5}, {0, 0, -1}}, box));            // on the boundary

    auto ts = intersectRaySphere(r, Sphere{{0, 0, 0}, 1.0f});
    ASSERT_TRUE(ts);
    EXPECT_NEAR(*ts, 4.0f, 1e-5f);
    EXPECT_NEAR(*intersectRaySphere(Ray{{0, 0, 0}, {0, 0, -1}}, Sphere{{0, 0, 0}, 2.0f}), 2.0f, 1e-5f);
    EXPECT_FALSE(intersectRaySphere(Ray{{0, 3, 5}, {0, 0, -1}}, Sphere{{0, 0, 0}, 1.0f}));
    // Unnormalised direction: t is in units of direction length.
    EXPECT_NEAR(*intersectRaySphere(Ray{{0, 0, 5}, {0, 0, -2}}, Sphere{{0, 0, 0}, 1.0f}), 2.0f, 1e-5f);

    const Plane ground = Plane::fromPointNormal({0, 0, 0}, {0, 1, 0});
    EXPECT_NEAR(*intersectRayPlane(Ray{{0, 10, 0}, {0, -1, 0}}, ground), 10.0f, 1e-5f);
    EXPECT_FALSE(intersectRayPlane(Ray{{0, 10, 0}, {1, 0, 0}}, ground));
    EXPECT_FALSE(intersectRayPlane(Ray{{0, 10, 0}, {0, 1, 0}}, ground));
}

TEST(Math, RayTriangle) {
    const glm::vec3 a{-1, -1, 0}, b{1, -1, 0}, c{0, 1, 0}; // CCW seen from +Z
    const Ray front{{0, 0, 5}, {0, 0, -1}};
    auto hit = intersectRayTriangle(front, a, b, c);
    ASSERT_TRUE(hit);
    EXPECT_NEAR(hit->t, 5.0f, 1e-5f);
    EXPECT_TRUE(hit->frontFace);
    EXPECT_TRUE(vecNear(hit->normal, {0, 0, 1}));
    const glm::vec3 p = (1 - hit->u - hit->v) * a + hit->u * b + hit->v * c;
    EXPECT_TRUE(vecNear(p, front.at(hit->t)));

    const Ray back{{0, 0, -5}, {0, 0, 1}};
    auto backHit = intersectRayTriangle(back, a, b, c);
    ASSERT_TRUE(backHit);
    EXPECT_FALSE(backHit->frontFace);
    EXPECT_FALSE(intersectRayTriangle(back, a, b, c, true));
    EXPECT_FALSE(intersectRayTriangle(Ray{{5, 5, 5}, {0, 0, -1}}, a, b, c));
    EXPECT_FALSE(intersectRayTriangle(Ray{{0, 0, 5}, {1, 0, 0}}, a, b, c)); // parallel
    EXPECT_FALSE(intersectRayTriangle(Ray{{0, 0, -1}, {0, 0, -1}}, a, b, c)); // behind origin
}

TEST(Math, RandomDeterministicAndInRange) {
    Random a(42), b(42);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.nextU64(), b.nextU64());
    }
    Random d(42);
    Random e(43);
    EXPECT_NE(d.nextU64(), e.nextU64());

    Random r(7);
    std::set<i32> seen;
    f64 sum = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const f32 f = r.nextFloat();
        ASSERT_GE(f, 0.0f);
        ASSERT_LT(f, 1.0f);
        sum += f;
        const i32 k = r.rangeInt(-3, 3);
        ASSERT_GE(k, -3);
        ASSERT_LE(k, 3);
        seen.insert(k);
        const f32 x = r.range(2.0f, 5.0f);
        ASSERT_GE(x, 2.0f);
        ASSERT_LT(x, 5.0f);
        ASSERT_NEAR(glm::length(r.unitVector()), 1.0f, 1e-4f);
        ASSERT_LT(glm::length(r.inUnitSphere()), 1.0f);
        ASSERT_NEAR(glm::length(r.rotation()), 1.0f, 1e-4f);
    }
    EXPECT_EQ(seen.size(), 7u);
    EXPECT_NEAR(sum / 20000.0, 0.5, 0.02);
    EXPECT_EQ(r.rangeInt(5, 5), 5);
    EXPECT_EQ(Random(1).rangeInt(std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()),
              Random(1).rangeInt(std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()));
}
