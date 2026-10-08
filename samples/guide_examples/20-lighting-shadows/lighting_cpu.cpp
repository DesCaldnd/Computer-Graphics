// Глава 20: источники света в физических единицах, окружение, экспозиция камеры, кластеры, математика теней
// (каскады, атлас), cvar'ы и уровни качества теней (docs/guide/20-lighting-shadows.md). GPU не нужен.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace ox;
using namespace ox::render;

TEST(GuideLighting, LightsAndEnvironmentInTheScene) {
    registerSceneTypes();
    World world;

    // Солнце: направленный свет в люксах. Светит вдоль локальной -Z сущности.
    Entity sun = world.create("Sun");
    sun.setRotation(lookRotation(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f))));
    auto& sl = sun.add<LightComponent>();
    sl.type = LightType::Directional;
    sl.intensity = 100000.0f;  // лк: прямое солнце в ясный полдень
    sl.color = {1.0f, 0.96f, 0.9f};
    sl.sourceRadius = 0.27f;   // для солнца — угловой радиус в градусах (мягкость PCSS)

    // Лампа: точечный свет в люменах (лампа накаливания 60 Вт ≈ 800 лм).
    Entity bulb = world.create("Bulb");
    bulb.setPosition({2.0f, 2.5f, 0.0f});
    auto& pl = bulb.add<LightComponent>();
    pl.type = LightType::Point;
    pl.intensity = 800.0f;     // лм
    pl.range = 6.0f;           // м: дальше свет обрезается
    pl.sourceRadius = 0.05f;   // м: радиус колбы (мягкость тени)
    pl.castShadows = true;

    // Прожектор: тоже люмены, конус задаётся внутренним и внешним углом.
    Entity spot = world.create("Spot");
    spot.setPosition({0.0f, 4.0f, 2.0f});
    spot.setRotation(lookRotation(glm::normalize(glm::vec3(0.0f, -1.0f, -0.5f))));
    auto& sp = spot.add<LightComponent>();
    sp.type = LightType::Spot;
    sp.intensity = 2000.0f;
    sp.range = 12.0f;
    sp.innerConeAngle = 20.0f; // градусы: полная яркость внутри
    sp.outerConeAngle = 30.0f; // градусы: ноль снаружи
    sp.shadowResolution = 512; // подсказка размера тайла в атласе (0 = решает рендер)

    // Окружение: небо, IBL, высотный туман. `sun` явно указывает, какой направленный свет — солнце.
    Entity envEntity = world.create("Environment");
    auto& env = envEntity.add<EnvironmentComponent>();
    env.sun = EntityRef(sun.get<IdComponent>().id);
    env.skyIntensity = 1.0f;     // множитель процедурного неба / HDRI
    env.ambientIntensity = 1.0f; // множитель IBL (диффуз и отражения неба)

    world.updateTransforms();
    RenderSnapshot snap;
    extract(world, snap);
    ASSERT_EQ(snap.lights.size(), 3u);
    ASSERT_TRUE(snap.environment.has_value());
    const SnapshotLight& sunLight = snap.lights[usize(snap.environment->sunLight)];
    EXPECT_EQ(sunLight.light.type, LightType::Directional);
    // direction — куда летит свет: мировая -Z сущности.
    EXPECT_NEAR(glm::dot(sunLight.direction, glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f))), 1.0f, 1e-4f);
}

TEST(GuideLighting, PhysicalCameraExposure) {
    // EV100 = log2(N² / t · 100 / ISO) − компенсация; exposure = 1 / (1.2 · 2^EV100).
    CameraComponent cam;          // f/16, 1/125 с, ISO 100 — правило «солнечных 16»
    EXPECT_NEAR(cam.ev100(), 14.97f, 0.01f);
    cam.exposureCompensation = 1.0f; // +1 EV = в два раза светлее кадр
    EXPECT_NEAR(cam.ev100(), 13.97f, 0.01f);
    EXPECT_FLOAT_EQ(cam.exposure(), 1.0f / (1.2f * std::exp2(cam.ev100())));

    CameraComponent indoor;
    indoor.aperture = 2.8f;
    indoor.shutterSpeed = 1.0f / 60.0f;
    indoor.iso = 800.0f;
    EXPECT_NEAR(indoor.ev100(), 5.88f, 0.01f); // интерьер: EV100 ≈ 5–7

    // Рендер берёт EV100 из CameraParams (CameraParams::fromComponent копирует ev100()).
    const CameraParams p = CameraParams::fromComponent(indoor, glm::mat4(1.0f));
    EXPECT_FLOAT_EQ(p.ev100, indoor.ev100());
}

TEST(GuideLighting, ClusterGrid) {
    // Сетка кластеров: 16×9 экранных плиток × 24 экспоненциальных среза глубины.
    ClusterGrid g;
    g.nearPlane = 0.1f;
    g.farPlane = 500.0f; // = min(camera far, r.Clusters.MaxDistance)
    EXPECT_EQ(g.clusterCount(), 16u * 9u * 24u);
    EXPECT_EQ(g.slice(0.1f), 0u);
    EXPECT_EQ(g.slice(1000.0f), 23u); // дальше far — последний срез
    // Срезы растут с расстоянием: ближний тоньше дальнего.
    EXPECT_LT(g.sliceFar(0) - g.sliceNear(0), g.sliceFar(23) - g.sliceNear(23));
    // Буфер списков: (1 + maxLightsPerCluster) u32 на кластер → ~3.5 МБ при 256.
    EXPECT_EQ(g.bufferSize(), u64(3456) * 257 * 4);
}

