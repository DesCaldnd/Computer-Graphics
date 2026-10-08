#include "world_test_utils.hpp"

#include <oxwald/core/events.hpp>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

struct DayScene {
    Entity env;
    Entity sun;
    Entity camera;
};

DayScene makeDayScene(WorldHarness& h, f64 localHours) {
    DayScene s;
    s.sun = h.world.create("Sun");
    auto& l = s.sun.add<LightComponent>();
    l.type = LightType::Directional;
    s.env = h.world.create("Environment");
    auto& tod = s.env.add<TimeOfDayComponent>(); // Amsterdam, 2024-06-21, UTC+2
    tod.localHours = localHours;
    tod.sun = s.sun.ref();
    s.env.add<EnvironmentComponent>();
    s.env.add<SkyComponent>();
    s.camera = h.world.create("Camera");
    s.camera.add<CameraComponent>().primary = true;
    return s;
}

glm::vec3 lightForward(Entity e) { return e.worldRotation() * glm::vec3(0.f, 0.f, -1.f); }

} // namespace

TEST(GameplayWorld, TimeOfDayDrivesSunAndEnvironmentInEditMode) {
    WorldHarness h;
    DayScene s = makeDayScene(h, 13.5);
    h.start(false);
    h.tick();
    auto& rt = h.worldRt();
    const world::SkyState* sky = rt.skyState();
    ASSERT_NE(sky, nullptr);
    EXPECT_GT(sky->sunDirection.y, 0.8f); // ~61 deg at solar noon on the solstice
    EXPECT_GT(glm::dot(lightForward(s.sun), -sky->sunDirection), 0.9999f);
    const auto& light = s.sun.get<LightComponent>();
    const f32 noonLux = light.intensity;
    const glm::vec3 noonColor = light.color;
    EXPECT_GT(noonLux, 50000.f);
    const auto& env = s.env.get<EnvironmentComponent>();
    const f32 noonFog = env.fogDensity, noonAmbient = env.ambientIntensity;
    EXPECT_TRUE(s.env.get<TimeOfDayComponent>().isDay);
    EXPECT_NEAR(*rt.timeOfDay(), 13.5, 1e-9);

    // Editor changes localHours: the light follows without play mode (and time does not advance).
    s.env.patch<TimeOfDayComponent>([](TimeOfDayComponent& c) { c.localHours = 1.0; });
    h.tick();
    h.tick();
    sky = rt.skyState();
    ASSERT_NE(sky, nullptr);
    EXPECT_LT(sky->sunDirection.y, 0.f);
    EXPECT_FALSE(s.env.get<TimeOfDayComponent>().isDay);
    EXPECT_DOUBLE_EQ(s.env.get<TimeOfDayComponent>().localHours, 1.0);
    EXPECT_GT(glm::dot(lightForward(s.sun), -sky->mainLightDirection), 0.9999f);
    EXPECT_LT(light.intensity, noonLux * 0.01f);
    EXPECT_NE(light.color, noonColor);
    EXPECT_NE(env.fogDensity, noonFog);
    EXPECT_NE(env.ambientIntensity, noonAmbient);
    EXPECT_FALSE(h.renderData().sky.isDay);
}

TEST(GameplayWorld, TimeOfDayFiresSunriseWhenCrossing) {
    WorldHarness h;
    DayScene s = makeDayScene(h, 4.0);
    s.env.get<TimeOfDayComponent>().timeScale = 3600.0; // one game hour per second
    std::vector<world::TimeOfDayEvent> signalled;
    std::vector<world::TimeOfDayEvent> published;
    auto& rt = h.worldRt();
    ScopedConnection c1 = rt.onTimeOfDayEvent.connect([&](const WorldTimeEvent& e) {
        signalled.push_back(e.event);
        EXPECT_EQ(e.entity, s.env);
    });
    ScopedConnection c2 = h.services.get<EventBus>().subscribe<WorldTimeEvent>([&](const WorldTimeEvent& e) { published.push_back(e.event); });
    h.start(true);
    h.tick();
    const f32 nightLux = s.sun.get<LightComponent>().intensity;
    EXPECT_TRUE(signalled.empty());
    h.run(3.0); // 04:00 -> ~07:00 (sunrise in Amsterdam ~05:18 CEST)
    ASSERT_EQ(signalled.size(), 1u);
    EXPECT_EQ(signalled[0], world::TimeOfDayEvent::Sunrise);
    EXPECT_EQ(published, signalled);
    EXPECT_NEAR(s.env.get<TimeOfDayComponent>().localHours, 7.0, 0.1);
    EXPECT_TRUE(s.env.get<TimeOfDayComponent>().isDay);
    EXPECT_GT(s.sun.get<LightComponent>().intensity, nightLux * 10.f);

    // Lua-style jump: no events.
    EXPECT_TRUE(rt.setTimeOfDay(20.0));
    h.tick();
    EXPECT_EQ(signalled.size(), 1u);
}

