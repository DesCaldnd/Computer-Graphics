#include "test_helpers.hpp"

#include <random>

using namespace ox;
using namespace ox::anim;
using namespace ox::anim::test;

TEST(AnimClip, SamplesExactlyAtKeys) {
    const Skeleton s = makeChainSkeleton();
    AnimationClip c = makeBendClip(2.0f);
    Pose p;
    c.sample(s, 0.0f, p);
    expectQuat(p.local[1].rotation, glm::quat(1, 0, 0, 0));
    expectVec(p.local[0].translation, {0, 0, 0});
    c.sample(s, 0.5f, p);
    expectVec(p.local[0].translation, {0, 0, 1.0f});
    c.sample(s, 1.0f, p);
    expectQuat(p.local[1].rotation, glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1)));
    expectVec(p.local[0].translation, {0, 0, 2.0f});
    // Joints without tracks keep the bind pose.
    expectVec(p.local[2].translation, {0, 1, 0});
}

TEST(AnimClip, InterpolatesBetweenKeys) {
    const Skeleton s = makeChainSkeleton();
    AnimationClip c = makeBendClip(2.0f);
    Pose p;
    c.sample(s, 0.25f, p);
    expectVec(p.local[0].translation, {0, 0, 0.5f});
    EXPECT_NEAR(midAngle(p), glm::quarter_pi<f32>() * 0.5f, 1e-4f); // slerp is linear in angle
    c.rotationBlend = RotationBlend::Nlerp;
    c.sample(s, 0.5f, p);
    EXPECT_NEAR(midAngle(p), glm::quarter_pi<f32>(), 1e-4f); // symmetric point: nlerp == slerp
    // Clamping outside the clip range.
    c.sample(s, 5.0f, p);
    expectVec(p.local[0].translation, {0, 0, 2.0f});
}

TEST(AnimClip, StepInterpolation) {
    Track<glm::vec3> t;
    t.times = {0, 1, 2};
    t.values = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    t.interpolation = Interpolation::Step;
    expectVec(sampleTrack(t, 0.99f), {0, 0, 0});
    expectVec(sampleTrack(t, 1.0f), {1, 0, 0});
    expectVec(sampleTrack(t, 1.5f), {1, 0, 0});
}

TEST(AnimClip, CubicSplineMatchesHermite) {
    Track<glm::vec3> t;
    t.interpolation = Interpolation::CubicSpline;
    t.times = {0.0f, 2.0f};
    t.values = {{0, 0, 0}, {1, 0, 0}};
    t.inTangents = {{0, 0, 0}, {0, 0, 0}};
    t.outTangents = {{0, 0, 0}, {0, 0, 0}};
    // Zero tangents → smoothstep.
    EXPECT_NEAR(sampleTrack(t, 1.0f).x, 0.5f, 1e-5f);
    EXPECT_NEAR(sampleTrack(t, 0.5f).x, 3 * 0.0625f - 2 * 0.015625f, 1e-5f);
    // Linear tangents (slope 0.5/s) reproduce a straight line.
    t.outTangents[0] = {0.5f, 0, 0};
    t.inTangents[1] = {0.5f, 0, 0};
    EXPECT_NEAR(sampleTrack(t, 0.5f).x, 0.25f, 1e-5f);
}

TEST(AnimClip, QuaternionShortestPath) {
    Track<glm::quat> t;
    const glm::quat a = glm::angleAxis(glm::radians(10.0f), glm::vec3(0, 1, 0));
    const glm::quat b = glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0));
    t.times = {0.0f, 1.0f};
    t.values = {a, -b}; // same rotation as b, opposite hemisphere
    for (RotationBlend mode : {RotationBlend::Slerp, RotationBlend::Nlerp}) {
        const glm::quat mid = sampleTrack(t, 0.5f, mode);
        expectQuat(mid, glm::angleAxis(glm::radians(20.0f), glm::vec3(0, 1, 0)), 1e-5f);
    }
    // fixQuaternionHemispheres makes keys continuous.
    AnimationClip c;
    c.tracks.resize(1);
    c.tracks[0].rotation = t;
    c.fixQuaternionHemispheres();
    EXPECT_GT(glm::dot(c.tracks[0].rotation.values[0], c.tracks[0].rotation.values[1]), 0.0f);
}

