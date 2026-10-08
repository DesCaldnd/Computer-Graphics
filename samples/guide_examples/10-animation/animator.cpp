// Глава 10: Animator — параметры, состояния, переходы, слои, события, root motion (docs/guide/10-animation.md).
#include "guide_rig.hpp"

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::anim;

TEST(GuideAnimationAnimator, StateMachine) {
    const Skeleton skel = guide::makeArm();

    // Контроллер — неизменяемый «ассет», который можно делить между персонажами.
    auto ctrl = std::make_shared<AnimatorController>();
    const u32 speed = ctrl->addParameter("speed", ParamType::Float);
    const u32 jump = ctrl->addParameter("jump", ParamType::Trigger);
    const u32 base = ctrl->addLayer("Base");

    auto loco = std::make_shared<BlendSpace1D>();
    loco->addSample(0.0f, guide::makePoseClip("idle", 0.0f));
    loco->addSample(2.0f, guide::makePoseClip("run", 1.0f));
    const i32 locoState = ctrl->addState(base, {"Locomotion", Motion::fromBlendSpace(loco, speed)});

    StateDesc jumpDesc{"Jump", Motion::fromClip(guide::makePoseClip("jump", 1.5f, 0.5f))};
    jumpDesc.loop = false;
    const i32 jumpState = ctrl->addState(base, jumpDesc);

    ctrl->addTransition(base, kAnyState, jumpState, 0.1f).when(jump, ConditionOp::Triggered);
    auto& back = ctrl->addTransition(base, jumpState, locoState, 0.2f);
    back.hasExitTime = true; // уйти, когда прыжок доиграл
    back.exitTime = 1.0f;

    Animator animator(skel, ctrl); // рантайм-экземпляр на одного персонажа
    animator.setFloat(speed, 1.0f); // посередине между idle и run
    animator.update(1.0f / 60.0f);
    EXPECT_EQ(animator.currentState(base), locoState);
    EXPECT_NEAR(guide::elbowAngle(animator.pose()), 0.5f, 1e-3f);

    animator.setTrigger("jump"); // можно и по имени
    animator.update(0.0f);
    EXPECT_EQ(animator.nextState(base), jumpState); // начался кроссфейд 0.1 с
    for (int i = 0; i < 12; ++i) animator.update(1.0f / 60.0f);
    EXPECT_EQ(animator.currentState(base), jumpState);
    EXPECT_FALSE(animator.getBool(jump)); // триггер «съеден» переходом

    for (int i = 0; i < 60; ++i) animator.update(1.0f / 60.0f);
    EXPECT_EQ(animator.currentState(base), locoState); // вернулись по exit time
}

TEST(GuideAnimationAnimator, LayersEventsRootMotion) {
    const Skeleton skel = guide::makeArm();

    // Клип ходьбы: плечо (корень) уезжает на 1.5 м по +Z за цикл 1 с, шаг на 0.5 с.
    auto walk = std::make_shared<AnimationClip>(*guide::makePoseClip("walk", 0.0f));
    walk->tracks[0].translation.times = {0.0f, 1.0f};
    walk->tracks[0].translation.values = {{0, 0, 0}, {0, 0, 1.5f}};
    walk->addEvent(0.5f, "footstep");
    extractRootMotion(*walk, skel, {.rootJoint = 0}); // XZ + yaw -> walk->rootMotion, корень «на месте»

    auto ctrl = std::make_shared<AnimatorController>();
    const u32 base = ctrl->addLayer("Base");
    ctrl->addState(base, {"Walk", Motion::fromClip(walk)});

    // Слой «рука» поверх базы с весом 0.5, маска — только Wrist.
    const u32 upper = ctrl->addLayer("Hand", LayerBlend::Override, 0.5f);
    auto wave = std::make_shared<AnimationClip>();
    wave->tracks.resize(3);
    wave->tracks[2].rotation.times = {0.0f};
    wave->tracks[2].rotation.values = {glm::angleAxis(1.0f, glm::vec3(1, 0, 0))};
    wave->duration = 1.0f;
    ctrl->addState(upper, {"Wave", Motion::fromClip(wave)});
    ctrl->layer(upper).mask = JointMask::fromBranch(skel, skel.findJoint("Wrist"));

    Animator animator(skel, ctrl);
    Transform owner; // трансформ персонажа в мире
    int footsteps = 0;
    for (int i = 0; i < 120; ++i) { // 2 секунды
        animator.update(1.0f / 60.0f);
        for (const FiredEvent& e : animator.events()) {
            if (e.event->name == "footstep") ++footsteps;
        }
        applyRootMotion(owner, animator.rootMotionDelta());
    }
    EXPECT_EQ(footsteps, 2);
    EXPECT_NEAR(owner.translation.z, 3.0f, 1e-2f);             // 2 цикла по 1.5 м
    EXPECT_NEAR(animator.pose().local[0].translation.z, 0.0f, 1e-5f); // in-place
    EXPECT_NEAR(glm::angle(animator.pose().local[2].rotation), 0.5f, 1e-3f); // слой с весом 0.5
}
