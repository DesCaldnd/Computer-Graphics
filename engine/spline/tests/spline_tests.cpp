#include <oxwald/spline/bezier.hpp>
#include <oxwald/spline/spline.hpp>
#include <oxwald/spline/spline_debug.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <random>
#include <vector>

using namespace ox;
using namespace ox::spline;

namespace {

constexpr f32 kK = 0.5522847498f; // cubic Bézier circle handle factor

void expectNear(const glm::vec3& a, const glm::vec3& b, f32 tol) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

Spline bezierCircle(f32 r) {
    Spline s(SplineType::Bezier, true);
    const glm::vec3 pts[4] = {{r, 0, 0}, {0, 0, -r}, {-r, 0, 0}, {0, 0, r}};
    const glm::vec3 tan[4] = {{0, 0, -1}, {-1, 0, 0}, {0, 0, 1}, {1, 0, 0}};
    for (int i = 0; i < 4; ++i) {
        ControlPoint cp;
        cp.position = pts[i];
        cp.handleMode = HandleMode::Mirrored;
        cp.outHandle = tan[i] * (kK * r);
        cp.inHandle = -cp.outHandle;
        s.addPoint(cp);
    }
    return s;
}

Spline nurbsCircle(f32 r) {
    Spline s(SplineType::Nurbs, false);
    const f32 w = std::sqrt(0.5f);
    const glm::vec3 pts[9] = {{r, 0, 0}, {r, r, 0}, {0, r, 0}, {-r, r, 0}, {-r, 0, 0},
                              {-r, -r, 0}, {0, -r, 0}, {r, -r, 0}, {r, 0, 0}};
    std::vector<ControlPoint> cps;
    for (int i = 0; i < 9; ++i) {
        ControlPoint cp;
        cp.position = pts[i];
        cp.weight = (i % 2 == 1) ? w : 1.0f;
        cps.push_back(cp);
    }
    SplineSettings settings;
    settings.degree = 2;
    settings.knots = {0, 0, 0, 0.25f, 0.25f, 0.5f, 0.5f, 0.75f, 0.75f, 1, 1, 1};
    s.setSettings(settings);
    s.setPoints(cps);
    return s;
}

Spline wiggly(SplineType type, bool closed) {
    Spline s(type, closed);
    s.addPoint({0, 0, 0});
    s.addPoint({1, 0.5f, -0.2f});
    s.addPoint({4, -1, -3});
    s.addPoint({4.5f, 2, -6});
    s.addPoint({8, 1, -6.5f});
    s.addPoint({12, 0, -2});
    return s;
}

} // namespace

TEST(Spline, BezierMatchesDeCasteljau) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<f32> dist(-5.0f, 5.0f);
    for (int i = 0; i < 50; ++i) {
        const CubicBezier b{glm::vec3(dist(rng), dist(rng), dist(rng)), glm::vec3(dist(rng), dist(rng), dist(rng)),
                            glm::vec3(dist(rng), dist(rng), dist(rng)), glm::vec3(dist(rng), dist(rng), dist(rng))};
        for (f32 u = 0.0f; u <= 1.0f; u += 0.05f) {
            expectNear(bezierPosition(b, u), deCasteljau(b, u), 1e-4f);
        }
        const f32 h = 1e-3f;
        const glm::vec3 fd = (bezierPosition(b, 0.5f + h) - bezierPosition(b, 0.5f - h)) / (2.0f * h);
        expectNear(bezierDerivative(b, 0.5f), fd, 2e-2f);
    }
    // Spline segments evaluate the same Bézier as their control points/handles describe.
    Spline s(SplineType::Bezier);
    ControlPoint a;
    a.position = {0, 0, 0};
    a.handleMode = HandleMode::Free;
    a.outHandle = {1, 2, 0};
    ControlPoint b2;
    b2.position = {4, 0, 1};
    b2.handleMode = HandleMode::Free;
    b2.inHandle = {0, 1, -1};
    s.addPoint(a);
    s.addPoint(b2);
    const CubicBezier ref{a.position, a.position + a.outHandle, b2.position + b2.inHandle, b2.position};
    for (f32 t = 0.0f; t <= 1.0f; t += 0.1f) {
        expectNear(s.position(t), deCasteljau(ref, t), 1e-5f);
    }
}

