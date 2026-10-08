#include "test_helpers.hpp"

#include <cmath>

using namespace ox;
using namespace ox::physics;
using namespace ox::physics::test;

namespace {

// Static mesh: flat floor for x in [-10, 2], then a ramp rising with `angle` along +X.
BodyHandle addRamp(PhysicsWorld& world, f32 angleRad) {
    const f32 x0 = 2.f, len = 10.f, h = len * std::tan(angleRad);
    std::vector<glm::vec3> v = {
        {-10.f, 0.f, -5.f}, {-10.f, 0.f, 5.f}, {x0, 0.f, 5.f}, {x0, 0.f, -5.f},
        {x0 + len, h, 5.f}, {x0 + len, h, -5.f},
    };
    std::vector<u32> idx = {0, 1, 2, 0, 2, 3, 3, 2, 4, 3, 4, 5};
    BodyDesc d;
    d.shape = createShape(ShapeDesc::triangleMesh(v, idx));
    d.motionType = MotionType::Static;
    return world.createBody(d);
}

CharacterHandle addCharacter(PhysicsWorld& world, glm::vec3 pos) {
    CharacterDesc c;
    c.position = pos;
    c.height = 1.8f;
    c.radius = 0.3f;
    c.maxSlopeAngle = glm::radians(45.f);
    c.maxStepHeight = 0.35f;
    c.userData = 0xC0FFEE;
    return world.createCharacter(c);
}

void walk(PhysicsWorld& world, CharacterHandle ch, glm::vec3 velocity, f32 seconds, bool jump = false) {
    int steps = int(seconds / kDt + 0.5f);
    for (int i = 0; i < steps; ++i) {
        world.moveCharacter(ch, kDt, CharacterMoveInput{.desiredVelocity = velocity, .jump = jump && i == 0});
        world.step(kDt);
    }
}

} // namespace

TEST(Character, StandsOnGround) {
    PhysicsWorld world(smallWorld());
    addFloor(world);
    CharacterHandle ch = addCharacter(world, {0.f, 0.5f, 0.f});
    ASSERT_TRUE(world.isValid(ch));
    walk(world, ch, {}, 1.f);
    CharacterState s = world.getCharacterState(ch);
    EXPECT_EQ(s.groundState, GroundState::OnGround);
    EXPECT_NEAR(s.position.y, 0.f, 0.05f);
    EXPECT_NEAR(s.groundNormal.y, 1.f, 1e-3f);
    EXPECT_EQ(s.groundUserData, 0xF100Fu);
    // The inner body makes the character visible to raycasts.
    BodyHandle inner = world.getCharacterInnerBody(ch);
    ASSERT_TRUE(inner.valid());
    auto hit = world.raycast({0.f, 0.9f, 5.f}, {0.f, 0.f, -1.f}, 10.f);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->body, inner);
    EXPECT_EQ(hit->userData, 0xC0FFEEu);
}

TEST(Character, WalksUpGentleSlope) {
    PhysicsWorld world(smallWorld());
    addRamp(world, glm::radians(30.f));
    CharacterHandle ch = addCharacter(world, {0.f, 0.05f, 0.f});
    walk(world, ch, {3.f, 0.f, 0.f}, 3.f);
    CharacterState s = world.getCharacterState(ch);
    EXPECT_GT(s.position.x, 6.f);
    // On the 30° ramp: y ≈ (x - 2) * tan 30°.
    EXPECT_NEAR(s.position.y, (s.position.x - 2.f) * std::tan(glm::radians(30.f)), 0.15f);
    EXPECT_EQ(s.groundState, GroundState::OnGround);
}

TEST(Character, BlockedBySteepSlope) {
    PhysicsWorld world(smallWorld());
    addRamp(world, glm::radians(60.f));
    CharacterHandle ch = addCharacter(world, {0.f, 0.05f, 0.f});
    walk(world, ch, {3.f, 0.f, 0.f}, 3.f);
    CharacterState s = world.getCharacterState(ch);
    EXPECT_LT(s.position.x, 2.6f);
    EXPECT_LT(s.position.y, 0.6f);
}