TEST(GameplayWorld, BuoyancyFloatsAtExpectedDraft) {
    WorldHarness h;
    Entity sea = h.world.create("Sea");
    auto& water = sea.add<WaterComponent>();
    water.waveCount = 0; // flat
    Entity boat = h.box("Crate", {0.f, 2.f, 0.f}, glm::vec3(0.5f));
    boat.get<RigidBodyComponent>().mass = 500.f; // half the density of water -> half submerged
    boat.add<BuoyancyComponent>();
    h.start(true);
    h.run(10.0);
    auto& rt = h.worldRt();
    EXPECT_NEAR(boat.worldPosition().y, 0.f, 0.06f);
    EXPECT_NEAR(rt.submergedFraction(boat), 0.5f, 0.06f);
    EXPECT_NEAR(boat.get<BuoyancyComponent>().submergedFraction, 0.5f, 0.06f);
    EXPECT_NEAR(*rt.waterHeight({0.f, 0.f}), 0.f, 1e-5f);
    EXPECT_FALSE(rt.waterHeight({500.f, 0.f}).has_value()); // outside the 200 m extent

    // Heavier box: deeper draft (800 kg -> 0.8 submerged; centre 0.3 m below the surface).
    boat.patch<RigidBodyComponent>([](RigidBodyComponent& rb) { rb.mass = 800.f; });
    h.run(10.0);
    EXPECT_NEAR(boat.worldPosition().y, -0.3f, 0.08f);
}

TEST(GameplayWorld, BuoyancyFollowsWaves) {
    WorldHarness h;
    Entity sea = h.world.create("Sea");
    sea.setPosition({0.f, 3.f, 0.f});
    auto& water = sea.add<WaterComponent>();
    water.waveCount = 4;
    water.windSpeed = 5.f;
    Entity boat = h.box("Boat", {0.f, 5.f, 0.f}, {1.f, 0.4f, 2.f});
    boat.get<RigidBodyComponent>().mass = 0.4f * 3.2f * 1000.f; // ~half of the displaced volume
    boat.add<BuoyancyComponent>().halfExtents = {1.f, 0.4f, 2.f};
    h.start(true);
    h.run(5.0);
    auto& rt = h.worldRt();
    f32 maxDev = 0.f;
    for (int i = 0; i < 180; ++i) {
        h.tick();
        const glm::vec3 p = boat.worldPosition();
        maxDev = std::max(maxDev, std::abs(p.y - *rt.waterHeight({p.x, p.z})));
    }
    EXPECT_LT(maxDev, 0.6f);
    EXPECT_GT(rt.submergedFraction(boat), 0.1f);
    EXPECT_LT(rt.submergedFraction(boat), 0.95f);
}

TEST(GameplayWorld, WindAndWeather) {
    WorldHarness h;
    Entity e = h.world.create("Weather");
    auto& w = e.add<WindComponent>();
    w.direction = {0.f, 1.f};
    w.speed = 5.f;
    w.gustStrength = 0.f;
    w.turbulence = 0.f;
    h.start(true);
    h.tick();
    auto& rt = h.worldRt();
    const glm::vec3 v = rt.windAt({0.f, 0.f, 0.f});
    EXPECT_NEAR(v.z, 5.f, 1e-3f);
    EXPECT_EQ(rt.weather(), nullptr);
    ASSERT_TRUE(rt.setWeather(world::WeatherPreset::storm(), 2.f));
    h.run(3.0);
    ASSERT_NE(rt.weather(), nullptr);
    EXPECT_NEAR(rt.weather()->current.rain, 1.f, 1e-3f);
    EXPECT_GT(rt.weather()->wetness, 0.f);
    EXPECT_TRUE(h.renderData().hasWeather);
    EXPECT_NEAR(h.renderData().wind.dirSpeedTime.z, 16.f, 1e-3f); // storm wind speed
    EXPECT_GT(e.get<WindComponent>().weatherState.wetness, 0.f);
}
