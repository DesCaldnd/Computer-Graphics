// Глава 28: компоненты мира на сцене → WorldRenderData (gameplay) → WorldSnapshot (extract рендера), без GPU.
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/gameplay/world.hpp>
#include <oxwald/render/features/world/world_skinning.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

using namespace ox;

TEST(WorldComponents, FromEcsToRenderSnapshot) {
    registerSceneTypes();
    registerGameplayTypes(); // + компоненты мира (категория "World")
    render::registerWorldSkinningTypes(); // TerrainRender, VegetationPrototypes

    World world;
    Services services;
    SystemScheduler scheduler;
    gameplay::GameplayConfig config;
    config.physicsWorld.workerThreads = 0;
    config.worldSystems.streamingExecutor = gameplay::WorldSystemsConfig::Executor::Inline;
    addGameplaySystems(scheduler, services, config); // + addWorldSystems (config.world = true)

    // Ландшафт: процедурный, 256 м, два слоя splat (материалы — ассеты; здесь пустые id).
    Entity terrain = world.create("Terrain");
    auto& t = terrain.add<gameplay::TerrainComponent>();
    t.resolution = 129;
    t.worldSize = 256.f;
    t.heightScale = 20.f;
    t.noise.fractal.frequency = 1.f / 128.f;
    t.lod = {.leafNodeSize = 16, .lodCount = 4, .viewDistance = 1200.f};
    t.layers = {Uuid::fromName("guide.grass"), Uuid::fromName("guide.rock")};
    t.splatRules = {{.layer = 1, .minSlopeDeg = 25.f}};
    terrain.add<render::TerrainRenderComponent>().triplanarSlopeDeg = 30.f; // вид: только для рендера

    // Растительность: деревья вокруг наблюдателей (камера), прототип 0 — встроенный процедурный.
    auto& veg = terrain.add<gameplay::VegetationComponent>();
    world::VegetationLayer trees{.name = "tree", .kind = world::VegetationKind::Tree, .prototype = 0,
                                 .minDistance = 8.f, .boundingRadius = 4.f};
    veg.layers.push_back(trees);
    veg.scatterRadius = 96.f;

    // Небо, время суток (ведёт солнце), ветер.
    Entity env = world.create("Environment");
    env.add<EnvironmentComponent>();
    env.add<gameplay::SkyComponent>();
    env.add<gameplay::TimeOfDayComponent>().localHours = 17.5;
    env.add<gameplay::WindComponent>().speed = 6.f;
    Entity sun = world.create("Sun");
    sun.add<LightComponent>().type = LightType::Directional;
    env.get<gameplay::TimeOfDayComponent>().sun = sun.ref();
    Entity cam = world.create("Camera");
    cam.setPosition({0.f, 30.f, 0.f});
    cam.add<CameraComponent>().primary = true;

    scheduler.attach(world, services);
    scheduler.setPlaying(false); // визуальные данные (ландшафт, LOD, небо) считаются и в режиме редактора
    for (int i = 0; i < 3; ++i) scheduler.tick(world, services, 1.0 / 60.0);

    // Контракт с рендерером, заполняется в фазе Extract.
    const gameplay::WorldRenderData& wrd = services.get<gameplay::WorldRenderData>();
    ASSERT_EQ(wrd.terrains.size(), 1u);
    EXPECT_NE(wrd.terrains[0].heightfield, nullptr);
    EXPECT_NE(wrd.terrains[0].splat, nullptr);
    EXPECT_EQ(wrd.terrains[0].layerMaterials.size(), 2u);
    ASSERT_EQ(wrd.vegetation.size(), 1u);
    EXPECT_FALSE(wrd.vegetation[0].batches.empty());
    EXPECT_TRUE(wrd.sky.valid);
    EXPECT_TRUE(wrd.hasWind);

    // Extract рендера: хук моста gameplay → render (ставится вместе с фичами мира) заполняет WorldSnapshot.
    render::FeatureRegistry features; // в игре — Renderer::create()
    render::registerWorldSkinningFeatures(features);
    render::RenderSnapshot snapshot;
    render::extract(world, snapshot, {.services = &services});
    render::finalizeWorldSnapshot(snapshot); // применяет TerrainRenderComponent, ключ IBL неба
    const render::WorldSnapshot* ws = snapshot.findExtension<render::WorldSnapshot>();
    ASSERT_NE(ws, nullptr);
    ASSERT_EQ(ws->terrains.size(), 1u);
    EXPECT_FLOAT_EQ(ws->terrains[0].settings.triplanarSlopeDeg, 30.f); // TerrainRenderComponent доехал
    EXPECT_EQ(ws->vegetation.size(), 1u);
    EXPECT_TRUE(ws->sky.valid);
    EXPECT_TRUE(ws->hasWind);
    scheduler.detach();
}
