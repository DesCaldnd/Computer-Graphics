// Глава 23: прозрачность, преломление, вода и GPU-частицы — CPU-часть (docs/guide/23-transparency-water-particles.md).
// Материалы .oxmat, компоненты ParticleEmitterComponent / WaterSurfaceComponent, extract в TranslucencySnapshot,
// математика волн Герстнера, cvar'ы и группы масштабируемости. GPU не нужен.
#include <oxwald/assets/material.hpp>
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/render/components/translucency.hpp>
#include <oxwald/render/features/translucency/translucency.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace ox;
using namespace ox::render;

namespace {

std::string cvarText(const char* name) { return CVarRegistry::instance().find(name)->toString(); }

void registerAll() {
    registerSceneTypes();
    registerTranslucencyTypes(); // вызывается и из render::registerRenderTypes()
}

} // namespace

// --- Шаг 1. Материалы: Transparent и Refractive ---

TEST(GuideTranslucency, MaterialsFromOxmat) {
    const std::filesystem::path dir = OX_GUIDE_DIR;
    Result<assets::MaterialAsset> glass = assets::loadMaterialFile(dir / "glass.oxmat");
    ASSERT_TRUE(glass.hasValue()) << glass.error().message;
    EXPECT_EQ(glass->blendMode, assets::BlendMode::Refractive);
    EXPECT_FLOAT_EQ(glass->ior, 1.5f);
    EXPECT_FLOAT_EQ(glass->absorptionDistance, 1.0f);
    EXPECT_FLOAT_EQ(glass->absorptionColor.y, 0.9f);
    EXPECT_FLOAT_EQ(glass->transmission, 0.0f); // для Refractive 0 трактуется как 1

    Result<assets::MaterialAsset> window = assets::loadMaterialFile(dir / "tinted_window.oxmat");
    ASSERT_TRUE(window.hasValue()) << window.error().message;
    EXPECT_EQ(window->blendMode, assets::BlendMode::Transparent);
    EXPECT_FLOAT_EQ(window->baseColor.a, 0.45f);    // непрозрачность берётся из альфы baseColor
    EXPECT_EQ(window->renderQueueOffset, 1);        // != 0 → всегда сортированный путь, не OIT
}

// --- Шаг 4. Частицы: значения по умолчанию (таблица в главе) ---

TEST(GuideTranslucency, EmitterDefaults) {
    const ParticleEmitterComponent e;
    EXPECT_TRUE(e.enabled);
    EXPECT_EQ(e.maxParticles, 1024u);
    EXPECT_EQ(e.seed, 1u);
    EXPECT_FLOAT_EQ(e.spawnRate, 50.0f);
    EXPECT_TRUE(e.bursts.empty());
    EXPECT_TRUE(e.loop);
    EXPECT_FLOAT_EQ(e.duration, 5.0f);
    EXPECT_EQ(e.lifetime, glm::vec2(1.5f, 2.5f));
    EXPECT_EQ(e.shape, ParticleShape::Point);
    EXPECT_FLOAT_EQ(e.radius, 0.5f);
    EXPECT_FLOAT_EQ(e.coneAngle, 25.0f);
    EXPECT_EQ(e.boxExtents, glm::vec3(0.5f));
    EXPECT_FALSE(e.emitFromShell);
    EXPECT_EQ(e.speed, glm::vec2(1.0f, 2.0f));
    EXPECT_EQ(e.velocity, glm::vec3(0.0f));
    EXPECT_EQ(e.gravity, glm::vec3(0.0f, -9.81f, 0.0f));
    EXPECT_FLOAT_EQ(e.gravityScale, 0.0f);
    EXPECT_FLOAT_EQ(e.drag, 0.0f);
    EXPECT_FLOAT_EQ(e.turbulence, 0.0f);
    EXPECT_FLOAT_EQ(e.turbulenceFrequency, 1.0f);
    EXPECT_FLOAT_EQ(e.turbulenceSpeed, 0.5f);
    EXPECT_EQ(e.size, glm::vec2(0.1f, 0.2f));
    EXPECT_EQ(e.color, glm::vec4(1.0f));
    EXPECT_FLOAT_EQ(e.emissive, 1.0f);
    EXPECT_EQ(e.rotation, glm::vec2(0.0f, 360.0f));
    EXPECT_EQ(e.rotationSpeed, glm::vec2(0.0f));
    EXPECT_EQ(e.sprite, ParticleSprite::SoftCircle);
    EXPECT_EQ(e.atlasColumns, 1u);
    EXPECT_EQ(e.atlasRows, 1u);
    EXPECT_FLOAT_EQ(e.flipbookFps, 0.0f);
    EXPECT_FALSE(e.randomStartFrame);
    EXPECT_EQ(e.blend, ParticleBlend::Alpha);
    EXPECT_EQ(e.renderMode, ParticleRenderMode::Billboard);
    EXPECT_FLOAT_EQ(e.stretch, 0.05f);
    EXPECT_FALSE(e.lit);
    EXPECT_FLOAT_EQ(e.softDistance, 0.3f);
    EXPECT_FALSE(e.sort);
    EXPECT_EQ(e.collision, ParticleCollision::None);
    EXPECT_FLOAT_EQ(e.bounce, 0.4f);
    EXPECT_FLOAT_EQ(e.friction, 0.2f);

    const ParticleBurst b;
    EXPECT_EQ(b.count, 10u);
    EXPECT_EQ(b.cycles, 1u);
    EXPECT_FLOAT_EQ(b.interval, 1.0f);
}

