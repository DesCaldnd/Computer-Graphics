#include "test_helpers.hpp"

#include <numeric>

using namespace ox;
using namespace ox::anim;
using namespace ox::anim::test;

namespace {
f32 sum(const std::vector<f32>& v) { return std::accumulate(v.begin(), v.end(), 0.0f); }
} // namespace

TEST(AnimBlend, TwoPoseLerp) {
    const Skeleton s = makeChainSkeleton();
    Pose a, b, out;
    makeStaticClip("a", 0.0f)->sample(s, 0.0f, a);
    makeStaticClip("b", glm::half_pi<f32>())->sample(s, 0.0f, b);
    blendPoses(a, b, 0.5f, out);
    EXPECT_NEAR(midAngle(out), glm::quarter_pi<f32>(), 1e-4f);
}

TEST(AnimBlend, WeightsAreNormalized) {
    const Skeleton s = makeChainSkeleton();
    Pose a, b, c, out;
    makeStaticClip("a", 0.0f)->sample(s, 0.0f, a);
    makeStaticClip("b", glm::radians(60.0f))->sample(s, 0.0f, b);
    makeStaticClip("c", glm::radians(30.0f))->sample(s, 0.0f, c);
    a.local[0].translation = {0, 0, 0};
    b.local[0].translation = {3, 0, 0};
    c.local[0].translation = {0, 3, 0};
    const Pose* poses[] = {&a, &b, &c};
    const f32 weights[] = {2.0f, 1.0f, 1.0f}; // un-normalised on purpose
    const std::vector<f32> n = blendPosesWeighted(poses, weights, out);
    EXPECT_NEAR(sum(n), 1.0f, 1e-6f);
    EXPECT_NEAR(n[0], 0.5f, 1e-6f);
    expectVec(out.local[0].translation, {0.75f, 0.75f, 0.0f});
    EXPECT_NEAR(glm::length(out.local[1].rotation), 1.0f, 1e-5f);
    EXPECT_NEAR(midAngle(out), glm::radians(22.5f), glm::radians(0.5f));

    const f32 zero[] = {0.0f, 0.0f, 0.0f};
    const std::vector<f32> z = blendPosesWeighted(poses, zero, out);
    EXPECT_FLOAT_EQ(z[0], 1.0f);
}

TEST(AnimBlend, AdditiveLayer) {
    const Skeleton s = makeChainSkeleton();
    Pose bind;
    bind.setBind(s);
    // "Lean" additive: Mid +30° relative to bind.
    auto lean = makeStaticClip("lean", glm::radians(30.0f));
    const AnimationClip additive = makeAdditiveClip(*lean, s, bind);
    EXPECT_TRUE(additive.additive);

    Pose base, delta;
    makeStaticClip("base", glm::radians(45.0f))->sample(s, 0.0f, base);
    additive.sample(s, 0.0f, delta);
    expectQuat(delta.local[0].rotation, glm::quat(1, 0, 0, 0)); // untouched joints: identity delta
    expectVec(delta.local[2].translation, {0, 0, 0});

    Pose full = base;
    applyAdditive(full, delta, 1.0f);
    EXPECT_NEAR(midAngle(full), glm::radians(75.0f), 1e-4f);
    expectVec(full.local[2].translation, {0, 1, 0}); // bind translation preserved
    Pose half = base;
    applyAdditive(half, delta, 0.5f);
    EXPECT_NEAR(midAngle(half), glm::radians(60.0f), 1e-3f);

    // makeAdditiveDelta + applyAdditive reproduce the source pose.
    Pose leanPose, d2, rebuilt = bind;
    lean->sample(s, 0.0f, leanPose);
    makeAdditiveDelta(leanPose, bind, d2);
    applyAdditive(rebuilt, d2, 1.0f);
    expectQuat(rebuilt.local[1].rotation, leanPose.local[1].rotation);
}

TEST(AnimBlend, JointMask) {
    const Skeleton s = makeChainSkeleton();
    const JointMask upper = JointMask::fromBranch(s, s.findJoint("Mid"));
    ASSERT_EQ(upper.weights.size(), 3u);
    EXPECT_EQ(upper.weights[0], 0.0f);
    EXPECT_EQ(upper.weights[1], 1.0f);
    EXPECT_EQ(upper.weights[2], 1.0f);

    Pose a, b, out;
    a.setBind(s);
    b.setBind(s);
    b.local[0].translation = {5, 0, 0};
    b.local[1].rotation = glm::angleAxis(1.0f, glm::vec3(0, 0, 1));
    blendPoses(a, b, 1.0f, out, &upper);
    expectVec(out.local[0].translation, {0, 0, 0}); // masked out
    expectQuat(out.local[1].rotation, b.local[1].rotation);

    Pose add = a;
    Pose delta;
    makeAdditiveDelta(b, a, delta);
    applyAdditive(add, delta, 1.0f, &upper);
    expectVec(add.local[0].translation, {0, 0, 0});
}

