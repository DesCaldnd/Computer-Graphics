#include "gameplay_test_utils.hpp"

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

TEST(GameplayPhysics, RigidBodyFallsAndLandsOnStaticCollider) {
    GameplayHarness h;
    h.ground();
    Entity crate = h.box("Crate", {0.f, 5.f, 0.f});
    Entity child = h.world.create("Lamp", crate);
    child.setPosition({0.f, 1.f, 0.f});
    h.start();

    h.run(0.5);
    EXPECT_LT(crate.worldPosition().y, 4.5f) << "falls under gravity";
    EXPECT_TRUE(crate.get<RigidBodyComponent>().body().valid());

    h.run(3.0);
    EXPECT_NEAR(crate.worldPosition().y, 0.5f, 0.05f) << "rests on the ground (half extent 0.5)";
    // Child follows through the hierarchy, cached world matrix included (TransformSystem ran).
    EXPECT_NEAR(child.worldPosition().y, 1.5f, 0.06f);
    const glm::vec3 cached = glm::vec3(child.get<WorldTransformComponent>().matrix[3]);
    EXPECT_NEAR(cached.y, 1.5f, 0.06f);
    EXPECT_NEAR(cached.x, crate.worldPosition().x, 1e-3f);
}

TEST(GameplayPhysics, InterpolatedPoseAndTeleport) {
    GameplayHarness h;
    h.ground();
    Entity ball = h.box("Ball", {0.f, 10.f, 0.f});
    h.start();
    h.tick(1.0 / 60.0);
    // A frame shorter than the fixed step still yields a valid interpolated pose (between two physics states).
    const f32 y0 = ball.worldPosition().y;
    h.tick(1.0 / 240.0);
    EXPECT_LE(ball.worldPosition().y, y0 + 1e-4f);

    // Gameplay code moving a dynamic body teleports it.
    ball.setWorldPosition({3.f, 6.f, 0.f});
    h.tick();
    EXPECT_NEAR(ball.worldPosition().x, 3.f, 1e-3f);
    EXPECT_GT(ball.worldPosition().y, 5.5f);
    const physics::BodyHandle body = h.runtime<PhysicsRuntime>().bodyOf(ball);
    EXPECT_NEAR(h.runtime<PhysicsRuntime>().physicsWorld().getPosition(body).x, 3.f, 1e-3f);
}

TEST(GameplayPhysics, KinematicFollowsTransformAndEventsHaveEntities) {
    GameplayHarness h;
    Entity platform = h.box("Platform", {0.f, 0.f, 0.f}, {2.f, 0.2f, 2.f}, physics::MotionType::Kinematic);
    Entity crate = h.box("Crate", {0.f, 1.f, 0.f});
    std::vector<std::pair<std::string, std::string>> begins;
    h.start();
    ScopedConnection c = h.runtime<PhysicsRuntime>().onCollision.connect([&](const CollisionEvent& e) {
        if (e.phase == ContactPhase::Begin) begins.emplace_back(e.a.name(), e.b.name());
    });
    h.run(1.0);
    ASSERT_FALSE(begins.empty());
    EXPECT_TRUE((begins[0].first == "Platform" && begins[0].second == "Crate") ||
                (begins[0].first == "Crate" && begins[0].second == "Platform"));
    // Moving the kinematic platform's transform carries the crate along (velocity-based kinematic move).
    for (int i = 0; i < 60; ++i) {
        platform.setPosition(platform.localTransform().position + glm::vec3(1.f / 60.f, 0.f, 0.f));
        h.tick();
    }
    EXPECT_NEAR(h.runtime<PhysicsRuntime>().physicsWorld().getPosition(h.runtime<PhysicsRuntime>().bodyOf(platform)).x, 1.f, 0.05f);
    EXPECT_GT(crate.worldPosition().x, 0.3f);
}