TEST(GuideLighting, CascadeSplitsAndStableCascades) {
    // Практическая схема: lambda 0 — равномерно, 1 — логарифмически (r.Shadows.CSM.Lambda = 0.75).
    const std::vector<f32> splits = cascadeSplits(0.1f, 120.0f, 4, 0.75f);
    ASSERT_EQ(splits.size(), 4u);
    EXPECT_NEAR(splits[3], 120.0f, 1e-3f);
    EXPECT_LT(splits[0], 30.0f); // первый каскад короче четверти дистанции

    CascadeInput in;
    in.cameraWorld = CameraParams::lookAt({0, 5, 10}, {0, 0, 0}).world;
    in.aspect = 16.0f / 9.0f;
    in.shadowDistance = 120.0f; // r.Shadows.CSM.Distance
    in.cascadeCount = 4;        // r.Shadows.CSM.Cascades
    in.resolution = 2048;       // r.Shadows.CSM.Resolution
    in.lightDirection = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.2f));
    const std::vector<Cascade> cascades = computeCascades(in);
    ASSERT_EQ(cascades.size(), 4u);
    for (const Cascade& c : cascades) EXPECT_NEAR(c.texelWorld, 2.0f * c.radius / 2048.0f, 1e-5f);
    // Размер каскада не зависит от поворота камеры (ограничивающая сфера) — тени не «плавают».
    CascadeInput turned = in;
    turned.cameraWorld = CameraParams::lookAt({0, 5, 10}, {8, 2, -3}).world;
    EXPECT_FLOAT_EQ(computeCascades(turned)[2].radius, cascades[2].radius);
}

TEST(GuideLighting, ShadowAtlasBudget) {
    // Бюджет локальных теней: как в ShadowsRaster, из r.Shadows.* (уровень High).
    ShadowBudget budget;
    budget.atlasSize = 4096;
    budget.minResolution = 128;  // r.Shadows.SpotMinResolution
    budget.maxResolution = 1024; // r.Shadows.SpotMaxResolution
    budget.maxShadowedLights = 16;
    budget.maxPointLights = 8;
    budget.pointResolution = 512;

    std::vector<ShadowRequest> requests;
    for (u32 i = 0; i < 3; ++i) {
        ShadowRequest r;
        r.lightIndex = i;
        r.position = {0.0f, 0.0f, -5.0f - 30.0f * f32(i)}; // ближе к камере → важнее → крупнее тайл
        r.range = 5.0f;
        requests.push_back(r);
    }
    ShadowAtlasAllocator atlas(budget.atlasSize, budget.minResolution);
    const std::vector<ShadowAllocation> allocs =
        allocateShadows(requests, glm::vec3(0.0f), glm::radians(60.0f), 1080.0f, budget, atlas);
    ASSERT_EQ(allocs.size(), 3u);
    EXPECT_EQ(allocs[0].lightIndex, 0u); // отсортированы по важности
    EXPECT_GT(allocs[0].resolution, allocs[2].resolution);
    EXPECT_EQ(allocs[0].resolution & (allocs[0].resolution - 1), 0u); // степень двойки
}

TEST(GuideLighting, ShadowCVarsAndScalability) {
    registerRenderCVars(); // cvar'ы рендера — статики в библиотеке; так они точно зарегистрированы
    auto& reg = CVarRegistry::instance();

    scalability::setGroup(Scalability::Shadows, QualityLevel::Low);
    RenderSettings low = RenderSettings::fromCVars(); // снимок, который рендер делает раз в кадр
    EXPECT_EQ(low.csmResolution, 1024);
    EXPECT_EQ(low.csmCascades, 2);
    EXPECT_EQ(low.pcfTaps, 0); // один аппаратный compare
    EXPECT_FALSE(low.pcss);

    scalability::setGroup(Scalability::Shadows, QualityLevel::Ultra);
    RenderSettings ultra = RenderSettings::fromCVars();
    EXPECT_EQ(ultra.csmResolution, 4096);
    EXPECT_FLOAT_EQ(ultra.csmDistance, 250.0f);
    EXPECT_EQ(ultra.atlasSize, 8192);
    EXPECT_TRUE(ultra.pcss);

    // Точечная правка поверх уровня: группа становится Custom.
    ASSERT_TRUE(reg.execute("r.Shadows.CSM.Distance 400"));
    EXPECT_FLOAT_EQ(RenderSettings::fromCVars().csmDistance, 400.0f);
    EXPECT_EQ(scalability::currentLevel(Scalability::Shadows), QualityLevel::Custom);

    // Остальные «ручки» освещения — без групп масштабируемости.
    ASSERT_TRUE(reg.execute("r.Exposure.Mode Manual"));
    ASSERT_TRUE(reg.execute("r.Exposure.EV100 12"));
    EXPECT_EQ(RenderSettings::fromCVars().exposureMode, ExposureMode::Manual);
    reg.find("r.Exposure.Mode")->reset();
    reg.find("r.Exposure.EV100")->reset();

    scalability::setGroup(Scalability::Shadows, QualityLevel::High); // обратно к значениям по умолчанию
    EXPECT_FLOAT_EQ(RenderSettings::fromCVars().csmDistance, 150.0f);
}