TEST(Character, StepsUpSmallStepButNotTallOne) {
    auto run = [](f32 stepHeight) {
        PhysicsWorld world(smallWorld());
        addFloor(world);
        BodyDesc step;
        step.shape = createShape(ShapeDesc::box({5.f, stepHeight * 0.5f, 5.f}));
        step.position = {7.f, stepHeight * 0.5f, 0.f};
        step.motionType = MotionType::Static;
        world.createBody(step);
        CharacterHandle ch = addCharacter(world, {0.f, 0.05f, 0.f});
        walk(world, ch, {2.f, 0.f, 0.f}, 3.f);
        return world.getCharacterState(ch);
    };
    CharacterState low = run(0.25f);
    EXPECT_GT(low.position.x, 4.f);
    EXPECT_NEAR(low.position.y, 0.25f, 0.05f);
    CharacterState high = run(0.7f);
    EXPECT_LT(high.position.x, 2.f);
    EXPECT_LT(high.position.y, 0.1f);
}

TEST(Character, JumpsAndLands) {
    PhysicsWorld world(smallWorld());
    addFloor(world);
    CharacterHandle ch = addCharacter(world, {0.f, 0.05f, 0.f});
    walk(world, ch, {}, 0.5f);
    ASSERT_EQ(world.getCharacterState(ch).groundState, GroundState::OnGround);
    f32 peak = 0.f;
    bool leftGround = false;
    for (int i = 0; i < 120; ++i) {
        world.moveCharacter(ch, kDt, CharacterMoveInput{.jump = i == 0, .jumpSpeed = 5.f});
        world.step(kDt);
        CharacterState s = world.getCharacterState(ch);
        peak = std::max(peak, s.position.y);
        leftGround |= s.groundState == GroundState::InAir;
    }
    EXPECT_TRUE(leftGround);
    EXPECT_NEAR(peak, 25.f / (2.f * 9.81f), 0.2f); // v²/2g ≈ 1.27 m
    EXPECT_EQ(world.getCharacterState(ch).groundState, GroundState::OnGround);
}

TEST(Character, RidesMovingPlatform) {
    PhysicsWorld world(smallWorld());
    BodyDesc p;
    p.shape = createShape(ShapeDesc::box({3.f, 0.25f, 3.f}));
    p.motionType = MotionType::Kinematic;
    p.position = {0.f, -0.25f, 0.f};
    BodyHandle platform = world.createBody(p);
    CharacterHandle ch = addCharacter(world, {0.f, 0.02f, 0.f});
    walk(world, ch, {}, 0.3f);
    f32 startX = world.getCharacterState(ch).position.x;
    for (int i = 1; i <= 120; ++i) {
        world.moveKinematic(platform, {i * kDt * 1.f, -0.25f, 0.f}, glm::quat(1, 0, 0, 0), kDt);
        world.moveCharacter(ch, kDt, {});
        world.step(kDt);
    }
    CharacterState s = world.getCharacterState(ch);
    EXPECT_NEAR(s.position.x - startX, 2.f, 0.15f);
    EXPECT_EQ(s.groundBody, platform);
}

TEST(Character, PushesDynamicBodies) {
    PhysicsWorld world(smallWorld());
    addFloor(world);
    BodyDesc crate;
    crate.shape = createShape(ShapeDesc::box(glm::vec3(0.4f)));
    crate.position = {1.5f, 0.4f, 0.f};
    crate.mass = 10.f;
    BodyHandle box = world.createBody(crate);
    CharacterHandle ch = addCharacter(world, {0.f, 0.02f, 0.f});
    walk(world, ch, {2.f, 0.f, 0.f}, 2.f);
    EXPECT_GT(world.getPosition(box).x, 2.f);
}