TEST(GameplayPhysics, ScaleChangeRebuildsShapeAndComponentRemovalDestroysBody) {
    GameplayHarness h;
    h.ground();
    Entity crate = h.box("Crate", {0.f, 0.5f, 0.f});
    h.start();
    h.run(1.0);
    EXPECT_NEAR(crate.worldPosition().y, 0.5f, 0.05f);
    crate.setScale(glm::vec3(2.f));
    h.run(1.5);
    EXPECT_NEAR(crate.worldPosition().y, 1.0f, 0.06f) << "doubled box rests one unit up";

    auto& rt = h.runtime<PhysicsRuntime>();
    const u32 before = rt.physicsWorld().bodyCount();
    crate.remove<RigidBodyComponent>();
    h.tick();
    EXPECT_EQ(rt.physicsWorld().bodyCount(), before) << "collider without rigid body becomes static";
    EXPECT_EQ(rt.physicsWorld().getMotionType(rt.bodyOf(crate)), physics::MotionType::Static);
    crate.destroy();
    h.tick();
    EXPECT_EQ(rt.physicsWorld().bodyCount(), before - 1);
}

TEST(GameplayPhysics, CharacterControllerWalksOnGround) {
    GameplayHarness h;
    h.ground();
    Entity player = h.world.create("Player");
    player.setPosition({0.f, 0.5f, 0.f});
    player.add<CharacterControllerComponent>();
    h.start();
    h.run(0.5);
    EXPECT_EQ(player.get<CharacterControllerComponent>().groundState, physics::GroundState::OnGround);
    for (int i = 0; i < 60; ++i) {
        player.get<CharacterControllerComponent>().desiredVelocity = {2.f, 0.f, 0.f};
        h.tick();
    }
    EXPECT_NEAR(player.worldPosition().x, 2.f, 0.25f);
    EXPECT_NEAR(player.worldPosition().y, 0.f, 0.05f);
}

TEST(GameplayPhysics, JointHoldsBodyToWorld) {
    GameplayHarness h;
    Entity bob = h.box("Bob", {0.f, 5.f, 0.f});
    auto& j = bob.add<JointComponent>();
    j.type = physics::ConstraintType::Distance;
    j.anchor = {0.f, 0.f, 0.f};
    j.targetAnchor = {0.f, 8.f, 0.f}; // world point (no target entity)
    h.start();
    h.run(2.0);
    EXPECT_NEAR(glm::distance(bob.worldPosition(), glm::vec3(0.f, 8.f, 0.f)), 3.f, 0.1f);
}

TEST(GameplayPhysics, PlayModeCloneDoesNotMutateEditWorld) {
    GameplayHarness h;
    h.ground();
    Entity crate = h.box("Crate", {0.f, 5.f, 0.f});
    // Edit mode: systems attached, not playing -> nothing simulates.
    h.start(false);
    h.run(0.5);
    EXPECT_FLOAT_EQ(crate.worldPosition().y, 5.f);
    EXPECT_FALSE(crate.get<RigidBodyComponent>().body().valid());

    // Play in editor: simulate a clone.
    std::unique_ptr<World> play = h.world.clone();
    h.scheduler.attach(*play, h.services);
    h.active = play.get();
    h.scheduler.setPlaying(true);
    h.run(1.5);
    const Entity playCrate = play->find(crate.uuid());
    ASSERT_TRUE(playCrate.valid());
    EXPECT_LT(playCrate.worldPosition().y, 1.f);

    // Stop: back to the edit world, untouched.
    h.scheduler.setPlaying(false);
    h.scheduler.attach(h.world, h.services);
    h.active = &h.world;
    play.reset();
    h.run(0.2);
    EXPECT_FLOAT_EQ(crate.worldPosition().y, 5.f);
    EXPECT_FALSE(crate.get<RigidBodyComponent>().body().valid());
    EXPECT_EQ(h.runtime<PhysicsRuntime>().physicsWorld().bodyCount(), 0u);
}
