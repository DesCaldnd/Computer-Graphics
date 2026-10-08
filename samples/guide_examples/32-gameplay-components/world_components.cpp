// Глава 32: компоненты мира — Terrain, TimeOfDay + Light + Environment, Water, Wind, Buoyancy
// (docs/guide/32-gameplay-components.md).
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/gameplay/world.hpp>
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::gameplay;

namespace {

GameplayConfig quietConfig() {
    GameplayConfig c;
    c.physicsWorld.workerThreads = 0;
    c.physicsWorld.maxBodies = 4096;
    c.physicsWorld.maxBodyPairs = 4096;
    c.physicsWorld.maxContactConstraints = 4096;
    c.physicsWorld.tempAllocatorBytes = 16u << 20;
    c.scriptVM.hotReloadInterval = 0.0;
    c.worldSystems.streamingExecutor = WorldSystemsConfig::Executor::Inline;   // детерминированный стриминг
    return c;
}

struct WorldGame {
    GameplayAssetRegistry providers;
    World world;
    Services services;
    SystemScheduler scheduler;

    WorldGame() {
        registerSceneTypes();
        registerGameplayTypes();   // вместе с компонентами мира (в сборке с модулем world)
        providers.registerIn(services);
        addGameplaySystems(scheduler, services, quietConfig());   // добавляет и системы мира
    }
    ~WorldGame() { scheduler.detach(); }
    void run(int frames) {
        for (int i = 0; i < frames; ++i) scheduler.tick(world, services, 1.0 / 60.0);
    }
};

} // namespace

TEST(GuideGameplayWorld, TerrainAndTimeOfDayInEditMode) {
    WorldGame g;
    // Ландшафт 128 × 128 м, шаг 1 м, высоты 0..8 м; центр — позиция сущности.
    Entity terrain = g.world.create("Terrain");
    auto& t = terrain.add<TerrainComponent>();
    t.source = TerrainSource::Procedural;
    t.resolution = 129;
    t.worldSize = 128.f;
    t.heightScale = 8.f;
    t.noise.fractal.frequency = 1.f / 128.f;
    t.noise.fractal.octaves = 3;
    t.lod = {.leafNodeSize = 16, .lodCount = 3, .viewDistance = 400.f};

    // Солнце, окружение и смена дня и ночи.
    Entity sun = g.world.create("Sun");
    sun.add<LightComponent>().type = LightType::Directional;
    Entity env = g.world.create("Environment");
    env.add<EnvironmentComponent>().sun = sun.ref();
    env.add<SkyComponent>();
    auto& tod = env.add<TimeOfDayComponent>();   // Амстердам, 21 июня 2024, UTC+2
    tod.localHours = 13.5;
    Entity camera = g.world.create("Camera");
    camera.add<CameraComponent>().primary = true;

    g.scheduler.attach(g.world, g.services);   // edit mode: время стоит, но ландшафт и небо считаются
    g.run(2);
    WorldRuntime& rt = g.services.get<WorldRuntime>();
    const auto h = rt.terrainHeight({3.f, 4.f});
    ASSERT_TRUE(h.has_value());
    EXPECT_GE(*h, 0.f);
    EXPECT_LE(*h, 8.f);
    EXPECT_FALSE(rt.terrainHeight({500.f, 0.f}).has_value());   // за пределами ландшафта
    EXPECT_GT(terrain.get<TerrainComponent>().maxHeight, 0.f);    // «зеркало» рантайма в компоненте
    EXPECT_GT(sun.get<LightComponent>().intensity, 50000.f);      // полдень: люксы от TimeOfDay
    EXPECT_TRUE(env.get<TimeOfDayComponent>().isDay);

    // Инспектор меняет время: patch() шлёт сигнал изменения — солнце уходит за горизонт без play mode.
    env.patch<TimeOfDayComponent>([](TimeOfDayComponent& c) { c.localHours = 1.0; });
    g.run(2);
    EXPECT_FALSE(env.get<TimeOfDayComponent>().isDay);
    EXPECT_LT(sun.get<LightComponent>().intensity, 1000.f);
}

TEST(GuideGameplayWorld, WaterWindAndBuoyancyInPlayMode) {
    WorldGame g;
    Entity sea = g.world.create("Sea");
    sea.setPosition({0.f, 0.f, 0.f});   // уровень воды = Y сущности
    auto& water = sea.add<WaterComponent>();
    water.waveCount = 0;               // без волн (пустой waves + 0 волн из ветра) — ровная вода
    water.size = {100.f, 100.f};

    Entity wind = g.world.create("Wind");
    auto& w = wind.add<WindComponent>();
    w.direction = {1.f, 0.f};
    w.speed = 6.f;

    // Ящик 1×1×1 м массой 400 кг: плотность 400 кг/м³ — плавает, погружён примерно на 40 %.
    Entity crate = g.world.create("Crate");
    crate.setPosition({0.f, 2.f, 0.f});
    auto& rb = crate.add<RigidBodyComponent>();
    rb.mass = 400.f;
    crate.add<ColliderComponent>().halfExtents = glm::vec3(0.5f);
    auto& b = crate.add<BuoyancyComponent>();
    b.halfExtents = glm::vec3(0.5f);
    b.subdivisions = 3;

    g.scheduler.attach(g.world, g.services);
    g.scheduler.setPlaying(true);
    g.run(600);   // 10 с: успокоился на воде
    WorldRuntime& rt = g.services.get<WorldRuntime>();
    EXPECT_NEAR(*rt.waterHeight({0.f, 0.f}), 0.f, 1e-4f);
    EXPECT_GT(glm::length(rt.windAt({0.f, 0.f, 0.f})), 1.f);
    EXPECT_NEAR(crate.get<BuoyancyComponent>().submergedFraction, 0.4f, 0.1f);
    EXPECT_GT(crate.worldPosition().y, -0.5f);   // не утонул
    EXPECT_LT(crate.worldPosition().y, 0.5f);
}
