#include "test_helpers.hpp"

using namespace ox;
using namespace ox::anim;
using namespace ox::anim::test;

namespace {

struct ChainState {
    Skeleton skeleton = makeChainSkeleton();
    Pose pose;
    std::vector<Transform> model;
    ChainState() {
        pose.setBind(skeleton);
        localToModel(skeleton, pose, model);
    }
    glm::vec3 pos(i32 j) const { return model[static_cast<usize>(j)].translation; }
};

} // namespace

TEST(AnimIK, TwoBoneReachesReachableTarget) {
    ChainState c;
    TwoBoneIKSettings ik;
    ik.root = 0;
    ik.mid = 1;
    ik.end = 2;
    ik.target = {1.0f, 1.0f, 0.5f};
    ik.pole = {0.0f, 1.0f, 5.0f};
    const IKResult r = solveTwoBoneIK(c.skeleton, c.pose, c.model, ik);
    EXPECT_TRUE(r.reached);
    EXPECT_LT(r.error, 1e-4f);
    expectVec(c.pos(2), ik.target, 1e-4f);
    EXPECT_NEAR(glm::length(c.pos(1) - c.pos(0)), 1.0f, 1e-5f); // bone lengths preserved
    EXPECT_NEAR(glm::length(c.pos(2) - c.pos(1)), 1.0f, 1e-5f);
    // Knee bends towards the pole (+Z side of the root→target line).
    const glm::vec3 dir = glm::normalize(ik.target - c.pos(0));
    const glm::vec3 off = c.pos(1) - dir * glm::dot(c.pos(1), dir);
    EXPECT_GT(off.z, 0.1f);
    // Model and local stay consistent.
    std::vector<Transform> check;
    localToModel(c.skeleton, c.pose, check);
    expectVec(check[2].translation, c.pos(2), 1e-5f);
    // End effector keeps its original model orientation.
    expectQuat(c.model[2].rotation, glm::quat(1, 0, 0, 0), 1e-5f);
}

TEST(AnimIK, TwoBoneUnreachableStretchesTowardTarget) {
    {
        ChainState c;
        TwoBoneIKSettings ik{0, 1, 2};
        ik.target = {3.0f, 0.0f, 0.0f};
        const IKResult r = solveTwoBoneIK(c.skeleton, c.pose, c.model, ik);
        EXPECT_FALSE(r.reached);
        expectVec(c.pos(2), {2.0f, 0.0f, 0.0f}, 1e-2f); // fully extended toward the target
        EXPECT_NEAR(r.error, 1.0f, 1e-2f);
    }
    {
        ChainState c;
        TwoBoneIKSettings ik{0, 1, 2};
        ik.target = {3.0f, 0.0f, 0.0f};
        ik.allowStretch = true;
        ik.maxStretch = 1.25f;
        solveTwoBoneIK(c.skeleton, c.pose, c.model, ik);
        expectVec(c.pos(2), {2.5f, 0.0f, 0.0f}, 1e-2f);
        ik.maxStretch = 2.0f;
        ChainState c2;
        const IKResult r = solveTwoBoneIK(c2.skeleton, c2.pose, c2.model, ik);
        EXPECT_LT(r.error, 1e-2f);
    }
}

TEST(AnimIK, TwoBoneWeightBlends) {
    ChainState c;
    TwoBoneIKSettings ik{0, 1, 2};
    ik.target = {1.0f, 1.0f, 0.0f};
    ik.weight = 0.0f;
    solveTwoBoneIK(c.skeleton, c.pose, c.model, ik);
    expectVec(c.pos(2), {0, 2, 0}, 1e-5f);
}

TEST(AnimIK, AimPointsAxisAtTarget) {
    ChainState c;
    AimIKSettings aim;
    aim.joint = 1;
    aim.aimAxis = {0, 1, 0};
    aim.upAxis = {0, 0, 0};
    aim.target = {3.0f, 1.0f, 2.0f};
    const IKResult r = solveAimIK(c.skeleton, c.pose, c.model, aim);
    EXPECT_TRUE(r.reached);
    const glm::vec3 axis = c.model[1].rotation * glm::vec3(0, 1, 0);
    expectVec(axis, glm::normalize(aim.target - c.pos(1)), 1e-4f);
    // Clamped variant.
    ChainState c2;
    aim.maxAngle = glm::radians(10.0f);
    solveAimIK(c2.skeleton, c2.pose, c2.model, aim);
    const glm::vec3 axis2 = c2.model[1].rotation * glm::vec3(0, 1, 0);
    EXPECT_NEAR(glm::degrees(std::acos(glm::dot(axis2, glm::vec3(0, 1, 0)))), 10.0f, 1e-2f);
}

