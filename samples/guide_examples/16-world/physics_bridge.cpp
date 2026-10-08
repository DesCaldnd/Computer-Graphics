// Guide chapter 16 «Открытый мир»: world → physics bridge (terrain colliders, tree colliders, buoyancy).
#include <oxwald/physics/physics.hpp>
#include <oxwald/world/physics_bridge.hpp> // только там, где слинкован Oxwald::physics
#include <oxwald/world/terrain_gen.hpp>
#include <oxwald/world/water.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::world;

TEST(GuideWorldPhysics, TerrainTileAndTreeColliders) {
    HeightfieldDesc d;
    d.resolution = 129;
    d.worldSize = 128.f;
    d.heightScale = 20.f;
    d.origin = {-64.f, -64.f};
    Heightfield hf(d);
    TerrainNoiseSettings ns;
    ns.fractal.frequency = 1.f / 100.f;
    generateNoise(hf, ns);

    physics::PhysicsWorldDesc wd;
    wd.workerThreads = 0;
    physics::PhysicsWorld physicsWorld(wd);

    // Ландшафт: тайл 64 × 64 квада → heightfield-шейп Jolt.
    const PhysicsHeightfieldTile tile = buildPhysicsTile(hf, 0, 0, 64);
    physics::BodyDesc ground;
    std::string error;
    ground.shape = physics::createShape(toShapeDesc(tile), &error);
    ASSERT_TRUE(ground.shape) << error;
    ground.motionType = physics::MotionType::Static;
    physicsWorld.createBody(ground);

    // Дерево: VegetationCollider задаёт позу тела, toShapeDesc — форму (капсула/цилиндр/бокс).
    VegetationCollider tree;
    tree.position = {-30.f, hf.sampleHeight({-30.f, -30.f}) + 3.4f, -30.f};
    tree.radius = 0.4f;
    tree.halfHeight = 3.f;
    physics::BodyDesc trunk;
    trunk.shape = physics::createShape(toShapeDesc(tree));
    trunk.position = tree.position;
    trunk.rotation = tree.rotation;
    trunk.motionType = physics::MotionType::Static;
    physicsWorld.createBody(trunk);
    physicsWorld.step(1.f / 60.f);

    // Луч сверху попадает в поверхность там же, где sampleHeight (с точностью квантования Jolt).
    const glm::vec2 xz{-50.3f, -40.7f};
    const auto hit = physicsWorld.raycast({xz.x, 100.f, xz.y}, {0.f, -1.f, 0.f}, 200.f);
    ASSERT_TRUE(hit);
    EXPECT_NEAR(hit->point.y, hf.sampleHeight(xz), 0.1f);

    const auto treeHit = physicsWorld.raycast({-30.f, tree.position.y, -20.f}, {0.f, 0.f, -1.f}, 20.f);
    ASSERT_TRUE(treeHit);
    EXPECT_NEAR(treeHit->point.z, -29.6f, 0.05f);
}

TEST(GuideWorldPhysics, CrateFloatsOnWaves) {
    physics::PhysicsWorldDesc wd;
    wd.workerThreads = 0;
    physics::PhysicsWorld physicsWorld(wd);

    // Ящик 1 × 1 × 1 м массой 400 кг: плотность 400 кг/м³ → должен плавать, погрузившись на ~40 %.
    const glm::vec3 half{0.5f};
    physics::BodyDesc crate;
    crate.shape = physics::createShape(physics::ShapeDesc::box(half));
    crate.position = {0.f, 3.f, 0.f};
    crate.mass = 400.f;
    crate.linearDamping = 0.f;
    crate.angularDamping = 0.f;
    const physics::BodyHandle body = physicsWorld.createBody(crate);

    const BuoyancySettings hull = BuoyancySettings::fromBox(half, 4);
    GerstnerWaves waves = GerstnerWaves::fromWind({1.f, 0.f}, /*wind*/ 3.f, 4, /*seed*/ 1);
    waves.baseHeight = 0.f;

    f32 t = 0.f;
    const f32 dt = 1.f / 60.f;
    for (int i = 0; i < 60 * 15; ++i) { // 15 секунд
        const BuoyancyResult r = computeBuoyancy(hull, physicsWorld.getCenterOfMassPosition(body),
                                                 physicsWorld.getRotation(body), physicsWorld.getLinearVelocity(body),
                                                 physicsWorld.getAngularVelocity(body),
                                                 [&](glm::vec2 xz) { return waves.heightAt(xz, t); });
        physicsWorld.addForce(body, r.force); // силы копятся до следующего step()
        physicsWorld.addTorque(body, r.torque);
        physicsWorld.step(dt);
        t += dt;
    }
    const glm::vec3 p = physicsWorld.getCenterOfMassPosition(body);
    const f32 water = waves.heightAt({p.x, p.z}, t);
    // Не утонул и не улетел: держится у поверхности.
    EXPECT_GT(p.y, water - 0.5f);
    EXPECT_LT(p.y, water + 0.5f);
}
