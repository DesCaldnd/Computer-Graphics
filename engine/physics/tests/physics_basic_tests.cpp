#include "test_helpers.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

using namespace ox;
using namespace ox::physics;
using namespace ox::physics::test;

TEST(PhysicsWorld, InitReportsBackend) {
    PhysicsWorld world(smallWorld());
    EXPECT_NE(std::string_view(backendInfo()).find("Jolt 5."), std::string_view::npos);
    EXPECT_EQ(world.bodyCount(), 0u);
    EXPECT_EQ(world.layers().name(layers::Trigger), "Trigger");
}

TEST(PhysicsWorld, FallingBoxRestsOnFloor) {
    PhysicsWorld world(smallWorld());
    addFloor(world);
    BodyHandle box = addDynamic(world, createShape(ShapeDesc::box({0.5f, 0.5f, 0.5f})), {0.f, 5.f, 0.f});
    ASSERT_TRUE(world.isValid(box));
    stepFor(world, 3.f);
    Transform t = world.getTransform(box);
    EXPECT_NEAR(t.position.y, 0.5f, 0.025f); // within Jolt's penetration slop (0.02)
    EXPECT_NEAR(t.position.x, 0.f, 0.01f);
    EXPECT_LT(glm::length(world.getLinearVelocity(box)), 0.05f);
    // Should stay upright: rotation ≈ identity (up to a yaw).
    glm::vec3 up = t.rotation * glm::vec3(0, 1, 0);
    EXPECT_GT(up.y, 0.999f);
    stepFor(world, 2.f);
    EXPECT_FALSE(world.isActive(box)) << "resting box should fall asleep";
}

TEST(PhysicsWorld, SphereBounceRespectsRestitution) {
    auto bounceHeight = [](f32 restitution) {
        PhysicsWorld world(smallWorld());
        addFloor(world, restitution);
        BodyDesc d;
        d.shape = createShape(ShapeDesc::sphere(0.5f));
        d.position = {0.f, 5.f, 0.f};
        d.restitution = restitution;
        d.linearDamping = 0.f;
        BodyHandle s = world.createBody(d);
        bool bounced = false;
        f32 maxAfter = 0.f;
        for (int i = 0; i < 240; ++i) {
            world.step(kDt);
            f32 vy = world.getLinearVelocity(s).y;
            if (!bounced && vy > 0.5f) {
                bounced = true;
            }
            if (bounced) {
                maxAfter = std::max(maxAfter, world.getPosition(s).y);
            }
        }
        return maxAfter;
    };
    // Drop height above contact = 4.5 m; e=0.8 → ~0.64*4.5 = 2.9 m above the contact height.
    f32 bouncy = bounceHeight(0.8f);
    EXPECT_GT(bouncy, 0.5f + 2.3f);
    EXPECT_LT(bouncy, 0.5f + 3.5f);
    f32 dead = bounceHeight(0.f);
    EXPECT_LT(dead, 0.7f);
}

TEST(PhysicsWorld, BodyPropertiesRoundTrip) {
    PhysicsWorld world(smallWorld());
    BodyDesc d;
    d.shape = createShape(ShapeDesc::box({0.5f, 0.5f, 0.5f}));
    d.mass = 12.f;
    d.friction = 0.7f;
    d.restitution = 0.3f;
    d.gravityFactor = 0.f;
    d.userData = 1234;
    d.lockAxes = lock::AllRotation;
    BodyHandle b = world.createBody(d);
    EXPECT_NEAR(world.getMass(b), 12.f, 1e-3f);
    EXPECT_FLOAT_EQ(world.getFriction(b), 0.7f);
    EXPECT_FLOAT_EQ(world.getRestitution(b), 0.3f);
    EXPECT_EQ(world.getUserData(b), 1234u);
    EXPECT_EQ(world.getMotionType(b), MotionType::Dynamic);
    EXPECT_EQ(world.getLayer(b), layers::Dynamic);

    // Rotation locked: an angular impulse does nothing; gravityFactor 0 keeps it in place.
    world.addAngularImpulse(b, {10.f, 10.f, 10.f});
    world.addImpulse(b, {12.f, 0.f, 0.f}); // 1 m/s
    stepFor(world, 1.f);
    EXPECT_LT(glm::length(world.getAngularVelocity(b)), 1e-4f);
    EXPECT_NEAR(world.getPosition(b).x, 1.f, 0.1f);
    EXPECT_NEAR(world.getPosition(b).y, 0.f, 1e-4f);

    world.setTransform(b, {3.f, 4.f, 5.f}, glm::angleAxis(0.5f, glm::vec3(0, 1, 0)));
    Transform t = world.getTransform(b);
    EXPECT_NEAR(glm::distance(t.position, glm::vec3(3, 4, 5)), 0.f, 1e-5f);
    EXPECT_NEAR(std::abs(glm::dot(t.rotation, glm::angleAxis(0.5f, glm::vec3(0, 1, 0)))), 1.f, 1e-5f);

    world.destroyBody(b);
    EXPECT_FALSE(world.isValid(b));
}