// --- Шаг 4. Частицы: дым и искры на сущностях ---

TEST(GuideTranslucency, SmokeAndSparksEmitters) {
    registerAll();
    World world;

    Entity smoke = world.create("Smoke");
    smoke.setPosition({-0.4f, 0.05f, 0.0f});
    auto& s = smoke.add<ParticleEmitterComponent>();
    s.maxParticles = 512;
    s.spawnRate = 40.0f;
    s.lifetime = {2.5f, 3.5f};
    s.shape = ParticleShape::Sphere;
    s.radius = 0.25f;
    s.speed = {0.1f, 0.3f};
    s.velocity = {0.0f, 0.6f, 0.0f};         // подъём вверх
    s.drag = 0.4f;
    s.turbulence = 0.6f;                      // curl-noise, м/с²
    s.size = {0.5f, 0.8f};
    s.sizeOverLife = {{0.0f, 0.6f}, {1.0f, 2.2f}};
    s.colorOverLife = {{0.0f, {0.9f, 0.9f, 0.92f, 0.0f}},  // появление из прозрачности
                       {0.15f, {0.9f, 0.9f, 0.92f, 0.6f}},
                       {1.0f, {0.8f, 0.8f, 0.82f, 0.0f}}};
    s.sprite = ParticleSprite::Smoke;
    s.blend = ParticleBlend::Alpha;
    s.lit = true;                             // освещённый дым…
    s.emissive = 0.0f;                        // …без собственного свечения
    s.softDistance = 0.5f;
    s.sort = true;                            // back-to-front (≤ 2048 частиц)

    Entity sparks = world.create("Sparks");
    sparks.setPosition({0.6f, 1.05f, 0.4f});
    auto& k = sparks.add<ParticleEmitterComponent>();
    k.spawnRate = 0.0f;                       // только залпы
    k.bursts = {{.time = 0.0f, .count = 80, .cycles = 0, .interval = 0.5f}}; // 80 искр каждые 0.5 с, бесконечно
    k.lifetime = {0.8f, 1.4f};
    k.shape = ParticleShape::Cone;
    k.coneAngle = 40.0f;
    k.speed = {1.5f, 3.0f};
    k.gravityScale = 1.0f;
    k.size = {0.03f, 0.05f};
    k.color = {1.0f, 0.55f, 0.15f, 1.0f};
    k.colorOverLife = {{0.0f, {1, 1, 1, 1}}, {1.0f, {1.0f, 0.3f, 0.1f, 0.0f}}};
    k.emissive = 6.0f;                        // ярче сцены → попадут в bloom
    k.blend = ParticleBlend::Additive;
    k.renderMode = ParticleRenderMode::StretchedBillboard;
    k.stretch = 0.06f;
    k.sprite = ParticleSprite::Spark;
    k.collision = ParticleCollision::Bounce;  // отскок от буфера глубины
    k.bounce = 0.35f;

    // Компонент отражён: сохраняется в сцену и читается обратно.
    ParticleEmitterComponent copy;
    ASSERT_TRUE(serial::fromValue(serial::toValue(k), copy));
    EXPECT_EQ(copy.bursts.size(), 1u);
    EXPECT_EQ(copy.renderMode, ParticleRenderMode::StretchedBillboard);
    EXPECT_NE(ComponentRegistry::instance().find("ParticleEmitter"), nullptr);

    // Extract копирует включённые эмиттеры в расширение снапшота.
    world.updateTransforms();
    RenderSnapshot snap;
    extract(world, snap, {.time = 0.0, .deltaTime = 1.0f / 60.0f});
    const TranslucencySnapshot* t = snap.findExtension<TranslucencySnapshot>();
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->emitters.size(), 2u);

    smoke.get<ParticleEmitterComponent>().enabled = false; // выключенный эмиттер не extract'ится
    extract(world, snap);
    EXPECT_EQ(snap.findExtension<TranslucencySnapshot>()->emitters.size(), 1u);
}