TEST(AnimClip, CursorMatchesUncachedSampling) {
    const Skeleton s = makeChainSkeleton();
    AnimationClip c;
    c.tracks.resize(3);
    auto& tr = c.tracks[0].translation;
    for (int i = 0; i <= 100; ++i) {
        tr.times.push_back(static_cast<f32>(i) * 0.05f);
        tr.values.push_back({std::sin(static_cast<f32>(i)), static_cast<f32>(i), 0.0f});
    }
    c.computeDuration();
    SamplingCursor cursor;
    Pose a, b;
    std::mt19937 rng(42);
    std::uniform_real_distribution<f32> dist(0.0f, c.duration);
    f32 t = 0.0f;
    for (int i = 0; i < 500; ++i) {
        // Mix sequential playback with random jumps (seeks / backwards).
        t = (i % 7 == 0) ? dist(rng) : std::fmod(t + 0.013f, c.duration);
        c.sample(s, t, a, &cursor);
        c.sample(s, t, b, nullptr);
        expectVec(a.local[0].translation, b.local[0].translation, 1e-5f);
    }
}

TEST(AnimClip, EventsInWindowAndWrap) {
    AnimationClip c = makeBendClip();
    c.addEvent(0.75f, "footR");
    c.addEvent(0.25f, "footL");
    ASSERT_EQ(c.events[0].name, "footL");
    std::vector<const AnimEvent*> out;
    c.collectEvents(0.0f, 0.5f, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]->name, "footL");
    out.clear();
    c.collectEvents(0.8f, 0.3f, out); // wrapped
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]->name, "footL");
    out.clear();
    c.collectEvents(0.25f, 0.75f, out); // (t0, t1]
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]->name, "footR");
}

TEST(AnimClip, RootMotionExtraction) {
    const Skeleton s = makeChainSkeleton();
    AnimationClip c = makeBendClip(4.0f, 2.0f);
    extractRootMotion(c, s, {});
    ASSERT_FALSE(c.rootMotion.empty());
    Pose p;
    c.sample(s, 2.0f, p);
    expectVec(p.local[0].translation, {0, 0, 0}); // root stays in place
    const Transform d = c.rootMotion.delta(0.0f, 2.0f);
    expectVec(d.translation, {0, 0, 4.0f});
    const Transform half = c.rootMotion.deltaLooped(1.5f, 0.5f, 1, 2.0f);
    expectVec(half.translation, {0, 0, 2.0f});
}

TEST(AnimClip, RootMotionYawExtraction) {
    Skeleton s;
    s.addJoint("Root", kNoJoint, Transform{});
    s.finalize();
    AnimationClip c;
    c.tracks.resize(1);
    c.duration = 1.0f;
    // Walk a quarter circle: turn 90° while moving.
    c.tracks[0].rotation.times = {0.0f, 1.0f};
    c.tracks[0].rotation.values = {glm::quat(1, 0, 0, 0), glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 1, 0))};
    c.tracks[0].translation.times = {0.0f, 1.0f};
    c.tracks[0].translation.values = {{0, 1, 0}, {1, 1, 1}};
    extractRootMotion(c, s, {});
    Pose p;
    c.sample(s, 1.0f, p);
    expectQuat(p.local[0].rotation, glm::quat(1, 0, 0, 0)); // yaw moved into root motion
    expectVec(p.local[0].translation, {0, 1, 0});            // height kept (translationY off)
    const Transform d = c.rootMotion.delta(0.0f, 1.0f);
    expectQuat(d.rotation, glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 1, 0)));
    expectVec(d.translation, {1, 0, 1});
}

