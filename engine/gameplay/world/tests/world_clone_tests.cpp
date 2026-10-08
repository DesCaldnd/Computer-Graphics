#include "world_test_utils.hpp"

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

TEST(GameplayWorld, PlayInEditorCloneLeavesEditWorldUntouched) {
    WorldHarness h;
    Entity terrain = h.terrain();
    Entity sun = h.world.create("Sun");
    sun.add<LightComponent>().type = LightType::Directional;
    Entity env = h.world.create("Env");
    auto& tod = env.add<TimeOfDayComponent>();
    tod.localHours = 10.0;
    tod.timeScale = 3600.0;
    Entity sea = h.world.create("Sea");
    sea.setPosition({0.f, -50.f, 0.f});
    sea.add<WaterComponent>();

    h.start(false);
    h.run(0.2);
    auto& rt = h.worldRt();
    const auto groundEdit = rt.terrainHeight({0.f, 0.f});
    ASSERT_TRUE(groundEdit.has_value());
    Entity crate = h.box("Crate", {0.f, *groundEdit + 5.f, 0.f});
    h.run(0.2);
    const glm::quat sunRotation = sun.worldRotation();
    const f32 sunLux = sun.get<LightComponent>().intensity;
    EXPECT_FLOAT_EQ(crate.worldPosition().y, *groundEdit + 5.f);
    EXPECT_EQ(rt.terrainBodyCount(terrain), 0u);

    // Play in editor: simulate a clone.
    std::unique_ptr<World> play = h.world.clone();
    h.scheduler.attach(*play, h.services);
    h.active = play.get();
    h.scheduler.setPlaying(true);
    h.run(2.0);
    const Entity playTerrain = play->find(terrain.uuid());
    const Entity playCrate = play->find(crate.uuid());
    const Entity playEnv = play->find(env.uuid());
    ASSERT_TRUE(playTerrain.valid() && playCrate.valid() && playEnv.valid());
    EXPECT_EQ(rt.world(), play.get());
    EXPECT_EQ(rt.terrainBodyCount(playTerrain), 4u);
    EXPECT_NEAR(playCrate.worldPosition().y, *rt.terrainHeight({playCrate.worldPosition().x, playCrate.worldPosition().z}) + 0.5f, 0.35f);
    EXPECT_GT(playEnv.get<TimeOfDayComponent>().localHours, 11.5); // two game hours later
    // Edit-world handles are not resolved by the runtime while the clone is attached.
    EXPECT_EQ(rt.heightfield(terrain), nullptr);

    // Stop: back to the edit world, untouched.
    h.scheduler.setPlaying(false);
    h.scheduler.attach(h.world, h.services);
    h.active = &h.world;
    play.reset();
    h.run(0.2);
    EXPECT_FLOAT_EQ(crate.worldPosition().y, *groundEdit + 5.f);
    EXPECT_DOUBLE_EQ(env.get<TimeOfDayComponent>().localHours, 10.0);
    EXPECT_EQ(sun.worldRotation(), sunRotation);
    EXPECT_FLOAT_EQ(sun.get<LightComponent>().intensity, sunLux);
    EXPECT_EQ(rt.terrainBodyCount(terrain), 0u);
    ASSERT_NE(rt.heightfield(terrain), nullptr); // rebuilt from the component for the edit world
    EXPECT_NEAR(*rt.terrainHeight({0.f, 0.f}), *groundEdit, 1e-5f);
    EXPECT_EQ(h.runtime<PhysicsRuntime>().physicsWorld().bodyCount(), 0u);
}