TEST(PhysicsWorld, KinematicMoveKinematicPushesDynamic) {
    PhysicsWorld world(smallWorld());
    addFloor(world);
    BodyDesc k;
    k.shape = createShape(ShapeDesc::box({0.5f, 0.5f, 0.5f}));
    k.motionType = MotionType::Kinematic;
    k.position = {0.f, 0.5f, 0.f};
    BodyHandle pusher = world.createBody(k);
    BodyHandle box = addDynamic(world, createShape(ShapeDesc::box({0.5f, 0.5f, 0.5f})), {1.2f, 0.5f, 0.f});
    for (int i = 1; i <= 120; ++i) {
        world.moveKinematic(pusher, {i * kDt * 1.5f, 0.5f, 0.f}, glm::quat(1, 0, 0, 0), kDt);
        world.step(kDt);
    }
    EXPECT_NEAR(world.getPosition(pusher).x, 3.f, 0.05f);
    EXPECT_GT(world.getPosition(box).x, 3.5f);
}

TEST(PhysicsWorld, RaycastHitsMissesAndFiltersLayers) {
    PhysicsWorldDesc wd = smallWorld();
    ObjectLayer props = *wd.layers.addLayer("Props");
    PhysicsWorld world(wd);
    EXPECT_EQ(world.layers().find("Props"), props);

    BodyHandle floor = addFloor(world);
    BodyDesc d;
    d.shape = createShape(ShapeDesc::box({0.5f, 0.5f, 0.5f}));
    d.motionType = MotionType::Static;
    d.position = {0.f, 2.f, 0.f};
    d.layer = props;
    d.userData = 77;
    BodyHandle prop = world.createBody(d);

    auto hit = world.raycast({0.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 100.f);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->body, prop);
    EXPECT_EQ(hit->userData, 77u);
    EXPECT_NEAR(hit->distance, 7.5f, 1e-3f);
    EXPECT_NEAR(hit->point.y, 2.5f, 1e-3f);
    EXPECT_NEAR(hit->normal.y, 1.f, 1e-3f);

    QueryFilter noProps;
    noProps.layerMask = kAllLayers & ~layerBit(props);
    hit = world.raycast({0.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 100.f, noProps);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->body, floor);
    EXPECT_NEAR(hit->distance, 10.f, 1e-3f);

    BodyHandle ignore[] = {prop};
    QueryFilter ignoring;
    ignoring.ignoreBodies = ignore;
    hit = world.raycast({0.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 100.f, ignoring);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->body, floor);

    QueryFilter onlyProps;
    onlyProps.layerMask = layerBit(props);
    EXPECT_FALSE(world.raycast({5.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 100.f, onlyProps));
    EXPECT_FALSE(world.raycast({0.f, 10.f, 0.f}, {0.f, 1.f, 0.f}, 100.f));   // pointing away
    EXPECT_FALSE(world.raycast({0.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 5.f));    // too short

    auto all = world.raycastAll({0.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 100.f);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].body, prop);
    EXPECT_EQ(all[1].body, floor);
    EXPECT_LT(all[0].distance, all[1].distance);
}

TEST(PhysicsWorld, ShapeCastOverlapAndClosestPoint) {
    PhysicsWorld world(smallWorld());
    BodyHandle floor = addFloor(world);
    BodyDesc d;
    d.shape = createShape(ShapeDesc::box({0.5f, 0.5f, 0.5f}));
    d.motionType = MotionType::Static;
    d.position = {5.f, 0.5f, 0.f};
    BodyHandle crate = world.createBody(d);

    auto sweep = world.sphereCast({0.f, 5.f, 0.f}, 0.5f, {0.f, -1.f, 0.f}, 10.f);
    ASSERT_TRUE(sweep);
    EXPECT_EQ(sweep->body, floor);
    EXPECT_NEAR(sweep->distance, 4.5f, 0.02f);
    EXPECT_NEAR(sweep->normal.y, 1.f, 0.01f);

    auto boxSweep = world.boxCast({0.f, 0.5f, 0.f}, glm::vec3(0.25f), glm::quat(1, 0, 0, 0), {1.f, 0.f, 0.f}, 10.f,
                                  QueryFilter{.layerMask = kAllLayers, .ignoreBodies = std::span(&floor, 1)});
    ASSERT_TRUE(boxSweep);
    EXPECT_EQ(boxSweep->body, crate);
    EXPECT_NEAR(boxSweep->distance, 4.25f, 0.02f);
    EXPECT_NEAR(boxSweep->normal.x, -1.f, 0.01f);

    auto capsuleSweep = world.capsuleCast({5.f, 5.f, 0.f}, 0.5f, 0.25f, glm::quat(1, 0, 0, 0), {0, -1, 0}, 10.f);
    ASSERT_TRUE(capsuleSweep);
    EXPECT_EQ(capsuleSweep->body, crate);
    EXPECT_NEAR(capsuleSweep->distance, 5.f - 0.75f - 1.f, 0.02f);

    auto near = world.overlapSphere({5.f, 1.2f, 0.f}, 0.5f);
    ASSERT_EQ(near.size(), 1u);
    EXPECT_EQ(near[0], crate);
    auto both = world.overlapBox({5.f, 0.5f, 0.f}, glm::vec3(1.f), glm::quat(1, 0, 0, 0));
    EXPECT_EQ(both.size(), 2u);
    auto aabb = world.overlapAabb({{4.f, 0.2f, -1.f}, {6.f, 3.f, 1.f}});
    ASSERT_EQ(aabb.size(), 1u); // floor AABB ends at y = 0
    EXPECT_EQ(aabb[0], crate);
    EXPECT_EQ(world.overlapAabb({{4.f, -0.2f, -1.f}, {6.f, 3.f, 1.f}}).size(), 2u);

    auto cp = world.closestPoint({5.f, 3.f, 0.f}, 5.f, QueryFilter{.ignoreBodies = std::span(&floor, 1)});
    ASSERT_TRUE(cp);
    EXPECT_EQ(cp->body, crate);
    EXPECT_NEAR(cp->distance, 2.f, 0.02f);
    EXPECT_NEAR(cp->point.y, 1.f, 0.02f);
    EXPECT_FALSE(world.closestPoint({5.f, 30.f, 0.f}, 5.f));
}

TEST(PhysicsWorld, TriggerEnterStayExit) {
    PhysicsWorld world(smallWorld());
    BodyDesc t;
    t.shape = createShape(ShapeDesc::box({2.f, 1.f, 2.f}));
    t.motionType = MotionType::Static;
    t.isSensor = true;
    t.userData = 500;
    BodyHandle trigger = world.createBody(t);
    EXPECT_TRUE(world.isSensor(trigger));
    EXPECT_EQ(world.getLayer(trigger), layers::Trigger);

    BodyHandle ball = addDynamic(world, createShape(ShapeDesc::sphere(0.25f)), {0.f, 3.f, 0.f}, 42);

    int enter = 0, stay = 0, exit = 0;
    std::vector<TriggerEvent> fromCallback;
    world.setTriggerCallback([&](const TriggerEvent& e) { fromCallback.push_back(e); });
    for (int i = 0; i < 120; ++i) {
        world.step(kDt);
        for (const TriggerEvent& e : world.triggerEvents()) {
            EXPECT_EQ(e.trigger, trigger);
            EXPECT_EQ(e.other, ball);
            EXPECT_EQ(e.triggerUserData, 500u);
            EXPECT_EQ(e.otherUserData, 42u);
            if (e.type == TriggerEventType::Enter) {
                EXPECT_EQ(stay, 0);
                EXPECT_EQ(exit, 0);
                ++enter;
            } else if (e.type == TriggerEventType::Stay) {
                EXPECT_EQ(enter, 1);
                ++stay;
            } else {
                EXPECT_EQ(enter, 1);
                ++exit;
            }
        }
    }
    EXPECT_EQ(enter, 1);
    EXPECT_GT(stay, 3);
    EXPECT_EQ(exit, 1);
    EXPECT_EQ(fromCallback.size(), size_t(enter + stay + exit));
    // Sensors don't block: the ball fell through.
    EXPECT_LT(world.getPosition(ball).y, -2.f);
    // Ignored by default queries, found with includeSensors.
    EXPECT_FALSE(world.raycast({0.f, 10.f, 0.f}, {0, -1, 0}, 20.f));
    EXPECT_TRUE(world.raycast({0.f, 10.f, 0.f}, {0, -1, 0}, 20.f, QueryFilter{.includeSensors = true}));
}

TEST(PhysicsWorld, TriggerExitOnDestroy) {
    PhysicsWorld world(smallWorld());
    BodyDesc t;
    t.shape = createShape(ShapeDesc::box({2.f, 2.f, 2.f}));
    t.motionType = MotionType::Static;
    t.isSensor = true;
    BodyHandle trigger = world.createBody(t);
    BodyDesc b;
    b.shape = createShape(ShapeDesc::sphere(0.25f));
    b.gravityFactor = 0.f;
    BodyHandle ball = world.createBody(b);
    world.step(kDt);
    ASSERT_EQ(world.triggerEvents().size(), 1u);
    EXPECT_EQ(world.triggerEvents()[0].type, TriggerEventType::Enter);
    world.destroyBody(ball);
    world.step(kDt);
    ASSERT_EQ(world.triggerEvents().size(), 1u);
    EXPECT_EQ(world.triggerEvents()[0].type, TriggerEventType::Exit);
    EXPECT_EQ(world.triggerEvents()[0].trigger, trigger);
    world.step(kDt);
    EXPECT_TRUE(world.triggerEvents().empty());
}

TEST(PhysicsWorld, ContactEventsBeginPersistEnd) {
    PhysicsWorld world(smallWorld());
    BodyHandle floor = addFloor(world);
    BodyHandle box = addDynamic(world, createShape(ShapeDesc::box({0.5f, 0.5f, 0.5f})), {0.f, 2.f, 0.f}, 9);

    std::vector<ContactEvent> all;
    world.setContactCallback([&](const ContactEvent& e) { all.push_back(e); });
    stepFor(world, 1.5f);

    auto count = [&](ContactEventType t) {
        return std::count_if(all.begin(), all.end(), [&](const ContactEvent& e) { return e.type == t; });
    };
    ASSERT_EQ(count(ContactEventType::Begin), 1);
    EXPECT_GT(count(ContactEventType::Persist), 5);
    EXPECT_EQ(count(ContactEventType::End), 0);

    const ContactEvent& begin = all.front();
    EXPECT_EQ(begin.type, ContactEventType::Begin);
    // Floor was created first → smaller id → bodyA.
    EXPECT_EQ(begin.bodyA, floor);
    EXPECT_EQ(begin.bodyB, box);
    EXPECT_EQ(begin.userDataB, 9u);
    EXPECT_NEAR(begin.normal.y, 1.f, 0.05f) << "normal points from A (floor) to B (box)";
    EXPECT_GE(begin.pointCount, 1u);
    EXPECT_NEAR(begin.points[0].y, 0.f, 0.1f);
    // m * v at impact ≈ 1000 kg * sqrt(2 g 1.5) ≈ 5400 N·s (restitution 0, single-step estimate).
    EXPECT_GT(begin.normalImpulse, 1000.f);

    all.clear();
    world.setLinearVelocity(box, {0.f, 10.f, 0.f});
    stepFor(world, 0.3f);
    EXPECT_EQ(count(ContactEventType::End), 1);

    // Falling asleep on the floor keeps the contact (no End); destroying the body emits End on the next step.
    all.clear();
    stepFor(world, 3.f);
    EXPECT_FALSE(world.isActive(box));
    EXPECT_EQ(count(ContactEventType::Begin), 1);
    EXPECT_EQ(count(ContactEventType::End), 0);
    all.clear();
    world.destroyBody(box);
    world.step(kDt);
    ASSERT_EQ(count(ContactEventType::End), 1);
    EXPECT_EQ(all.back().bodyB, box);
}

TEST(PhysicsWorld, ContactReportingCanBeDisabled) {
    PhysicsWorld world(smallWorld());
    BodyDesc f;
    f.shape = createShape(ShapeDesc::box({10.f, 0.5f, 10.f}));
    f.position = {0.f, -0.5f, 0.f};
    f.motionType = MotionType::Static;
    f.reportContacts = false;
    world.createBody(f);
    BodyDesc b;
    b.shape = createShape(ShapeDesc::sphere(0.5f));
    b.position = {0.f, 1.f, 0.f};
    b.reportContacts = false;
    world.createBody(b);
    size_t events = 0;
    for (int i = 0; i < 60; ++i) {
        world.step(kDt);
        events += world.contactEvents().size();
    }
    EXPECT_EQ(events, 0u);
}

TEST(PhysicsWorld, CompoundHullMeshAndHeightFieldCollide) {
    PhysicsWorld world(smallWorld());

    // Height field terrain: gentle sine bumps around y = 0, 33x33 samples over 32 m.
    constexpr u32 n = 33;
    std::vector<f32> heights(n * n);
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            heights[z * n + x] = 0.2f * std::sin(f32(x) * 0.4f) * std::cos(f32(z) * 0.4f);
        }
    }
    BodyDesc terrain;
    terrain.shape = createShape(ShapeDesc::heightField(heights, n, {-16.f, 0.f, -16.f}, {1.f, 1.f, 1.f}));
    ASSERT_TRUE(terrain.shape);
    EXPECT_EQ(terrain.shape.type(), ShapeType::HeightField);
    terrain.motionType = MotionType::Static;
    world.createBody(terrain);

    // Triangle mesh platform (2 triangles, CCW from above) at y = 3 over x,z in [20, 30].
    BodyDesc mesh;
    mesh.shape = createShape(ShapeDesc::triangleMesh(
        {{20.f, 3.f, 20.f}, {20.f, 3.f, 30.f}, {30.f, 3.f, 30.f}, {30.f, 3.f, 20.f}}, {0, 1, 2, 0, 2, 3}));
    ASSERT_TRUE(mesh.shape);
    EXPECT_EQ(mesh.shape.type(), ShapeType::TriangleMesh);
    mesh.motionType = MotionType::Static;
    world.createBody(mesh);

    // Compound "dumbbell": two spheres + a bar.
    ShapeDesc dumbbell = ShapeDesc::compound({
        {ShapeDesc::sphere(0.3f), {-0.6f, 0.f, 0.f}},
        {ShapeDesc::sphere(0.3f), {0.6f, 0.f, 0.f}},
        {ShapeDesc::box({0.6f, 0.08f, 0.08f}), {0.f, 0.f, 0.f}},
    });
    ShapeRef compoundShape = createShape(dumbbell);
    ASSERT_TRUE(compoundShape);
    EXPECT_EQ(compoundShape.type(), ShapeType::Compound);
    BodyHandle compound = addDynamic(world, compoundShape, {0.f, 3.f, 0.f});

    // Octahedron convex hull dropped on the mesh platform.
    ShapeRef hull = createShape(ShapeDesc::convexHull(
        {{0.5f, 0, 0}, {-0.5f, 0, 0}, {0, 0.5f, 0}, {0, -0.5f, 0}, {0, 0, 0.5f}, {0, 0, -0.5f}}));
    ASSERT_TRUE(hull);
    EXPECT_EQ(hull.type(), ShapeType::ConvexHull);
    BodyHandle hullBody = addDynamic(world, hull, {25.f, 6.f, 25.f});

    // Scaled + center-of-mass-offset cylinder on the terrain.
    ShapeDesc cyl = ShapeDesc::cylinder(0.5f, 0.3f);
    cyl.scale = glm::vec3(2.f);
    cyl.centerOfMassOffset = {0.f, -0.3f, 0.f};
    ShapeRef cylShape = createShape(cyl);
    ASSERT_TRUE(cylShape);
    EXPECT_NEAR(cylShape.centerOfMass().y, -0.3f, 1e-4f);
    BodyHandle cylBody = addDynamic(world, cylShape, {5.f, 4.f, 5.f});

    stepFor(world, 4.f);

    glm::vec3 c = world.getPosition(compound);
    EXPECT_GT(c.y, 0.05f);
    EXPECT_LT(c.y, 0.7f);
    glm::vec3 h = world.getPosition(hullBody);
    EXPECT_GT(h.y, 3.f + 0.3f) << "hull should rest on the mesh platform";
    EXPECT_LT(h.y, 3.f + 0.6f);
    glm::vec3 cy = world.getPosition(cylBody);
    EXPECT_GT(cy.y, -0.3f);
    EXPECT_LT(cy.y, 1.5f);
}