TEST(Character, TriggersSeeCharacters) {
    PhysicsWorld world(smallWorld());
    addFloor(world);
    BodyDesc t;
    t.shape = createShape(ShapeDesc::box({0.5f, 1.f, 2.f}));
    t.position = {3.f, 1.f, 0.f};
    t.motionType = MotionType::Static;
    t.isSensor = true;
    world.createBody(t);
    CharacterHandle ch = addCharacter(world, {0.f, 0.02f, 0.f});
    int enters = 0, exits = 0;
    world.setTriggerCallback([&](const TriggerEvent& e) {
        enters += e.type == TriggerEventType::Enter;
        exits += e.type == TriggerEventType::Exit;
    });
    walk(world, ch, {3.f, 0.f, 0.f}, 3.f);
    EXPECT_EQ(enters, 1);
    EXPECT_EQ(exits, 1);
    world.destroyCharacter(ch);
    EXPECT_FALSE(world.isValid(ch));
}

TEST(Constraints, HingeLimitRespected) {
    PhysicsWorld world(smallWorld());
    BodyDesc door;
    door.shape = createShape(ShapeDesc::box({0.5f, 1.f, 0.05f}));
    door.position = {0.5f, 1.f, 0.f};
    door.gravityFactor = 0.f;
    door.allowSleeping = false;
    BodyHandle b = world.createBody(door);

    ConstraintDesc hinge;
    hinge.type = ConstraintType::Hinge;
    hinge.bodyA = b; // bodyB invalid → world
    hinge.pointA = {0.f, 1.f, 0.f};
    hinge.axis = {0.f, 1.f, 0.f};
    hinge.limitsEnabled = true;
    hinge.limitMin = -0.5f;
    hinge.limitMax = 0.5f;
    ConstraintHandle c = world.createConstraint(hinge);
    ASSERT_TRUE(world.isValid(c));

    f32 maxAngle = 0.f, minAngle = 0.f;
    for (int i = 0; i < 180; ++i) {
        world.addTorque(b, {0.f, (i < 90 ? 1.f : -1.f) * 500.f, 0.f});
        world.step(kDt);
        f32 a = world.getJointValue(c);
        maxAngle = std::max(maxAngle, a);
        minAngle = std::min(minAngle, a);
    }
    // Soft overshoot while the limit absorbs the motion is bounded; it settles on the limit.
    EXPECT_GT(maxAngle, 0.45f);
    EXPECT_LT(maxAngle, 0.5f + 0.05f);
    EXPECT_LT(minAngle, -0.45f);
    EXPECT_GT(minAngle, -0.5f - 0.05f);
    EXPECT_NEAR(world.getJointValue(c), -0.5f, 0.02f);
    // Pivot stays at the hinge: the door's far edge never leaves radius 1.
    glm::vec3 com = world.getCenterOfMassPosition(b);
    EXPECT_NEAR(glm::length(glm::vec2(com.x, com.z)), 0.5f, 0.01f);
}

TEST(Constraints, HingeVelocityMotorAndSliderLimits) {
    PhysicsWorld world(smallWorld());
    BodyDesc wheel;
    wheel.shape = createShape(ShapeDesc::cylinder(0.1f, 0.5f));
    wheel.gravityFactor = 0.f;
    wheel.allowSleeping = false;
    BodyHandle w = world.createBody(wheel);
    ConstraintDesc hinge;
    hinge.type = ConstraintType::Hinge;
    hinge.bodyA = w;
    hinge.axis = {0.f, 1.f, 0.f};
    hinge.motorMode = MotorMode::Velocity;
    hinge.motorTarget = 2.f;
    hinge.motorMaxForce = 1e5f;
    ConstraintHandle motor = world.createConstraint(hinge);
    stepFor(world, 0.5f);
    EXPECT_NEAR(world.getAngularVelocity(w).y, 2.f, 0.05f);
    world.setMotor(motor, MotorMode::Off, 0.f);

    BodyDesc sled;
    sled.shape = createShape(ShapeDesc::box(glm::vec3(0.2f)));
    sled.position = {0.f, 5.f, 0.f};
    sled.gravityFactor = 0.f;
    sled.allowSleeping = false;
    BodyHandle s = world.createBody(sled);
    ConstraintDesc slider;
    slider.type = ConstraintType::Slider;
    slider.bodyA = s;
    slider.pointA = {0.f, 5.f, 0.f};
    slider.axis = {1.f, 0.f, 0.f};
    slider.limitsEnabled = true;
    slider.limitMin = 0.f;
    slider.limitMax = 1.f;
    ConstraintHandle sc = world.createConstraint(slider);
    for (int i = 0; i < 120; ++i) {
        world.addForce(s, {500.f, 300.f, 0.f});
        world.step(kDt);
    }
    EXPECT_NEAR(world.getJointValue(sc), 1.f, 0.03f);
    EXPECT_NEAR(world.getPosition(s).y, 5.f, 0.01f);
}

