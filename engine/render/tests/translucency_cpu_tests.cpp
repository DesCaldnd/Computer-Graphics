// Translucency area CPU tests: component reflection/registry, extract hook into the snapshot extension, water
// bridge, cvar scalability tables, feature registration.
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

using namespace ox;
using namespace ox::render;

namespace {

void registerAll() {
    registerSceneTypes();
    registerTranslucencyTypes();
}

i32 intCVar(const char* name) { return std::stoi(CVarRegistry::instance().find(name)->toString()); }

} // namespace

TEST(Translucency, ComponentsAreRegisteredAndSerialize) {
    registerAll();
    EXPECT_NE(ComponentRegistry::instance().find("ParticleEmitter"), nullptr);
    EXPECT_NE(ComponentRegistry::instance().find("WaterSurface"), nullptr);

    ParticleEmitterComponent e;
    e.maxParticles = 333;
    e.shape = ParticleShape::Cone;
    e.blend = ParticleBlend::Additive;
    e.bursts = {{0.5f, 12, 3, 0.25f}};
    e.colorOverLife = {{0.0f, {1, 0, 0, 1}}, {1.0f, {0, 0, 1, 0}}};
    ParticleEmitterComponent back;
    ASSERT_TRUE(serial::fromValue(serial::toValue(e), back));
    EXPECT_EQ(back.maxParticles, 333u);
    EXPECT_EQ(back.shape, ParticleShape::Cone);
    EXPECT_EQ(back.blend, ParticleBlend::Additive);
    ASSERT_EQ(back.bursts.size(), 1u);
    EXPECT_EQ(back.bursts[0].count, 12u);
    ASSERT_EQ(back.colorOverLife.size(), 2u);
    EXPECT_FLOAT_EQ(back.colorOverLife[1].color.b, 1.0f);

    WaterSurfaceComponent w;
    w.waves = {{{0.0f, 1.0f}, 4.0f, 0.1f, 0.3f, 0.0f, 1.0f}};
    w.absorption = {0.3f, 0.1f, 0.05f};
    WaterSurfaceComponent wb;
    ASSERT_TRUE(serial::fromValue(serial::toValue(w), wb));
    ASSERT_EQ(wb.waves.size(), 1u);
    EXPECT_FLOAT_EQ(wb.waves[0].wavelength, 4.0f);
    EXPECT_FLOAT_EQ(wb.absorption.x, 0.3f);
}

TEST(Translucency, ExtractHookFillsSnapshotExtension) {
    registerAll();
    World world;
    Entity a = world.create("Sparks");
    a.setPosition({1.0f, 2.0f, 3.0f});
    a.add<ParticleEmitterComponent>().maxParticles = 77;
    Entity off = world.create("Off");
    off.add<ParticleEmitterComponent>().enabled = false;
    Entity lake = world.create("Lake");
    lake.setPosition({5.0f, -1.5f, 7.0f});
    lake.add<WaterSurfaceComponent>().size = {20.0f, 10.0f};
    world.updateTransforms();

    RenderSnapshot snap;
    extract(world, snap, {.time = 2.0});
    const TranslucencySnapshot* t = snap.findExtension<TranslucencySnapshot>();
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->emitters.size(), 1u);
    EXPECT_EQ(t->emitters[0].emitter.maxParticles, 77u);
    EXPECT_FLOAT_EQ(t->emitters[0].world[3].y, 2.0f);
    ASSERT_EQ(t->water.size(), 1u);
    const SnapshotWater& w = t->water[0];
    EXPECT_FLOAT_EQ(w.params.info.y, -1.5f);  // base height = entity Y
    EXPECT_FLOAT_EQ(w.params.info.z, 2.0f);   // time
    EXPECT_GT(w.params.info.x, 0.0f);         // default swell when no waves are given
    EXPECT_EQ(w.center, glm::vec2(5.0f, 7.0f));
    EXPECT_EQ(w.size, glm::vec2(20.0f, 10.0f));

    // Re-extracting clears the previous frame's data (extension keeps its capacity).
    world.destroyImmediate(a);
    extract(world, snap);
    EXPECT_TRUE(snap.findExtension<TranslucencySnapshot>()->emitters.empty());

    // Bridges (e.g. gameplay::WorldRenderData water) append after extract.
    SnapshotWater extra;
    extra.params = packGerstnerWaves({{{1.0f, 0.0f}, 10.0f, 0.2f, 0.5f, 0.0f, 1.0f}}, 0.0f, 0.0f);
    addWaterSurface(snap, extra);
    EXPECT_EQ(snap.findExtension<TranslucencySnapshot>()->water.size(), 2u);
}

TEST(Translucency, FeaturesRegisterWithScalabilityTables) {
    registerAll();
    FeatureRegistry registry;
    registerTranslucencyFeatures(registry);
    for (const char* n : {"Translucency", "Water", "Underwater", "Particles"}) EXPECT_NE(registry.find(n), nullptr) << n;

    scalability::setGroup(Scalability::Effects, QualityLevel::Low);
    EXPECT_EQ(intCVar("r.Particles.ResolutionDivisor"), 4);
    EXPECT_EQ(intCVar("r.Particles.Budget"), 16384);
    scalability::setGroup(Scalability::Effects, QualityLevel::Ultra);
    EXPECT_EQ(intCVar("r.Particles.ResolutionDivisor"), 1);
    scalability::setGroup(Scalability::Reflections, QualityLevel::Low);
    EXPECT_EQ(intCVar("r.Water.SSRSteps"), 0);
    EXPECT_EQ(intCVar("r.Refraction.Mips"), 3);
    scalability::setGroup(Scalability::Shading, QualityLevel::Medium);
    EXPECT_EQ(intCVar("r.Water.GridResolution"), 160);
    for (Scalability g : {Scalability::Effects, Scalability::Reflections, Scalability::Shading}) {
        scalability::setGroup(g, QualityLevel::High);
    }
    EXPECT_EQ(intCVar("r.Particles.ResolutionDivisor"), 2);
}