TEST(PhysicsWorld, ShapeCacheDeduplicates) {
    PhysicsWorld world(smallWorld());
    ShapeCache& cache = world.shapeCache();
    ShapeRef a = cache.getOrCreate(ShapeDesc::box({1.f, 2.f, 3.f}));
    ShapeRef b = cache.getOrCreate(ShapeDesc::box({1.f, 2.f, 3.f}));
    ShapeRef c = cache.getOrCreate(ShapeDesc::box({1.f, 2.f, 3.5f}));
    EXPECT_TRUE(a);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(hashShapeDesc(ShapeDesc::sphere(1.f)), hashShapeDesc(ShapeDesc::sphere(1.f)));
    EXPECT_NE(hashShapeDesc(ShapeDesc::sphere(1.f)), hashShapeDesc(ShapeDesc::sphere(1.01f)));
    EXPECT_EQ(cache.size(), 2u);
    c = {};
    EXPECT_EQ(cache.collectGarbage(), 1u);
    EXPECT_EQ(cache.size(), 1u);

    std::string error;
    EXPECT_FALSE(createShape(ShapeDesc::convexHull({{0, 0, 0}}), &error));
    EXPECT_FALSE(error.empty());
    ShapeDesc badScale = ShapeDesc::sphere(1.f);
    badScale.scale = {1.f, 2.f, 1.f};
    EXPECT_FALSE(createShape(badScale));
}