TEST(Constraints, DistanceAndPointKeepBodiesTogether) {
    PhysicsWorld world(smallWorld());
    BodyHandle bob = addDynamic(world, createShape(ShapeDesc::sphere(0.2f)), {2.f, 5.f, 0.f});
    ConstraintDesc rope;
    rope.type = ConstraintType::Distance;
    rope.bodyA = bob;
    rope.pointA = {2.f, 5.f, 0.f};
    rope.pointB = {0.f, 5.f, 0.f}; // world anchor
    ConstraintHandle c = world.createConstraint(rope);
    stepFor(world, 2.f);
    glm::vec3 p = world.getPosition(bob);
    EXPECT_NEAR(glm::distance(p, glm::vec3(0, 5, 0)), 2.f, 0.03f);
    EXPECT_NEAR(world.getJointValue(c), 2.f, 0.03f);

    BodyHandle a = addDynamic(world, createShape(ShapeDesc::box(glm::vec3(0.25f))), {0.f, 10.f, 5.f});
    BodyHandle b = addDynamic(world, createShape(ShapeDesc::box(glm::vec3(0.25f))), {0.f, 9.f, 5.f});
    ConstraintDesc ball;
    ball.type = ConstraintType::Point;
    ball.bodyA = a;
    ball.bodyB = b;
    ball.pointA = ball.pointB = {0.f, 9.5f, 5.f};
    world.createConstraint(ball);
    ConstraintDesc cone;
    cone.type = ConstraintType::Cone;
    cone.bodyA = a;
    cone.pointA = {0.f, 10.25f, 5.f};
    cone.axis = {0.f, -1.f, 0.f};
    cone.coneHalfAngle = 0.3f;
    world.createConstraint(cone);
    world.addImpulse(b, {500.f, 0.f, 0.f});
    stepFor(world, 2.f);
    EXPECT_NEAR(glm::distance(world.getPosition(a), world.getPosition(b)), 1.f, 0.05f);
    EXPECT_NEAR(glm::distance(world.getPosition(a), glm::vec3(0.f, 10.f, 5.f)), 0.f, 0.3f);

    // Destroying a body removes its constraints.
    world.destroyBody(bob);
    EXPECT_FALSE(world.isValid(c));
}

TEST(Constraints, BreakableFixedJoint) {
    PhysicsWorld world(smallWorld());
    BodyDesc d;
    d.shape = createShape(ShapeDesc::box(glm::vec3(0.5f)));
    d.position = {0.f, 5.f, 0.f};
    d.mass = 10.f;
    BodyHandle b = world.createBody(d);
    ConstraintDesc weld;
    weld.type = ConstraintType::Fixed;
    weld.bodyA = b;
    weld.pointA = {0.f, 5.5f, 0.f};
    weld.breakForce = 500.f; // holds the 98 N weight, not an extra 1000 N pull
    ConstraintHandle c = world.createConstraint(weld);
    std::vector<ConstraintBrokenEvent> broken;
    world.setConstraintBrokenCallback([&](const ConstraintBrokenEvent& e) { broken.push_back(e); });
    stepFor(world, 1.f);
    EXPECT_TRUE(broken.empty());
    EXPECT_NEAR(world.getPosition(b).y, 5.f, 0.01f);
    for (int i = 0; i < 10 && broken.empty(); ++i) {
        world.addForce(b, {0.f, -1000.f, 0.f});
        world.step(kDt);
    }
    ASSERT_EQ(broken.size(), 1u);
    EXPECT_EQ(broken[0].constraint, c);
    EXPECT_GT(broken[0].force, 500.f);
    EXPECT_FALSE(world.isConstraintEnabled(c));
    stepFor(world, 0.5f);
    EXPECT_LT(world.getPosition(b).y, 4.f);
}
