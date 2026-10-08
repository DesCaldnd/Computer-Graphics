// Глава 09: триггеры и события контактов (docs/guide/09-physics.md).
#include <oxwald/physics/physics.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace ox;
using namespace ox::physics;

TEST(GuidePhysicsEvents, TriggerZone) {
    PhysicsWorld world;

    BodyDesc zone;
    zone.shape = createShape(ShapeDesc::box({2.f, 1.f, 2.f}));
    zone.motionType = MotionType::Static;
    zone.isSensor = true; // слой по умолчанию станет layers::Trigger
    zone.userData = 500;
    BodyHandle trigger = world.createBody(zone);

    BodyDesc ball;
    ball.shape = createShape(ShapeDesc::sphere(0.25f));
    ball.position = {0.f, 3.f, 0.f};
    ball.userData = 42;
    world.createBody(ball);

    int entered = 0, exited = 0;
    world.setTriggerCallback([&](const TriggerEvent& e) {
        if (e.type == TriggerEventType::Enter) {
            ++entered;
            EXPECT_EQ(e.trigger, trigger);
            EXPECT_EQ(e.otherUserData, 42u);
        } else if (e.type == TriggerEventType::Exit) {
            ++exited;
        }
    });
    for (int i = 0; i < 120; ++i) {
        world.step(1.f / 60.f); // шар пролетает зону насквозь (пола нет)
    }
    EXPECT_EQ(entered, 1);
    EXPECT_EQ(exited, 1);
}

TEST(GuidePhysicsEvents, ContactEvents) {
    PhysicsWorld world;
    BodyDesc floor;
    floor.shape = createShape(ShapeDesc::box({10.f, 0.5f, 10.f}));
    floor.position = {0.f, -0.5f, 0.f};
    floor.motionType = MotionType::Static;
    world.createBody(floor);

    BodyDesc rock;
    rock.shape = createShape(ShapeDesc::sphere(0.5f));
    rock.position = {0.f, 5.f, 0.f};
    rock.mass = 10.f;
    BodyHandle body = world.createBody(rock);

    int begins = 0;
    f32 impactImpulse = 0.f;
    for (int i = 0; i < 120; ++i) {
        world.step(1.f / 60.f);
        // События валидны до следующего step().
        for (const ContactEvent& c : world.contactEvents()) {
            if (c.type == ContactEventType::Begin) {
                ++begins;
                impactImpulse = c.normalImpulse; // оценка импульса удара, Н·с
                EXPECT_GT(c.pointCount, 0u);
                EXPECT_TRUE(c.bodyA == body || c.bodyB == body);
            }
        }
    }
    EXPECT_EQ(begins, 1);
    // ~ m * v = 10 кг * sqrt(2 * 9.81 * 4.5) ≈ 94 Н·с
    EXPECT_GT(impactImpulse, 50.f);

    // Удаление тела даёт End на следующем шаге.
    world.destroyBody(body);
    world.step(1.f / 60.f);
    bool ended = false;
    for (const ContactEvent& c : world.contactEvents()) {
        ended |= c.type == ContactEventType::End;
    }
    EXPECT_TRUE(ended);
}