TEST(AnimIK, LookAtChainDistributes) {
    ChainState c;
    const i32 joints[] = {0, 1, 2};
    const f32 weights[] = {0.3f, 0.5f, 1.0f};
    LookAtChainSettings la;
    la.joints = joints;
    la.weights = weights;
    la.aimAxis = {0, 0, 1};
    la.target = {3.0f, 2.0f, 0.5f};
    la.maxAngle = glm::radians(120.0f);
    const IKResult r = solveLookAtChain(c.skeleton, c.pose, c.model, la);
    EXPECT_LT(r.error, 1e-2f);
    // Root took part of the rotation.
    EXPECT_GT(glm::angle(c.pose.local[0].rotation), 0.1f);
}

TEST(AnimIK, FabrikConverges) {
    Skeleton s;
    i32 parent = kNoJoint;
    std::vector<i32> chain;
    for (int i = 0; i < 5; ++i) {
        parent = s.addJoint("j" + std::to_string(i), parent, Transform{{0, i == 0 ? 0.0f : 1.0f, 0}});
        chain.push_back(parent);
    }
    s.finalize();
    Pose pose;
    pose.setBind(s);
    std::vector<Transform> model;
    localToModel(s, pose, model);

    FabrikSettings f;
    f.chain = chain;
    f.target = {2.0f, 2.0f, 1.0f};
    f.tolerance = 1e-4f;
    f.maxIterations = 64;
    const IKResult r = solveFABRIK(s, pose, model, f);
    EXPECT_TRUE(r.reached);
    EXPECT_LT(r.iterations, 64u);
    EXPECT_LT(glm::length(model[4].translation - f.target), 1e-3f);
    for (int i = 1; i < 5; ++i) {
        EXPECT_NEAR(glm::length(model[static_cast<usize>(i)].translation - model[static_cast<usize>(i - 1)].translation),
                    1.0f, 1e-4f);
    }
    // Unreachable: straight line toward the target.
    std::vector<glm::vec3> pts = {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}};
    const IKResult u = fabrikPositions(pts, {10, 0, 0}, 1e-4f, 10);
    EXPECT_FALSE(u.reached);
    expectVec(pts[2], {2, 0, 0}, 1e-5f);
}

TEST(AnimIK, FootIKPlantsFeetOnUnevenGround) {
    Skeleton s;
    const i32 pelvis = s.addJoint("Pelvis", kNoJoint, Transform{{0, 1.0f, 0}});
    const i32 hipL = s.addJoint("HipL", pelvis, Transform{{-0.2f, 0, 0}});
    const i32 kneeL = s.addJoint("KneeL", hipL, Transform{{0, -0.5f, 0.05f}});
    const i32 ankleL = s.addJoint("AnkleL", kneeL, Transform{{0, -0.5f, -0.05f}});
    const i32 hipR = s.addJoint("HipR", pelvis, Transform{{0.2f, 0, 0}});
    const i32 kneeR = s.addJoint("KneeR", hipR, Transform{{0, -0.5f, 0.05f}});
    const i32 ankleR = s.addJoint("AnkleR", kneeR, Transform{{0, -0.5f, -0.05f}});
    s.finalize();
    Pose pose;
    pose.setBind(s);
    std::vector<Transform> model;
    localToModel(s, pose, model);

    // Character stands at (10, 0, 0) world. Left ground is 0.1 lower, right ground is a 0.15 step, tilted.
    FootIKSettings settings;
    settings.pelvis = pelvis;
    settings.ownerWorld.translation = {10.0f, 0.0f, 0.0f};
    const glm::vec3 tilted = glm::normalize(glm::vec3(0.2f, 1.0f, 0.0f));
    GroundRaycast ray = [&](const glm::vec3& origin, const glm::vec3& dir, f32 maxDist) -> std::optional<GroundHit> {
        EXPECT_NEAR(dir.y, -1.0f, 1e-6f);
        const f32 ground = origin.x < 10.0f ? -0.1f : 0.15f;
        if (origin.y - ground > maxDist) return std::nullopt;
        return GroundHit{{origin.x, ground, origin.z}, origin.x < 10.0f ? glm::vec3(0, 1, 0) : tilted};
    };
    const FootIKLeg legs[] = {{hipL, kneeL, ankleL, 0.0f}, {hipR, kneeR, ankleR, 0.0f}};
    const FootIKResult r = solveFootIK(s, pose, model, legs, settings, ray);
    ASSERT_TRUE(r.grounded[0] && r.grounded[1]);
    EXPECT_NEAR(r.pelvisOffset, -0.1f, 1e-5f);
    EXPECT_NEAR(model[static_cast<usize>(pelvis)].translation.y, 0.9f, 1e-5f);
    EXPECT_NEAR(model[static_cast<usize>(ankleL)].translation.y, -0.1f, 1e-3f);
    EXPECT_NEAR(model[static_cast<usize>(ankleR)].translation.y, 0.15f, 1e-3f);
    // Right foot aligned to the slope normal.
    const glm::vec3 footUp = model[static_cast<usize>(ankleR)].rotation * glm::vec3(0, 1, 0);
    expectVec(footUp, tilted, 1e-4f);
}