TEST(PhysicsWorld, SnapshotRestoreIsDeterministic) {
    PhysicsWorld world(smallWorld(3));
    addFloor(world, 0.2f);
    ShapeRef box = createShape(ShapeDesc::box({0.4f, 0.4f, 0.4f}));
    ShapeRef ball = createShape(ShapeDesc::sphere(0.3f));
    std::vector<BodyHandle> bodies;
    for (int i = 0; i < 40; ++i) {
        glm::vec3 p{f32(i % 4) * 0.5f - 0.75f, 1.f + f32(i) * 0.6f, f32((i / 4) % 3) * 0.45f - 0.45f};
        bodies.push_back(addDynamic(world, (i % 3 == 0) ? ball : box, p, u64(i)));
    }
    stepFor(world, 1.f);
    std::vector<u8> snapshot = world.saveState();
    ASSERT_FALSE(snapshot.empty());

    auto run = [&](std::vector<Transform>& out, size_t& events) {
        events = 0;
        for (int i = 0; i < 90; ++i) {
            world.step(kDt);
            events += world.contactEvents().size();
        }
        out.clear();
        for (BodyHandle b : bodies) {
            out.push_back(world.getTransform(b));
        }
    };
    std::vector<Transform> a, b;
    size_t eventsA = 0, eventsB = 0;
    run(a, eventsA);
    u64 stepsAfterA = world.stepCount();
    ASSERT_TRUE(world.restoreState(snapshot));
    EXPECT_EQ(world.saveState(), snapshot) << "restore → save must round-trip byte-exactly";
    run(b, eventsB);
    EXPECT_EQ(world.stepCount(), stepsAfterA);
    EXPECT_EQ(eventsA, eventsB);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].position, b[i].position) << "body " << i;
        EXPECT_EQ(a[i].rotation, b[i].rotation) << "body " << i;
    }

    std::vector<u8> garbage(16, 0xAB);
    EXPECT_FALSE(world.restoreState(garbage));
}

