#include "test_helpers.hpp"

using namespace ox;
using namespace ox::anim;
using namespace ox::anim::test;

namespace {

struct Fixture {
    Skeleton skeleton = makeChainSkeleton();
    std::shared_ptr<AnimatorController> ctrl = std::make_shared<AnimatorController>();
    u32 speed = 0, jump = 0, grounded = 0;
    i32 idle = 0, walk = 0, jumpState = 0;

    Fixture() {
        speed = ctrl->addParameter("speed", ParamType::Float);
        jump = ctrl->addParameter("jump", ParamType::Trigger);
        grounded = ctrl->addParameter("grounded", ParamType::Bool, 1.0f);
        const u32 base = ctrl->addLayer("Base");
        idle = ctrl->addState(base, {"Idle", Motion::fromClip(makeStaticClip("idle", 0.0f))});
        walk = ctrl->addState(base, {"Walk", Motion::fromClip(makeStaticClip("walk", glm::half_pi<f32>()))});
        StateDesc j{"Jump", Motion::fromClip(makeStaticClip("jump", 1.0f, 0.5f))};
        j.loop = false;
        jumpState = ctrl->addState(base, j);
        ctrl->addTransition(base, idle, walk, 0.2f).when(speed, ConditionOp::Greater, 0.1f);
        ctrl->addTransition(base, walk, idle, 0.2f).when(speed, ConditionOp::Less, 0.1f);
        ctrl->addTransition(base, kAnyState, jumpState, 0.1f).when(jump, ConditionOp::Triggered);
        auto& back = ctrl->addTransition(base, jumpState, idle, 0.1f);
        back.hasExitTime = true;
        back.exitTime = 1.0f;
        back.when(grounded, ConditionOp::IsTrue);
    }
};

void run(Animator& a, f32 seconds, f32 dt = 1.0f / 60.0f) {
    for (f32 t = 0.0f; t < seconds - 1e-6f; t += dt) a.update(dt);
}

} // namespace

TEST(AnimStateMachine, ConditionsDriveTransitionsAndCrossfade) {
    Fixture f;
    Animator a(f.skeleton, f.ctrl);
    a.update(0.016f);
    EXPECT_EQ(a.currentState(0), f.idle);
    EXPECT_NEAR(midAngle(a.pose()), 0.0f, 1e-4f);

    a.setFloat(f.speed, 1.0f);
    a.update(0.0f); // transition starts
    EXPECT_TRUE(a.inTransition(0));
    EXPECT_EQ(a.nextState(0), f.walk);
    a.update(0.1f); // halfway through the 0.2 s crossfade
    EXPECT_NEAR(a.transitionProgress(0), 0.5f, 1e-4f);
    EXPECT_NEAR(midAngle(a.pose()), glm::quarter_pi<f32>(), 1e-3f);
    a.update(0.15f);
    EXPECT_FALSE(a.inTransition(0));
    EXPECT_EQ(a.currentState(0), f.walk);
    EXPECT_NEAR(midAngle(a.pose()), glm::half_pi<f32>(), 1e-4f);

    a.setFloat("speed", 0.0f);
    run(a, 0.5f);
    EXPECT_EQ(a.currentState(0), f.idle);
}

TEST(AnimStateMachine, TriggersAnyStateAndExitTime) {
    Fixture f;
    Animator a(f.skeleton, f.ctrl);
    a.setFloat(f.speed, 1.0f);
    run(a, 0.5f);
    ASSERT_EQ(a.currentState(0), f.walk);

    a.setTrigger(f.jump);
    a.update(0.01f);
    EXPECT_EQ(a.nextState(0), f.jumpState);
    EXPECT_FALSE(a.getBool(f.jump)) << "trigger must be consumed";
    run(a, 0.2f);
    EXPECT_EQ(a.currentState(0), f.jumpState);
    // Any-state doesn't retrigger itself (trigger consumed, canTransitionToSelf = false).
    // Exit time: jump is 0.5 s long, then returns to idle (speed still 1 → walk afterwards).
    a.setBool("grounded", false);
    run(a, 0.6f);
    EXPECT_EQ(a.currentState(0), f.jumpState) << "exit transition waits for grounded";
    a.setBool("grounded", true);
    a.update(0.016f);
    EXPECT_TRUE(a.inTransition(0));
    EXPECT_EQ(a.nextState(0), f.idle);
    run(a, 0.5f);
    EXPECT_EQ(a.currentState(0), f.walk);
}

