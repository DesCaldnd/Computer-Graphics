#include "world_test_utils.hpp"

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

TEST(GameplayWorld, LuaReadsAndWritesWorldComponentsAndApi) {
    WorldHarness h;
    Entity terrain = h.terrain();
    Entity env = h.world.create("Env");
    auto& tod = env.add<TimeOfDayComponent>();
    tod.localHours = 9.0;
    tod.paused = true;
    Entity sea = h.world.create("Sea");
    sea.setPosition({0.f, -1.f, 0.f});
    sea.add<WaterComponent>().waveCount = 0;
    env.add<WindComponent>().speed = 3.f;

    h.assets.addScript("probe", R"(
        function onStart(self)
            local t = scene.find("Terrain"):get("Terrain")
            self.resolution = t.resolution
            self.source = t.source
            t.lod.viewDistance = 3000
            local tod = scene.find("Env"):get("TimeOfDay")
            self.hoursBefore = tod.localHours
            tod.localHours = 15.5
            self.height = world.terrainHeight(3, 4)
            self.outside = world.terrainHeight(5000, 0) == nil
            self.water = world.waterHeight(1, 1)
            self.noWater = world.waterHeight(5000, 0) == nil
            local w = world.wind(vec3(0, 0, 0))
            self.windLength = math.sqrt(w.x * w.x + w.z * w.z)
        end
        function onUpdate(self, dt)
            self.frames = (self.frames or 0) + 1
            if self.frames == 3 then
                self.todSeen = world.timeOfDay()
                world.setTimeOfDay(18.25)
            end
        end
    )");
    Entity probe = h.world.create("Probe");
    probe.add<ScriptComponent>().script = "probe";
    h.start(true);
    h.run(0.2);

    sol::table self = h.runtime<ScriptRuntime>().self(probe);
    ASSERT_TRUE(self.valid());
    EXPECT_EQ(self["resolution"].get_or(0), 129);
    EXPECT_EQ(self["source"].get_or(std::string()), "Procedural");
    auto& rt = h.worldRt();
    EXPECT_NEAR(self["height"].get_or(-1000.0), static_cast<f64>(*rt.terrainHeight({3.f, 4.f})), 1e-4);
    EXPECT_TRUE(self["outside"].get_or(false));
    EXPECT_NEAR(self["water"].get_or(-1000.0), -1.0, 1e-5);
    EXPECT_TRUE(self["noWater"].get_or(false));
    EXPECT_GT(self["windLength"].get_or(0.0), 0.5);
    EXPECT_DOUBLE_EQ(self["hoursBefore"].get_or(0.0), 9.0);
    EXPECT_NEAR(self["todSeen"].get_or(0.0), 15.5, 1e-6);

    // Nested reflected write fired the change signal -> terrain rebuilt with the new LOD settings.
    EXPECT_FLOAT_EQ(terrain.get<TerrainComponent>().lod.viewDistance, 3000.f);
    ASSERT_NE(rt.quadtree(terrain), nullptr);
    EXPECT_FLOAT_EQ(rt.quadtree(terrain)->settings().viewDistance, 3000.f);
    // world.setTimeOfDay (paused clock keeps it).
    EXPECT_NEAR(env.get<TimeOfDayComponent>().localHours, 18.25, 1e-6);
    EXPECT_NEAR(*rt.timeOfDay(), 18.25, 1e-6);
}