TEST(Spline, EndpointsInterpolated) {
    for (SplineType type : {SplineType::Linear, SplineType::Bezier, SplineType::CatmullRom}) {
        Spline s = wiggly(type, false);
        ASSERT_EQ(s.segmentCount(), 5u);
        for (usize i = 0; i < s.pointCount(); ++i) {
            expectNear(s.position(static_cast<f32>(i)), s.point(i).position, 1e-5f);
        }
    }
    for (f32 alpha : {kCatmullRomUniform, kCatmullRomCentripetal, kCatmullRomChordal}) {
        Spline s = wiggly(SplineType::CatmullRom, true);
        SplineSettings st;
        st.catmullRomAlpha = alpha;
        s.setSettings(st);
        ASSERT_EQ(s.segmentCount(), 6u);
        for (usize i = 0; i < s.pointCount(); ++i) {
            expectNear(s.position(static_cast<f32>(i)), s.point(i).position, 1e-5f);
        }
    }
    for (SplineType type : {SplineType::BSpline, SplineType::Nurbs}) {
        Spline s = wiggly(type, false);
        ASSERT_EQ(s.segmentCount(), 3u); // n - p clamped spans
        expectNear(s.position(0.0f), s.points().front().position, 1e-5f);
        expectNear(s.position(s.maxT()), s.points().back().position, 1e-4f);
        // Clamped ends are tangent to the control polygon.
        const glm::vec3 t0 = s.tangent(0.0f);
        EXPECT_GT(glm::dot(t0, glm::normalize(s.point(1).position - s.point(0).position)), 0.9999f);
    }
}

TEST(Spline, C1AtJoins) {
    auto checkJoins = [](const Spline& s, bool exactC1) {
        for (u32 i = 1; i < s.segmentCount(); ++i) {
            const f32 t = static_cast<f32>(i);
            const glm::vec3 left = s.derivative(t - 1e-4f);
            const glm::vec3 right = s.derivative(t + 1e-4f);
            if (exactC1) {
                EXPECT_LT(glm::length(left - right), 1e-2f * std::max(1.0f, glm::length(left))) << "join " << i;
            } else {
                EXPECT_GT(glm::dot(glm::normalize(left), glm::normalize(right)), 0.9999f) << "join " << i;
            }
        }
    };
    checkJoins(wiggly(SplineType::Bezier, false), true); // Auto handles
    checkJoins(wiggly(SplineType::Bezier, true), true);

    Spline mirrored = wiggly(SplineType::Bezier, false);
    for (usize i = 0; i < mirrored.pointCount(); ++i) {
        mirrored.setHandleMode(i, HandleMode::Mirrored);
        mirrored.setOutHandle(i, glm::vec3(1.0f, 0.3f * static_cast<f32>(i), -0.5f));
    }
    checkJoins(mirrored, true);

    Spline uniform = wiggly(SplineType::CatmullRom, false);
    SplineSettings st;
    st.catmullRomAlpha = kCatmullRomUniform;
    uniform.setSettings(st);
    checkJoins(uniform, true);
    checkJoins(wiggly(SplineType::CatmullRom, false), false); // centripetal: G1
    checkJoins(wiggly(SplineType::BSpline, false), true);
}

TEST(Spline, ArcLengthBezierCircle) {
    const f32 r = 3.0f;
    const Spline s = bezierCircle(r);
    EXPECT_EQ(s.segmentCount(), 4u);
    EXPECT_NEAR(s.length(), glm::two_pi<f32>() * r, glm::two_pi<f32>() * r * 1e-3f);
    const Bounds b = s.bounds();
    expectNear(b.min, {-r, 0, -r}, 1e-3f);
    expectNear(b.max, {r, 0, r}, 1e-3f);
}

