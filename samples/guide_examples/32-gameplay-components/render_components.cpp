// Глава 32: компоненты рендера — отражения, частицы, вода, волюметрика, пост-обработка, растительность.
// CPU-часть: регистрация, значения по умолчанию, сохранение в сцену (docs/guide/32-gameplay-components.md).
#include <oxwald/render/components/postprocess.hpp>
#include <oxwald/render/components/reflections.hpp>
#include <oxwald/render/components/translucency.hpp>
#include <oxwald/render/components/volumetrics.hpp>
#include <oxwald/render/components/world.hpp>
#include <oxwald/render/register_types.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

#include <filesystem>

using namespace ox;
using namespace ox::render;

TEST(GuideRenderComponents, RegisteredByName) {
    registerSceneTypes();
    registerRenderTypes();   // идемпотентно; Engine/редактор вызывают сами
    const ComponentRegistry& reg = ComponentRegistry::instance();
    for (const char* name : {"ReflectionProbe", "PlanarReflector", "IrradianceVolume", "ParticleEmitter", "WaterSurface",
                             "FogVolume", "VolumetricFog", "CloudLayer", "PostProcessVolume", "VegetationPrototypes",
                             "TerrainRender"}) {
        EXPECT_NE(reg.find(name), nullptr) << name;
    }
}

TEST(GuideRenderComponents, DefaultsMatchTheReference) {
    const ReflectionProbeComponent probe;
    EXPECT_EQ(probe.extents, glm::vec3(5.f, 3.f, 5.f));
    EXPECT_EQ(probe.resolution, 128u);
    EXPECT_EQ(probe.update, ReflectionProbeUpdate::Baked);
    const ParticleEmitterComponent particles;
    EXPECT_EQ(particles.maxParticles, 1024u);
    EXPECT_FLOAT_EQ(particles.spawnRate, 50.f);
    EXPECT_EQ(particles.blend, ParticleBlend::Alpha);
    const FogVolumeComponent fog;
    EXPECT_FLOAT_EQ(fog.density, 0.1f);
    const CloudLayerComponent clouds;
    EXPECT_FLOAT_EQ(clouds.altitude, 1500.f);
    EXPECT_FLOAT_EQ(clouds.coverage, 0.45f);
    const PostProcessVolumeComponent volume;
    EXPECT_TRUE(volume.unbound);
    EXPECT_FLOAT_EQ(volume.settings.bloomIntensity, 0.04f);
    const WaterSurfaceComponent water;
    EXPECT_EQ(water.size, glm::vec2(200.f, 200.f));
    const TerrainRenderComponent terrain;
    EXPECT_FLOAT_EQ(terrain.triplanarSlopeDeg, 35.f);
}

TEST(GuideRenderComponents, SceneRoundTrip) {
    registerSceneTypes();
    registerRenderTypes();
    World world;

    Entity room = world.create("RoomProbe");
    room.setPosition({0.f, 1.5f, 0.f});
    auto& probe = room.add<ReflectionProbeComponent>();
    probe.extents = {4.f, 1.5f, 6.f};   // коробка комнаты: параллакс-коррекция отражений
    probe.update = ReflectionProbeUpdate::OnEnable;

    Entity torch = world.create("TorchFire");
    auto& fire = torch.add<ParticleEmitterComponent>();
    fire.shape = ParticleShape::Cone;
    fire.coneAngle = 15.f;
    fire.spawnRate = 120.f;
    fire.blend = ParticleBlend::Additive;
    fire.colorOverLife = {{0.f, {1.f, 0.6f, 0.2f, 1.f}}, {1.f, {0.3f, 0.1f, 0.05f, 0.f}}};

    Entity mist = world.create("Mist");
    auto& fog = mist.add<FogVolumeComponent>();
    fog.shape = FogVolumeShape::Ellipsoid;
    fog.extents = {10.f, 2.f, 10.f};
    fog.density = 0.05f;

    Entity grading = world.create("CaveGrading");
    auto& pp = grading.add<PostProcessVolumeComponent>();
    pp.unbound = false;   // коробка: действует внутри extents + blendRadius
    pp.extents = {8.f, 4.f, 8.f};
    pp.priority = 10;
    pp.settings.overrideExposure = true;
    pp.settings.exposureCompensation = 1.0f;

    const auto path = std::filesystem::temp_directory_path() / ("guide32_render_" + Uuid::generate().toString() + ".oxscene.json");
    ASSERT_TRUE(saveScene(world, path));   // JSON: поля по именам, enum'ы строками
    World loaded;
    ASSERT_TRUE(loadScene(loaded, path));
    std::filesystem::remove(path);

    EXPECT_EQ(loaded.findByName("RoomProbe").get<ReflectionProbeComponent>().extents, glm::vec3(4.f, 1.5f, 6.f));
    EXPECT_EQ(loaded.findByName("TorchFire").get<ParticleEmitterComponent>().colorOverLife.size(), 2u);
    EXPECT_EQ(loaded.findByName("Mist").get<FogVolumeComponent>().shape, FogVolumeShape::Ellipsoid);
    const auto& loadedPp = loaded.findByName("CaveGrading").get<PostProcessVolumeComponent>();
    EXPECT_TRUE(loadedPp.settings.overrideExposure);
    EXPECT_EQ(loadedPp.priority, 10);
}