TEST(AnimBlendSpace, OneDimensional) {
    BlendSpace1D bs;
    bs.addSample(4.0f, makeStaticClip("run", 2.0f));
    bs.addSample(0.0f, makeStaticClip("idle", 0.0f));
    bs.addSample(1.5f, makeStaticClip("walk", 1.0f));
    ASSERT_EQ(bs.samples()[0].clip->name, "idle");
    std::vector<f32> w;
    bs.computeWeights(-1.0f, w);
    EXPECT_FLOAT_EQ(w[0], 1.0f);
    bs.computeWeights(1.5f, w);
    EXPECT_FLOAT_EQ(w[1], 1.0f);
    bs.computeWeights(2.75f, w);
    EXPECT_NEAR(w[1], 0.5f, 1e-6f);
    EXPECT_NEAR(w[2], 0.5f, 1e-6f);
    EXPECT_NEAR(sum(w), 1.0f, 1e-6f);
    bs.computeWeights(10.0f, w);
    EXPECT_FLOAT_EQ(w[2], 1.0f);
}

TEST(AnimBlendSpace, DelaunayTriangulation) {
    std::vector<glm::vec2> grid;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x) grid.emplace_back(static_cast<f32>(x), static_cast<f32>(y));
    const auto tris = delaunayTriangulate(grid);
    EXPECT_EQ(tris.size(), 8u); // 2 * (n-1)^2 for a regular grid
    f32 area = 0.0f;
    for (const auto& t : tris) {
        const glm::vec2 a = grid[t[0]], b = grid[t[1]], c = grid[t[2]];
        area += 0.5f * std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
    }
    EXPECT_NEAR(area, 4.0f, 1e-4f);
}

TEST(AnimBlendSpace, DelaunayWeights) {
    BlendSpace2D bs(BlendSpace2D::Mode::Delaunay);
    bs.addSample({0, 0}, makeStaticClip("idle", 0.0f));
    bs.addSample({0, 2}, makeStaticClip("fwd", 0.1f));
    bs.addSample({-2, 0}, makeStaticClip("left", 0.2f));
    bs.addSample({2, 0}, makeStaticClip("right", 0.3f));
    bs.addSample({0, -2}, makeStaticClip("back", 0.4f));
    std::vector<f32> w;
    bs.computeWeights({0, 2}, w);
    EXPECT_NEAR(w[1], 1.0f, 1e-5f);
    bs.computeWeights({1, 1}, w); // edge between fwd and right
    EXPECT_NEAR(w[1], 0.5f, 1e-5f);
    EXPECT_NEAR(w[3], 0.5f, 1e-5f);
    bs.computeWeights({0.5f, 0.5f}, w); // inside triangle idle/fwd/right
    EXPECT_NEAR(w[0], 0.5f, 1e-5f);
    EXPECT_NEAR(w[1], 0.25f, 1e-5f);
    EXPECT_NEAR(w[3], 0.25f, 1e-5f);
    EXPECT_NEAR(sum(w), 1.0f, 1e-5f);
    bs.computeWeights({0, 5}, w); // outside hull → clamps to fwd
    EXPECT_NEAR(w[1], 1.0f, 1e-5f);
}

TEST(AnimBlendSpace, FreeformDirectionalWeights) {
    for (auto mode : {BlendSpace2D::Mode::FreeformDirectional, BlendSpace2D::Mode::FreeformCartesian}) {
        BlendSpace2D bs(mode);
        bs.addSample({0, 0}, makeStaticClip("idle", 0.0f));
        bs.addSample({0, 1.5f}, makeStaticClip("walkF", 0.1f));
        bs.addSample({0, 4}, makeStaticClip("runF", 0.2f));
        bs.addSample({1.5f, 0}, makeStaticClip("walkR", 0.3f));
        bs.addSample({-1.5f, 0}, makeStaticClip("walkL", 0.4f));
        bs.addSample({0, -1.5f}, makeStaticClip("walkB", 0.5f));
        std::vector<f32> w;
        for (usize i = 0; i < bs.samples().size(); ++i) {
            bs.computeWeights(bs.samples()[i].position, w);
            EXPECT_NEAR(w[i], 1.0f, 1e-4f) << "sample " << i;
            EXPECT_NEAR(sum(w), 1.0f, 1e-5f);
        }
        bs.computeWeights({0, 2.75f}, w); // between walkF and runF on the same direction
        EXPECT_GT(w[1], 0.2f);
        EXPECT_GT(w[2], 0.2f);
        EXPECT_NEAR(w[3] + w[4] + w[5], 0.0f, 1e-4f);
        bs.computeWeights({1.0f, 1.0f}, w);
        EXPECT_NEAR(sum(w), 1.0f, 1e-5f);
        EXPECT_GT(w[1], 0.05f);
        EXPECT_GT(w[3], 0.05f);
    }
}
