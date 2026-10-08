// Глава 09: соединения (constraints) — дверь на петле, мотор, разрушаемая сварка (docs/guide/09-physics.md).
#include <oxwald/physics/physics.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace ox;
using namespace ox::physics;

TEST(GuidePhysicsConstraints, DoorHingeWithLimitAndMotor) {
    PhysicsWorld world;
    BodyDesc door;
    door.shape = createShape(ShapeDesc::box({0.5f, 1.f, 0.05f}));
    door.position = {0.5f, 1.f, 0.f};
    door.gravityFactor = 0.f; // в примере без гравитации
    door.allowSleeping = false;
    BodyHandle doorBody = world.createBody(door);

    ConstraintDesc hinge;
    hinge.type = ConstraintType::Hinge;
    hinge.bodyA = doorBody;           // bodyB пустой -> крепление к миру
    hinge.pointA = {0.f, 1.f, 0.f};   // мировые координаты петли
    hinge.axis = {0.f, 1.f, 0.f};
    hinge.limitsEnabled = true;
    hinge.limitMin = -1.5f;           // радианы
    hinge.limitMax = 0.f;
    ConstraintHandle h = world.createConstraint(hinge);
    ASSERT_TRUE(world.isValid(h));

    // Мотор по скорости открывает дверь в отрицательную сторону до упора.
    world.setMotor(h, MotorMode::Velocity, -1.f);
    for (int i = 0; i < 180; ++i) world.step(1.f / 60.f);
    EXPECT_NEAR(world.getJointValue(h), -1.5f, 0.05f);

    // Мотор по позиции возвращает дверь в -0.5 рад.
    world.setMotor(h, MotorMode::Position, -0.5f);
    for (int i = 0; i < 240; ++i) world.step(1.f / 60.f);
    EXPECT_NEAR(world.getJointValue(h), -0.5f, 0.05f);
}

TEST(GuidePhysicsConstraints, BreakableWeld) {
    PhysicsWorld world;
    BodyDesc d;
    d.shape = createShape(ShapeDesc::box(glm::vec3(0.5f)));
    d.position = {0.f, 5.f, 0.f};
    d.mass = 10.f;
    BodyHandle lamp = world.createBody(d);

    ConstraintDesc weld;
    weld.type = ConstraintType::Fixed;
    weld.bodyA = lamp;
    weld.pointA = {0.f, 5.5f, 0.f};
    weld.breakForce = 500.f; // держит вес 98 Н, но не рывок
    ConstraintHandle c = world.createConstraint(weld);

    std::vector<ConstraintBrokenEvent> broken;
    world.setConstraintBrokenCallback([&](const ConstraintBrokenEvent& e) { broken.push_back(e); });
    for (int i = 0; i < 60; ++i) world.step(1.f / 60.f);
    EXPECT_TRUE(broken.empty());

    for (int i = 0; i < 10 && broken.empty(); ++i) {
        world.addForce(lamp, {0.f, -1000.f, 0.f}); // дёргаем вниз
        world.step(1.f / 60.f);
    }
    ASSERT_EQ(broken.size(), 1u);
    EXPECT_EQ(broken[0].constraint, c);
    EXPECT_FALSE(world.isConstraintEnabled(c)); // сломанное соединение выключено, но не удалено
}
