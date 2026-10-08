// Глава 22: компоненты объёмного тумана и облаков, сетка фроксов, ветер мира, уровни качества группы Volumetrics
// (docs/guide/22-volumetrics.md). GPU не нужен.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>
#include <oxwald/render/register_types.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::render;

TEST(GuideVolumetrics, FogAndCloudsOnTheScene) {
    registerSceneTypes();
    registerRenderTypes();
    World world;

    // Глобальный высотный туман живёт в EnvironmentComponent.
    auto& env = world.create("Environment").add<EnvironmentComponent>();
    env.fogEnabled = true;
    env.fogDensity = 0.02f;        // экстинкция (1/м) на высоте y = 0
    env.fogHeightFalloff = 0.1f;   // экспоненциальное убывание с высотой
    env.fogColor = {0.8f, 0.85f, 0.9f}; // альбедо рассеяния
    env.fogStartDistance = 5.0f;   // первые 5 м от камеры без тумана

    // Локальный объём: туман в низине, с шумом и ветром.
    Entity mist = world.create("SwampMist");
    mist.setPosition({10.0f, 1.0f, -20.0f});
    auto& fv = mist.add<FogVolumeComponent>();
    fv.shape = FogVolumeShape::Ellipsoid;
    fv.extents = {12.0f, 2.0f, 8.0f};    // полуразмеры (м), масштаб сущности тоже учитывается
    fv.density = 0.25f;
    fv.albedo = {0.85f, 0.9f, 0.8f};
    fv.falloff = 0.4f;                   // мягкий край: 40 % формы от границы внутрь
    fv.noiseIntensity = 0.7f;            // клочья
    fv.noiseScale = 6.0f;
    fv.noiseVelocity = {0.3f, 0.0f, 0.0f};

    // Глобальные переопределения фроксельного тумана (первый активный в мире).
    auto& vf = world.create("FogSettings").add<VolumetricFogComponent>();
    vf.anisotropy = 0.7f;   // сильнее рассеяние вперёд → ярче лучи против солнца
    vf.distance = 96.0f;    // дальность сетки (0 = r.VolumetricFog.Distance)

    // Слой облаков.
    auto& clouds = world.create("Clouds").add<CloudLayerComponent>();
    clouds.altitude = 1500.0f;
    clouds.thickness = 1800.0f;
    clouds.coverage = 0.6f;     // 0 — ясно, 1 — сплошная облачность
    clouds.cloudType = 0.5f;    // 0 слоистые, 0.5 кучевые, 1 кучево-дождевые
    clouds.windSpeed = 12.0f;

    world.updateTransforms();
    RenderSnapshot snap;
    extract(world, snap);
    EXPECT_TRUE(volumetrics::fogActive(snap));
    const auto* ext = snap.findExtension<volumetrics::VolumetricsSnapshot>();
    ASSERT_NE(ext, nullptr);
    ASSERT_EQ(ext->volumes.size(), 1u);
    ASSERT_TRUE(ext->clouds.has_value());
    EXPECT_FLOAT_EQ(volumetrics::froxelGridFromCVars(&snap).farDistance, 96.0f); // переопределение дальности

    // Мост мира передаёт глобальный ветер (м/с) — он сдвигает шум объёмов и облака.
    volumetrics::setWorldWind(snap, {4.0f, 0.0f, 1.0f});
    EXPECT_TRUE(ext->hasWind);
}

TEST(GuideVolumetrics, FroxelGrid) {
    volumetrics::FroxelGrid g; // по умолчанию 160 × 90 × 64, 128 м, scale 32 (уровень High)
    EXPECT_EQ(g.froxelCount(), 160u * 90u * 64u);
    EXPECT_NEAR(g.sliceToDepth(1.0f), 128.0f, 0.01f);
    // Экспоненциальное распределение: первый срез тоньше полуметра, последний — несколько метров.
    const f32 first = g.sliceToDepth(1.0f / 64.0f);
    const f32 last = g.sliceToDepth(1.0f) - g.sliceToDepth(63.0f / 64.0f);
    EXPECT_LT(first, 0.5f);
    EXPECT_GT(last, 4.0f * first);
    EXPECT_NEAR(g.depthToSlice(g.sliceToDepth(0.5f)), 0.5f, 1e-4f);
}

TEST(GuideVolumetrics, QualityLevels) {
    registerRenderTypes(); // регистрирует и cvar'ы фичи
    auto& reg = CVarRegistry::instance();

    scalability::setGroup(Scalability::Volumetrics, QualityLevel::Low);
    volumetrics::FroxelGrid low = volumetrics::froxelGridFromCVars();
    EXPECT_EQ(low.x, 96u);
    EXPECT_EQ(low.z, 32u);
    EXPECT_FLOAT_EQ(low.farDistance, 64.0f);
    EXPECT_EQ(reg.find("r.VolumetricClouds")->toString(), "false"); // Low: без облаков

    scalability::setGroup(Scalability::Volumetrics, QualityLevel::Ultra);
    volumetrics::FroxelGrid ultra = volumetrics::froxelGridFromCVars();
    EXPECT_EQ(ultra.froxelCount(), 240u * 135u * 128u); // в 4.5 раза больше фроксов, чем High
    EXPECT_EQ(reg.find("r.VolumetricClouds.Checkerboard")->toString(), "4");

    scalability::setGroup(Scalability::Volumetrics, QualityLevel::High); // = значения по умолчанию
    EXPECT_EQ(volumetrics::froxelGridFromCVars().z, 64u);
}