// --- Шаг 3. Вода: компонент, волны Герстнера, «камера под водой» ---

TEST(GuideTranslucency, WaterSurfaceAndGerstnerWaves) {
    registerAll();
    World world;
    Entity lake = world.create("Lake");
    lake.setPosition({0.0f, 0.25f, 0.0f});    // базовая высота воды = Y сущности
    auto& w = lake.add<WaterSurfaceComponent>();
    w.size = {40.0f, 40.0f};                  // XZ вокруг сущности; <= 0 — бесконечная вода
    w.waves = {
        {.direction = {1.0f, 0.3f}, .wavelength = 6.0f, .amplitude = 0.06f, .steepness = 0.5f},
        {.direction = {0.6f, -0.8f}, .wavelength = 3.1f, .amplitude = 0.035f, .steepness = 0.5f, .phase = 1.1f},
        {.direction = {-0.2f, 1.0f}, .wavelength = 1.7f, .amplitude = 0.015f, .steepness = 0.4f, .phase = 2.3f},
    };
    w.absorption = {0.45f, 0.09f, 0.06f};     // красный гаснет первым → бирюзовая глубина
    w.shoreFoamDistance = 0.35f;
    w.causticsIntensity = 1.0f;
    w.underwaterDensity = 0.06f;

    world.updateTransforms();
    RenderSnapshot snap;
    extract(world, snap, {.time = 2.0});
    const TranslucencySnapshot* t = snap.findExtension<TranslucencySnapshot>();
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->water.size(), 1u);
    const SnapshotWater& water = t->water[0];
    EXPECT_FLOAT_EQ(water.params.info.x, 3.0f);  // число волн
    EXPECT_FLOAT_EQ(water.params.info.y, 0.25f); // базовая высота
    EXPECT_FLOAT_EQ(water.params.info.z, 2.0f);  // время

    // Та же математика, что в шейдере (world/gerstner.glsl) и в плавучести gameplay.
    const GerstnerParams p = packGerstnerWaves(w.waves, 0.25f, 2.0f);
    EXPECT_NEAR(gerstnerAmplitudeSum(p), 0.06f + 0.035f + 0.015f, 1e-6f);
    const f32 h = gerstnerHeight(p, {3.0f, -2.0f}, 2.0f);
    EXPECT_NEAR(h, 0.25f, gerstnerAmplitudeSum(p) + 1e-4f);

    // Камера под водой → рендер включает пост-эффект Underwater.
    const glm::vec3 cameraPos{3.0f, 0.1f, -2.0f};
    const bool underwater = cameraPos.y < gerstnerHeight(p, {cameraPos.x, cameraPos.z}, 2.0f);
    EXPECT_TRUE(underwater);

    // Без волн компонент даёт «спокойную зыбь» по умолчанию.
    w.waves.clear();
    extract(world, snap);
    EXPECT_GT(snap.findExtension<TranslucencySnapshot>()->water[0].params.info.x, 0.0f);
}

// --- cvar'ы и масштабируемость ---

TEST(GuideTranslucency, CVarsAndScalability) {
    registerAll();
    FeatureRegistry registry;
    registerTranslucencyFeatures(registry); // так делает Renderer::create (registerBuiltinFeatures)
    for (const char* n : {"Translucency", "Water", "Underwater", "Particles"}) EXPECT_NE(registry.find(n), nullptr) << n;

    // Способ отрисовки прозрачных объектов: Auto / OIT / Sorted.
    EXPECT_EQ(cvarText("r.Translucency.Method"), "Auto");
    CVarRegistry::instance().set("r.Translucency.Method", "OIT");
    EXPECT_EQ(cvarText("r.Translucency.Method"), "OIT");
    CVarRegistry::instance().set("r.Translucency.Method", "Auto");

    scalability::setGroup(Scalability::Effects, QualityLevel::Low);
    EXPECT_EQ(cvarText("r.Particles.Budget"), "16384");
    EXPECT_EQ(cvarText("r.Particles.ResolutionDivisor"), "4");
    EXPECT_EQ(cvarText("r.Water.Caustics"), "false");
    scalability::setGroup(Scalability::Effects, QualityLevel::Ultra);
    EXPECT_EQ(cvarText("r.Particles.Budget"), "1048576");
    EXPECT_EQ(cvarText("r.Particles.ResolutionDivisor"), "1");
    scalability::setGroup(Scalability::Reflections, QualityLevel::Low);
    EXPECT_EQ(cvarText("r.Water.SSRSteps"), "0");
    EXPECT_EQ(cvarText("r.Refraction.Mips"), "3");
    for (Scalability g : {Scalability::Effects, Scalability::Reflections}) scalability::setGroup(g, QualityLevel::High);
    EXPECT_EQ(cvarText("r.Particles.ResolutionDivisor"), "2");
}