TEST(Spline, NurbsExactCircle) {
    const f32 r = 2.0f;
    const Spline s = nurbsCircle(r);
    ASSERT_EQ(s.segmentCount(), 4u);
    for (f32 t = 0.0f; t <= 4.0f; t += 0.0625f) {
        EXPECT_NEAR(glm::length(s.position(t)), r, 1e-5f);
        EXPECT_NEAR(s.curvature(t), 1.0f / r, 1e-3f);
    }
    EXPECT_NEAR(s.length(), glm::two_pi<f32>() * r, 1e-4f);
}

TEST(Spline, DistanceRoundTrip) {
    for (SplineType type : {SplineType::Bezier, SplineType::CatmullRom, SplineType::Nurbs, SplineType::Linear}) {
        const Spline s = wiggly(type, false);
        const f32 total = s.length();
        ASSERT_GT(total, 0.0f);
        for (int i = 0; i <= 40; ++i) {
            const f32 d = total * static_cast<f32>(i) / 40.0f;
            const f32 t = s.distanceToT(d);
            EXPECT_NEAR(s.tToDistance(t), d, 1e-3f) << "type " << static_cast<int>(type);
        }
        for (f32 t = 0.0f; t <= s.maxT(); t += 0.13f) {
            EXPECT_NEAR(s.distanceToT(s.tToDistance(t)), t, 1e-3f);
        }
        // Uniform parameters are equally spaced in distance.
        const auto ts = s.uniformParameters(25);
        for (usize i = 0; i < ts.size(); ++i) {
            EXPECT_NEAR(s.tToDistance(ts[i]), total * static_cast<f32>(i) / 25.0f, 1e-3f);
        }
    }
    // Brute force polyline length agrees with the quadrature.
    const Spline s = wiggly(SplineType::CatmullRom, false);
    f64 poly = 0.0;
    glm::vec3 prev = s.position(0.0f);
    for (int i = 1; i <= 20000; ++i) {
        const glm::vec3 p = s.position(s.maxT() * static_cast<f32>(i) / 20000.0f);
        poly += glm::length(p - prev);
        prev = p;
    }
    EXPECT_NEAR(s.length(), poly, 1e-3 * poly);
}

TEST(Spline, ClosestPoint) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> dist(-3.0f, 13.0f);
    for (SplineType type : {SplineType::Bezier, SplineType::CatmullRom, SplineType::Nurbs}) {
        const Spline s = wiggly(type, false);
        for (int i = 0; i < 30; ++i) {
            const glm::vec3 q(dist(rng), dist(rng) * 0.3f, -dist(rng) * 0.6f);
            f32 brute = 1e30f;
            for (int k = 0; k <= 5000; ++k) {
                brute = std::min(brute, glm::length(s.position(s.maxT() * static_cast<f32>(k) / 5000.0f) - q));
            }
            const ClosestPointResult res = s.closestPoint(q);
            EXPECT_LE(res.distance, brute + 1e-4f);
            EXPECT_NEAR(res.distance, glm::length(res.position - q), 1e-4f);
            expectNear(res.position, s.position(res.t), 1e-5f);
        }
    }
    // Point exactly on the curve.
    const Spline s = wiggly(SplineType::CatmullRom, false);
    const glm::vec3 on = s.position(2.37f);
    const ClosestPointResult res = s.closestPoint(on);
    EXPECT_NEAR(res.t, 2.37f, 1e-3f);
    EXPECT_LT(res.distance, 1e-4f);
}