TEST(Interpolation, FixedStepperAndInterpolator) {
    FixedStepper stepper(60.f, 4);
    EXPECT_EQ(stepper.advance(1.f / 120.f), 0u);
    EXPECT_NEAR(stepper.alpha(), 0.5f, 1e-4f);
    EXPECT_EQ(stepper.advance(1.f / 120.f), 1u);
    EXPECT_NEAR(stepper.alpha(), 0.f, 1e-3f);
    EXPECT_EQ(stepper.advance(1.f), 4u); // clamped, excess dropped
    EXPECT_LT(stepper.alpha(), 1.f);

    PhysicsWorld world(smallWorld());
    BodyDesc d;
    d.shape = createShape(ShapeDesc::sphere(0.5f));
    d.gravityFactor = 0.f;
    d.linearDamping = 0.f;
    d.angularDamping = 0.f;
    d.linearVelocity = {6.f, 0.f, 0.f};
    d.angularVelocity = {0.f, 3.f, 0.f};
    BodyHandle b = world.createBody(d);

    TransformInterpolator interp;
    interp.track(b);
    interp.capture(world);
    world.step(kDt);
    interp.capture(world);
    const Transform* prev = interp.previous(b);
    const Transform* cur = interp.current(b);
    ASSERT_TRUE(prev && cur);
    EXPECT_NEAR(cur->position.x - prev->position.x, 0.1f, 1e-3f);

    Transform mid = interp.get(b, 0.5f);
    EXPECT_NEAR(mid.position.x, 0.05f, 1e-3f);
    f32 angle = glm::angle(mid.rotation);
    EXPECT_NEAR(angle, 0.025f, 1e-3f);
    Transform end = interp.get(b, 1.f);
    EXPECT_EQ(end.position, cur->position);

    // Shortest path across the quaternion double cover.
    Transform qa{{}, glm::quat(1, 0, 0, 0)};
    Transform qb{{}, -glm::angleAxis(0.2f, glm::vec3(0, 0, 1))};
    EXPECT_NEAR(glm::angle(TransformInterpolator::interpolate(qa, qb, 0.5f).rotation), 0.1f, 1e-3f);

    world.destroyBody(b);
    interp.capture(world);
    EXPECT_FALSE(interp.tracked(b));
}