TEST(AnimCompact, PackedQuatAccuracy) {
    std::mt19937 rng(7);
    std::normal_distribution<f32> n(0.0f, 1.0f);
    for (int i = 0; i < 2000; ++i) {
        const glm::quat q = glm::normalize(glm::quat(n(rng), n(rng), n(rng), n(rng)));
        const glm::quat u = PackedQuat::pack(q).unpack();
        const glm::quat d = glm::conjugate(q) * u;
        const f32 angle = 2.0f * std::asin(std::min(1.0f, glm::length(glm::vec3(d.x, d.y, d.z))));
        EXPECT_LT(angle, 2e-4f); // radians
    }
}

TEST(AnimCompact, MatchesSourceClip) {
    const Skeleton s = makeChainSkeleton();
    AnimationClip c = makeBendClip(2.0f);
    c.tracks[2].scale.interpolation = Interpolation::CubicSpline;
    c.tracks[2].scale.times = {0.0f, 1.0f};
    c.tracks[2].scale.values = {{1, 1, 1}, {2, 2, 2}};
    c.tracks[2].scale.inTangents = {{0, 0, 0}, {0, 0, 0}};
    c.tracks[2].scale.outTangents = {{0, 0, 0}, {0, 0, 0}};
    for (bool quantize : {false, true}) {
        const CompactClip cc = CompactClip::build(c, {quantize, 60.0f});
        SamplingCursor cur;
        Pose a, b;
        for (f32 t = 0.0f; t <= 1.0f; t += 0.01f) {
            c.sample(s, t, a);
            cc.sample(s, t, b, &cur);
            for (usize j = 0; j < 3; ++j) {
                expectVec(a.local[j].translation, b.local[j].translation, 1e-5f);
                expectQuat(a.local[j].rotation, b.local[j].rotation, 1e-6f);
                expectVec(a.local[j].scale, b.local[j].scale, 2e-3f); // cubic resampled to linear
            }
        }
    }
    const CompactClip packed = CompactClip::build(c);
    const CompactClip unpacked = CompactClip::build(c, {false});
    EXPECT_LT(packed.memoryBytes(), unpacked.memoryBytes());
}

TEST(AnimSerialization, RoundTrip) {
    const Skeleton s = makeChainSkeleton();
    AnimationClip c = makeBendClip(2.0f);
    c.addEvent(0.5f, "hit", 3.0f);
    extractRootMotion(c, s, {});
    const CompactClip cc = CompactClip::build(c);

    ByteWriter w;
    serialize(w, s);
    serialize(w, c);
    serialize(w, cc);
    const std::vector<u8> bytes = w.take();

    ByteReader r(bytes);
    Skeleton s2;
    AnimationClip c2;
    CompactClip cc2;
    ASSERT_TRUE(deserialize(r, s2));
    ASSERT_TRUE(deserialize(r, c2));
    ASSERT_TRUE(deserialize(r, cc2));
    EXPECT_EQ(r.remaining(), 0u);

    ASSERT_EQ(s2.jointCount(), 3u);
    EXPECT_EQ(s2.jointName(2), "Tip");
    EXPECT_EQ(s2.parent(2), 1);
    EXPECT_EQ(c2.events.size(), 1u);
    EXPECT_EQ(c2.events[0].name, "hit");
    EXPECT_FLOAT_EQ(c2.events[0].payload, 3.0f);
    EXPECT_EQ(c2.rootMotion.times.size(), c.rootMotion.times.size());
    Pose a, b, d;
    c.sample(s, 0.3f, a);
    c2.sample(s2, 0.3f, b);
    cc2.sample(s2, 0.3f, d);
    for (usize j = 0; j < 3; ++j) {
        expectQuat(a.local[j].rotation, b.local[j].rotation, 1e-6f);
        expectQuat(a.local[j].rotation, d.local[j].rotation, 1e-6f);
    }

    // Truncated data fails cleanly.
    std::vector<u8> cut(bytes.begin(), bytes.begin() + static_cast<long>(bytes.size() / 2));
    ByteReader r2(cut);
    Skeleton s3;
    AnimationClip c3;
    CompactClip cc3;
    const bool ok = deserialize(r2, s3) && deserialize(r2, c3) && deserialize(r2, cc3);
    EXPECT_FALSE(ok);
}
