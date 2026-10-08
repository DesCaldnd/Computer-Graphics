#pragma once

#include <oxwald/physics/physics.hpp>

#include <gtest/gtest.h>

namespace ox::physics::test {

inline constexpr f32 kDt = 1.f / 60.f;

inline PhysicsWorldDesc smallWorld(i32 threads = 2) {
    PhysicsWorldDesc d;
    d.maxBodies = 4096;
    d.maxBodyPairs = 8192;
    d.maxContactConstraints = 8192;
    d.workerThreads = threads;
    return d;
}

// Static floor whose top surface is at y = 0.
inline BodyHandle addFloor(PhysicsWorld& world, f32 restitution = 0.f, f32 halfSize = 50.f) {
    BodyDesc d;
    d.shape = createShape(ShapeDesc::box({halfSize, 0.5f, halfSize}));
    d.position = {0.f, -0.5f, 0.f};
    d.motionType = MotionType::Static;
    d.restitution = restitution;
    d.userData = 0xF100F;
    return world.createBody(d);
}

inline BodyHandle addDynamic(PhysicsWorld& world, const ShapeRef& shape, glm::vec3 position, u64 userData = 0) {
    BodyDesc d;
    d.shape = shape;
    d.position = position;
    d.userData = userData;
    return world.createBody(d);
}

inline void stepFor(PhysicsWorld& world, f32 seconds) {
    int steps = int(seconds / kDt + 0.5f);
    for (int i = 0; i < steps; ++i) {
        world.step(kDt);
    }
}

} // namespace ox::physics::test