TEST(AnimStateMachine, InterruptedTransitionFreezesSource) {
    Fixture f;
    Animator a(f.skeleton, f.ctrl);
    a.update(0.016f);
    a.crossFade(0, f.walk, 1.0f);
    run(a, 0.5f);
    const f32 before = midAngle(a.pose());
    EXPECT_GT(before, 0.3f);
    a.setTrigger(f.jump); // any-state interrupts the running crossfade
    a.update(0.0f);
    EXPECT_EQ(a.nextState(0), f.jumpState);
    EXPECT_NEAR(midAngle(a.pose()), before, 0.05f); // no pop
    run(a, 0.2f);
    EXPECT_EQ(a.currentState(0), f.jumpState);
}

TEST(AnimStateMachine, BlendSpaceStateAndLayers) {
    const Skeleton s = makeChainSkeleton();
    auto ctrl = std::make_shared<AnimatorController>();
    const u32 speed = ctrl->addParameter("speed", ParamType::Float);
    const u32 base = ctrl->addLayer("Base");
    auto bs = std::make_shared<BlendSpace1D>();
    bs->addSample(0.0f, makeStaticClip("idle", 0.0f, 1.0f));
    bs->addSample(2.0f, makeStaticClip("run", 1.0f, 0.5f));
    ctrl->addState(base, {"Locomotion", Motion::fromBlendSpace(bs, speed)});

    // Upper body override layer: Tip only, half weight.
    const u32 upper = ctrl->addLayer("Upper", LayerBlend::Override, 0.5f);
    auto aim = std::make_shared<AnimationClip>();
    aim->tracks.resize(3);
    aim->tracks[2].rotation.times = {0.0f};
    aim->tracks[2].rotation.values = {glm::angleAxis(1.0f, glm::vec3(1, 0, 0))};
    aim->duration = 1.0f;
    ctrl->addState(upper, {"Aim", Motion::fromClip(aim)});
    ctrl->layer(upper).mask = JointMask::fromBranch(s, s.findJoint("Tip"));

    // Additive layer: +0.25 rad on Mid.
    const u32 add = ctrl->addLayer("Breath", LayerBlend::Additive, 1.0f);
    Pose bind;
    bind.setBind(s);
    auto breath = std::make_shared<AnimationClip>(makeAdditiveClip(*makeStaticClip("b", 0.25f), s, bind));
    ctrl->addState(add, {"Breath", Motion::fromClip(breath)});

    Animator a(s, ctrl);
    a.setFloat(speed, 1.0f);
    a.update(0.1f);
    EXPECT_NEAR(midAngle(a.pose()), 0.5f + 0.25f, 1e-3f);
    EXPECT_NEAR(glm::angle(a.pose().local[2].rotation), 0.5f, 1e-3f);
    // Phase sync: normalized time advances by dt / (weighted duration = 0.75 s).
    EXPECT_NEAR(a.normalizedTime(0), 0.1f / 0.75f, 1e-4f);
    a.setLayerWeight(upper, 0.0f);
    a.update(0.1f);
    EXPECT_NEAR(glm::angle(a.pose().local[2].rotation), 0.0f, 1e-3f);
}

TEST(AnimStateMachine, EventsFireOncePerCycle) {
    const Skeleton s = makeChainSkeleton();
    auto ctrl = std::make_shared<AnimatorController>();
    const u32 base = ctrl->addLayer("Base");
    auto clip = makeStaticClip("walk", 0.0f, 1.0f);
    clip->addEvent(0.0f, "start");
    clip->addEvent(0.5f, "step");
    ctrl->addState(base, {"Walk", Motion::fromClip(clip)});
    Animator a(s, ctrl);
    int steps = 0, starts = 0;
    for (int i = 0; i < 295; ++i) { // 2.95 s at 100 Hz
        a.update(0.01f);
        for (const auto& e : a.events()) {
            if (e.event->name == "step") ++steps;
            if (e.event->name == "start") ++starts;
        }
    }
    EXPECT_EQ(steps, 3);
    EXPECT_EQ(starts, 3); // t = 0 (initial), 1, 2
}

TEST(AnimRootMotion, AccumulatedDistance) {
    const Skeleton s = makeChainSkeleton();
    auto clip = std::make_shared<AnimationClip>(makeBendClip(1.5f, 1.0f)); // 1.5 m per 1 s cycle
    extractRootMotion(*clip, s, {});
    auto ctrl = std::make_shared<AnimatorController>();
    const u32 base = ctrl->addLayer("Base");
    ctrl->addState(base, {"Walk", Motion::fromClip(clip)});
    Animator a(s, ctrl);

    Transform owner;
    owner.rotation = glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 1, 0)); // facing +X
    for (int i = 0; i < 250; ++i) { // 2.5 s with uneven frame times
        a.update(i % 2 ? 0.007f : 0.013f);
        applyRootMotion(owner, a.rootMotionDelta());
        EXPECT_NEAR(a.pose().local[0].translation.z, 0.0f, 1e-5f); // in-place root
    }
    expectVec(owner.translation, {1.5f * 2.5f, 0.0f, 0.0f}, 1e-3f);
}