TEST(Spline, FramesOrthonormalAndRotationMinimizing) {
    // Planar curve in XZ: an RMF seeded with +Y keeps +Y (a Frenet frame would flip at inflections).
    Spline planar(SplineType::CatmullRom);
    planar.addPoint({0, 0, 0});
    planar.addPoint({3, 0, -2});
    planar.addPoint({6, 0, 2});
    planar.addPoint({9, 0, -2});
    for (f32 t = 0.0f; t <= planar.maxT(); t += 0.05f) {
        const SplineSample sm = planar.evaluate(t);
        expectNear(sm.normal, {0, 1, 0}, 1e-4f);
    }

    // Helix: orthonormal frames and zero accumulated twist around the tangent.
    Spline helix(SplineType::CatmullRom);
    for (int i = 0; i <= 16; ++i) {
        const f32 a = static_cast<f32>(i) * 0.6f;
        helix.addPoint({3.0f * std::cos(a), 0.5f * static_cast<f32>(i), 3.0f * std::sin(a)});
    }
    const auto samples = helix.sampleByDistance(0.01f);
    f64 twist = 0.0;
    for (usize i = 0; i < samples.size(); ++i) {
        const SplineSample& sm = samples[i];
        EXPECT_NEAR(glm::length(sm.tangent), 1.0f, 1e-4f);
        EXPECT_NEAR(glm::length(sm.normal), 1.0f, 1e-4f);
        EXPECT_NEAR(glm::length(sm.binormal), 1.0f, 1e-4f);
        EXPECT_NEAR(glm::dot(sm.tangent, sm.normal), 0.0f, 1e-4f);
        EXPECT_NEAR(glm::dot(sm.tangent, sm.binormal), 0.0f, 1e-4f);
        EXPECT_NEAR(glm::dot(sm.normal, sm.binormal), 0.0f, 1e-4f);
        expectNear(glm::cross(sm.tangent, sm.normal), sm.binormal, 1e-4f);
        if (i > 0) {
            twist += std::asin(std::clamp(glm::dot(sm.normal, samples[i - 1].binormal), -1.0f, 1.0f));
        }
    }
    EXPECT_LT(std::abs(twist), 1e-2);

    // Roll rotates the normal around the tangent.
    Spline line(SplineType::Linear);
    ControlPoint a;
    a.position = {0, 0, 0};
    a.roll = glm::half_pi<f32>();
    ControlPoint b = a;
    b.position = {0, 0, -10};
    line.addPoint(a);
    line.addPoint(b);
    const SplineSample sm = line.evaluate(0.5f);
    expectNear(sm.tangent, {0, 0, -1}, 1e-5f);
    expectNear(sm.normal, {1, 0, 0}, 1e-4f);
    expectNear(sm.binormal, {0, -1, 0}, 1e-4f);
}

TEST(Spline, UpVectorMode) {
    Spline s = wiggly(SplineType::CatmullRom, false);
    SplineSettings st;
    st.frameMode = FrameMode::UpVector;
    s.setSettings(st);
    for (f32 t = 0.0f; t <= s.maxT(); t += 0.1f) {
        const SplineSample sm = s.evaluate(t);
        // Normal lies in the plane spanned by the tangent and world up, on the up side.
        EXPECT_NEAR(glm::dot(sm.binormal, glm::vec3(0, 1, 0)), 0.0f, 1e-4f);
        EXPECT_GT(sm.normal.y, 0.0f);
    }
    s.setUp(2, glm::vec3(1, 0, 0));
    const SplineSample at2 = s.evaluate(2.0f);
    const glm::vec3 t2 = at2.tangent;
    const glm::vec3 expected = glm::normalize(glm::vec3(1, 0, 0) - t2.x * t2);
    expectNear(at2.normal, expected, 1e-4f);
}

TEST(Spline, ClosedLoopSeamContinuity) {
    for (SplineType type : {SplineType::Bezier, SplineType::CatmullRom, SplineType::BSpline, SplineType::Nurbs}) {
        // Wavy, non-planar loop so parallel transport has a non-zero holonomy to distribute.
        Spline s(type, true);
        for (int i = 0; i < 6; ++i) {
            const f32 a = glm::two_pi<f32>() * static_cast<f32>(i) / 6.0f;
            s.addPoint({5.0f * std::cos(a), 2.0f * std::sin(2.0f * a), 5.0f * std::sin(a)});
        }
        const f32 m = s.maxT();
        ASSERT_EQ(s.segmentCount(), 6u);
        expectNear(s.position(0.0f), s.position(m), 1e-4f);
        expectNear(s.tangent(0.0f), s.tangent(m), 1e-3f);
        const SplineSample a = s.evaluate(0.0f);
        const SplineSample b = s.evaluate(m);
        expectNear(a.normal, b.normal, 1e-3f);
        expectNear(a.binormal, b.binormal, 1e-3f);
        const SplineSample before = s.evaluate(m - 1e-3f);
        const SplineSample after = s.evaluate(1e-3f);
        EXPECT_LT(glm::length(before.normal - after.normal), 1e-2f) << "type " << static_cast<int>(type);
        // Wrapping parameters and distances.
        expectNear(s.position(m + 0.5f), s.position(0.5f), 1e-4f);
        expectNear(s.position(-0.5f), s.position(m - 0.5f), 1e-4f);
        EXPECT_NEAR(s.distanceToT(s.length() + 1.0f), s.distanceToT(1.0f), 1e-3f);
    }
}

