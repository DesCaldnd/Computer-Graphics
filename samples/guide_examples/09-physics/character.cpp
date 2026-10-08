// Глава 09: контроллер персонажа (docs/guide/09-physics.md).
#include <oxwald/physics/physics.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::physics;

TEST(GuidePhysicsCharacter, WalkJumpAndStairs) {
    PhysicsWorld world;
    BodyDesc floor;
    floor.shape = createShape(ShapeDesc::box({50.f, 0.5f, 50.f}));
    floor.position = {0.f, -0.5f, 0.f};
    floor.motionType = MotionType::Static;
    floor.userData = 1;
    world.createBody(floor);

    // Ступенька 0.25 м на x >= 3 — персонаж поднимется автоматически.
    BodyDesc step;
    step.shape = createShape(ShapeDesc::box({5.f, 0.125f, 5.f}));
    step.position = {8.f, 0.125f, 0.f};
    step.motionType = MotionType::Static;
    world.createBody(step);

    CharacterDesc cd;
    cd.position = {0.f, 0.1f, 0.f}; // позиция = ступни (низ капсулы)
    cd.height = 1.8f;
    cd.radius = 0.3f;
    cd.maxSlopeAngle = glm::radians(45.f);
    cd.maxStepHeight = 0.35f;
    cd.userData = 0xC0FFEE;
    CharacterHandle ch = world.createCharacter(cd);

    const f32 dt = 1.f / 60.f;
    auto tick = [&](glm::vec3 wish, bool jump) {
        // Каждый фиксированный шаг: сначала персонаж, потом step().
        world.moveCharacter(ch, dt, CharacterMoveInput{.desiredVelocity = wish, .jump = jump});
        world.step(dt);
    };

    for (int i = 0; i < 30; ++i) tick({}, false); // приземлиться
    CharacterState s = world.getCharacterState(ch);
    EXPECT_EQ(s.groundState, GroundState::OnGround);
    EXPECT_EQ(s.groundUserData, 1u);

    // Прыжок: в воздухе, затем снова на земле.
    tick({}, true);
    for (int i = 0; i < 10; ++i) tick({}, false);
    EXPECT_EQ(world.getCharacterState(ch).groundState, GroundState::InAir);
    for (int i = 0; i < 90; ++i) tick({}, false);
    EXPECT_EQ(world.getCharacterState(ch).groundState, GroundState::OnGround);

    // Идём к ступеньке со скоростью 4 м/с.
    for (int i = 0; i < 90; ++i) tick({4.f, 0.f, 0.f}, false);
    s = world.getCharacterState(ch);
    EXPECT_GT(s.position.x, 4.f);
    EXPECT_NEAR(s.position.y, 0.25f, 0.05f); // стоит на ступеньке

    // Внутреннее кинематическое тело делает персонажа видимым для лучей.
    BodyHandle inner = world.getCharacterInnerBody(ch);
    auto hit = world.raycast(s.position + glm::vec3(0.f, 0.9f, 5.f), {0.f, 0.f, -1.f}, 10.f);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->body, inner);
}