namespace {
class ThreadPoolExecutor final : public IPhysicsJobExecutor {
public:
    explicit ThreadPoolExecutor(u32 threads) {
        for (u32 i = 0; i < threads; ++i) {
            m_threads.emplace_back([this] { worker(); });
        }
    }
    ~ThreadPoolExecutor() override {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads) {
            t.join();
        }
    }
    u32 maxConcurrency() const override { return u32(m_threads.size()) + 1; }
    void submit(void (*fn)(void*), void* ctx) override {
        {
            std::lock_guard lock(m_mutex);
            m_queue.emplace_back(fn, ctx);
        }
        ++submitted;
        m_cv.notify_one();
    }
    std::atomic<u32> submitted{0};

private:
    void worker() {
        for (;;) {
            std::pair<void (*)(void*), void*> task;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
                if (m_stop && m_queue.empty()) {
                    return;
                }
                task = m_queue.front();
                m_queue.pop_front();
            }
            task.first(task.second);
        }
    }
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<void (*)(void*), void*>> m_queue;
    std::vector<std::thread> m_threads;
    bool m_stop = false;
};
} // namespace

TEST(PhysicsWorld, CustomJobExecutor) {
    ThreadPoolExecutor executor(3);
    {
        PhysicsWorldDesc d = smallWorld();
        d.jobExecutor = &executor;
        PhysicsWorld world(d);
        addFloor(world);
        BodyHandle box = addDynamic(world, createShape(ShapeDesc::box(glm::vec3(0.5f))), {0.f, 3.f, 0.f});
        stepFor(world, 2.f);
        EXPECT_NEAR(world.getPosition(box).y, 0.5f, 0.025f);
    }
    EXPECT_GT(executor.submitted.load(), 0u);
}

