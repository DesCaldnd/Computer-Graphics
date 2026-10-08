// Глава 10: IK — two-bone, aim, FABRIK, foot IK (docs/guide/10-animation.md).
#include "guide_rig.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using namespace ox;
using namespace ox::anim;

TEST(GuideAnimationIK, TwoBoneAndAim) {
    const Skeleton skel = guide::makeArm();
    Pose pose;
    pose.setBind(skel);
    std::vector<Transform> model; // IK работает в модельном пространстве
    localToModel(skel, pose, model);

    TwoBoneIKSettings ik;
    ik.root = skel.findJoint("Shoulder");
    ik.mid = skel.findJoint("Elbow");
    ik.end = skel.findJoint("Wrist");
    ik.target = {1.0f, 1.0f, 0.5f};
    ik.pole = {0.0f, 1.0f, 5.0f}; // локоть сгибается в сторону +Z
    IKResult r = solveTwoBoneIK(skel, pose, model, ik);
    EXPECT_TRUE(r.reached);
    EXPECT_LT(glm::length(model[2].translation - ik.target), 1e-3f);
    // pose.local тоже обновлён — решатели можно выстраивать цепочкой.

    AimIKSettings aim;
    aim.joint = ik.end;
    aim.aimAxis = {0, 1, 0}; // локальная ось кисти, которую наводим на цель
    aim.upAxis = {0, 0, 0};  // без контроля скручивания
    aim.target = {5.0f, 1.0f, 0.5f};
    r = solveAimIK(skel, pose, model, aim);
    EXPECT_TRUE(r.reached);
    const glm::vec3 axis = model[2].rotation * glm::vec3(0, 1, 0);
    EXPECT_GT(glm::dot(axis, glm::normalize(aim.target - model[2].translation)), 0.999f);
}

TEST(GuideAnimationIK, FabrikTail) {
    // Хвост из 5 суставов.
    Skeleton tail;
    std::vector<i32> chain;
    i32 parent = kNoJoint;
    for (int i = 0; i < 5; ++i) {
        parent = tail.addJoint("tail" + std::to_string(i), parent, Transform{{0, i == 0 ? 0.0f : 1.0f, 0}});
        chain.push_back(parent);
    }
    tail.finalize();
    Pose pose;
    pose.setBind(tail);
    std::vector<Transform> model;
    localToModel(tail, pose, model);

    FabrikSettings f;
    f.chain = chain; // корень -> кончик
    f.target = {2.0f, 2.0f, 1.0f};
    f.tolerance = 1e-4f;
    f.maxIterations = 64;
    const IKResult r = solveFABRIK(tail, pose, model, f);
    EXPECT_TRUE(r.reached);
    EXPECT_LT(glm::length(model[4].translation - f.target), 1e-3f);
}

TEST(GuideAnimationIK, FootIK) {
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

    // Луч в мир даёт игра (обычно PhysicsWorld::raycast). Здесь: левая нога над ямой -0.1,
    // правая — над ступенькой +0.15.
    GroundRaycast ray = [](const glm::vec3& origin, const glm::vec3&, f32 maxDist) -> std::optional<GroundHit> {
        const f32 ground = origin.x < 0.0f ? -0.1f : 0.15f;
        if (origin.y - ground > maxDist) return std::nullopt;
        return GroundHit{{origin.x, ground, origin.z}, {0, 1, 0}};
    };
    FootIKSettings settings;
    settings.pelvis = pelvis;
    settings.ownerWorld = Transform{}; // модель -> мир (трансформ персонажа)
    const FootIKLeg legs[] = {{hipL, kneeL, ankleL, 0.0f}, {hipR, kneeR, ankleR, 0.0f}};
    const FootIKResult r = solveFootIK(s, pose, model, legs, settings, ray);
    ASSERT_TRUE(r.grounded[0] && r.grounded[1]);
    EXPECT_NEAR(r.pelvisOffset, -0.1f, 1e-4f); // таз опустился под нижнюю ногу
    EXPECT_NEAR(model[static_cast<usize>(ankleL)].translation.y, -0.1f, 1e-3f);
    EXPECT_NEAR(model[static_cast<usize>(ankleR)].translation.y, 0.15f, 1e-3f);
}