TEST(Spline, SplitPreservesShape) {
    Spline s = wiggly(SplineType::Bezier, false);
    s.addMarker("m", 1.8f);
    const Spline before = s;
    const glm::vec3 markerPos = s.position(1.8f);
    const usize index = s.insertPointAt(1.37f);
    EXPECT_EQ(index, 2u);
    EXPECT_EQ(s.pointCount(), before.pointCount() + 1);
    EXPECT_NEAR(s.length(), before.length(), 1e-4f * before.length());
    for (int i = 0; i <= 200; ++i) {
        const f32 d = before.length() * static_cast<f32>(i) / 200.0f;
        expectNear(s.evaluateAtDistance(d).position, before.evaluateAtDistance(d).position, 2e-3f);
    }
    expectNear(s.position(*s.markerT("m")), markerPos, 1e-4f);
    expectNear(s.point(index).position, before.position(1.37f), 1e-5f);

    // Removing the inserted point brings back (approximately for non-auto neighbours) a valid spline.
    s.removePoint(index);
    EXPECT_EQ(s.pointCount(), before.pointCount());
}

TEST(Spline, HandleModes) {
    Spline s(SplineType::Bezier);
    s.addPoint({0, 0, 0});
    s.addPoint({5, 0, 0});
    s.setHandleMode(0, HandleMode::Mirrored);
    s.setOutHandle(0, {1, 2, 3});
    expectNear(s.point(0).inHandle, {-1, -2, -3}, 1e-6f);

    s.setHandleMode(1, HandleMode::Aligned);
    s.setInHandle(1, {-2, 0, 0});
    const f32 outLen = glm::length(s.point(1).outHandle);
    s.setInHandle(1, {0, -1, 0});
    expectNear(s.point(1).outHandle, glm::vec3(0, 1, 0) * outLen, 1e-5f);

    s.setHandleMode(1, HandleMode::Auto);
    s.setOutHandle(1, {0, 0, 2}); // editing an auto handle converts to Aligned
    EXPECT_EQ(s.point(1).handleMode, HandleMode::Aligned);

    const u64 v = s.version();
    s.setPosition(0, {0, 1, 0});
    EXPECT_GT(s.version(), v);
}

TEST(Spline, DegenerateInputsAreSafe) {
    Spline empty;
    EXPECT_EQ(empty.segmentCount(), 0u);
    EXPECT_EQ(empty.length(), 0.0f);
    EXPECT_EQ(empty.distanceToT(3.0f), 0.0f);
    (void)empty.evaluate(1.0f);
    (void)empty.closestPoint({1, 2, 3});
    Spline one(SplineType::CatmullRom);
    one.addPoint({1, 2, 3});
    expectNear(one.position(0.5f), {1, 2, 3}, 0.0f);
    Spline dup(SplineType::CatmullRom);
    dup.addPoint({0, 0, 0});
    dup.addPoint({0, 0, 0});
    dup.addPoint({1, 0, 0});
    const SplineSample sm = dup.evaluate(0.5f);
    EXPECT_FALSE(std::isnan(sm.normal.x));
}

TEST(Spline, DebugDrawEmitsLines) {
    const Spline s = wiggly(SplineType::Bezier, false);
    int lines = 0;
    const LineCallback cb = [&](glm::vec3, glm::vec3, glm::vec4) { ++lines; };
    DebugDrawOptions opt;
    opt.samplesPerSegment = 10;
    opt.frameSpacing = 0.0f;
    drawSpline(s, cb, opt);
    EXPECT_EQ(lines, 5 * 10 + 6 * 3 + 6 * 2);
}