TEST(PhysicsWorld, StressThousandBodies) {
    PhysicsWorldDesc d = smallWorld(-1);
    d.maxBodies = 2048;
    d.maxBodyPairs = 65536;
    d.maxContactConstraints = 32768;
    PhysicsWorld world(d);
    addFloor(world, 0.f, 100.f);
    ShapeRef box = createShape(ShapeDesc::box(glm::vec3(0.4f)));
    ShapeRef ball = createShape(ShapeDesc::sphere(0.4f));
    for (int i = 0; i < 1000; ++i) {
        int x = i % 10, z = (i / 10) % 10, y = i / 100;
        addDynamic(world, (i & 1) ? box : ball, {f32(x) * 1.f - 5.f, 1.f + f32(y) * 1.f, f32(z) * 1.f - 5.f});
    }
    world.optimizeBroadPhase();
    EXPECT_EQ(world.bodyCount(), 1001u);

    using Clock = std::chrono::steady_clock;
    constexpr int kSteps = 120;
    double worst = 0.0;
    auto start = Clock::now();
    for (int i = 0; i < kSteps; ++i) {
        auto s = Clock::now();
        world.step(kDt);
        worst = std::max(worst, std::chrono::duration<double, std::milli>(Clock::now() - s).count());
    }
    double total = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("[ stress   ] 1000 bodies, %d steps: avg %.3f ms/step, worst %.3f ms, active %u\n", kSteps,
                total / kSteps, worst, world.activeBodyCount());
    // Generous bound — only catches pathological regressions (e.g. accidental O(n^2)).
    EXPECT_LT(total / kSteps, 100.0);
}

namespace {
struct CountingSink final : PhysicsDebugSink {
    size_t lines = 0, triangles = 0, texts = 0;
    void line(const glm::vec3&, const glm::vec3&, const Color&) override { ++lines; }
    void triangle(const glm::vec3&, const glm::vec3&, const glm::vec3&, const Color&) override { ++triangles; }
    void text(const glm::vec3&, std::string_view, const Color&) override { ++texts; }
};
} // namespace

TEST(PhysicsWorld, DebugDrawEmitsColliderWireframes) {
    PhysicsWorld world(smallWorld());
    auto linesFor = [&](const ShapeDesc& desc, DebugDrawOptions opt = {}) {
        BodyDesc d;
        d.shape = createShape(desc);
        d.motionType = MotionType::Static;
        BodyHandle b = world.createBody(d);
        CountingSink sink;
        world.debugDraw(sink, opt);
        world.destroyBody(b);
        return sink;
    };
    EXPECT_EQ(linesFor(ShapeDesc::box(glm::vec3(1.f))).lines, 12u);
    EXPECT_EQ(linesFor(ShapeDesc::sphere(1.f)).lines, 3u * 24u);
    EXPECT_GT(linesFor(ShapeDesc::capsule(0.5f, 0.3f)).lines, 40u);
    EXPECT_GT(linesFor(ShapeDesc::cylinder(0.5f, 0.3f)).lines, 40u);
    EXPECT_GE(linesFor(ShapeDesc::convexHull({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}})).lines, 12u);
    EXPECT_EQ(linesFor(ShapeDesc::triangleMesh({{0, 0, 0}, {0, 0, 1}, {1, 0, 1}}, {0, 1, 2})).lines, 3u);
    DebugDrawOptions filled;
    filled.filledTriangles = true;
    EXPECT_EQ(linesFor(ShapeDesc::triangleMesh({{0, 0, 0}, {0, 0, 1}, {1, 0, 1}}, {0, 1, 2}), filled).triangles, 1u);
    DebugDrawOptions extras;
    extras.aabbs = true;
    extras.labels = true;
    CountingSink s = linesFor(ShapeDesc::box(glm::vec3(1.f)), extras);
    EXPECT_EQ(s.lines, 24u);
    EXPECT_EQ(s.texts, 1u);
    EXPECT_EQ(linesFor(ShapeDesc::compound({{ShapeDesc::box(glm::vec3(0.5f)), {1, 0, 0}},
                                            {ShapeDesc::box(glm::vec3(0.5f)), {-1, 0, 0}}}))
                  .lines,
              24u);
}
