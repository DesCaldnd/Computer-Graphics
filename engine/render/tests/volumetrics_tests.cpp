// Volumetrics CPU tests: froxel depth distribution, cvar/scalability tables, component reflection and extract.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>
#include <oxwald/render/register_types.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::render;

TEST(VolumetricsGrid, DepthDistributionRoundTrip) {
    volumetrics::FroxelGrid g;
    g.farDistance = 128.0f;
    g.depthScale = 32.0f;
    EXPECT_NEAR(g.sliceToDepth(0.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(g.sliceToDepth(1.0f), 128.0f, 1e-2f);
    f32 prev = -1.0f;
    for (int i = 0; i <= 64; ++i) {
        const f32 s = f32(i) / 64.0f;
        const f32 d = g.sliceToDepth(s);
        EXPECT_GT(d, prev);
        EXPECT_NEAR(g.depthToSlice(d), s, 1e-4f);
        prev = d;
    }
    // Exponential: slices get thicker with distance.
    const f32 first = g.sliceToDepth(1.0f / 64.0f) - g.sliceToDepth(0.0f);
    const f32 last = g.sliceToDepth(1.0f) - g.sliceToDepth(63.0f / 64.0f);
    EXPECT_GT(last, first * 4.0f);
    EXPECT_LT(first, 0.5f);
}

TEST(VolumetricsGrid, ScalabilityTables) {
    registerRenderTypes();
    scalability::setGroup(Scalability::Volumetrics, QualityLevel::Low);
    volumetrics::FroxelGrid low = volumetrics::froxelGridFromCVars();
    EXPECT_EQ(low.z, 32u);
    EXPECT_EQ(CVarRegistry::instance().find("r.VolumetricClouds")->toString(), "false");
    scalability::setGroup(Scalability::Volumetrics, QualityLevel::High);
    volumetrics::FroxelGrid high = volumetrics::froxelGridFromCVars();
    EXPECT_EQ(high.x, 160u);
    EXPECT_EQ(high.y, 90u);
    EXPECT_EQ(high.z, 64u);
    EXPECT_EQ(CVarRegistry::instance().find("r.VolumetricClouds")->toString(), "true");
    scalability::setGroup(Scalability::Volumetrics, QualityLevel::Ultra);
    EXPECT_GT(volumetrics::froxelGridFromCVars().froxelCount(), high.froxelCount());
    scalability::setGroup(Scalability::Volumetrics, QualityLevel::High);
}

TEST(VolumetricsTypes, ReflectedAndExtracted) {
    registerSceneTypes();
    registerRenderTypes();
    EXPECT_NE(ComponentRegistry::instance().find("FogVolume"), nullptr);
    EXPECT_NE(ComponentRegistry::instance().find("CloudLayer"), nullptr);
    EXPECT_NE(ComponentRegistry::instance().find("VolumetricFog"), nullptr);

    World world;
    Entity v = world.create("Fog");
    v.setPosition({1, 2, 3});
    auto& fv = v.add<FogVolumeComponent>();
    fv.shape = FogVolumeShape::Sphere;
    fv.density = 0.5f;
    Entity hidden = world.create("Hidden");
    hidden.add<FogVolumeComponent>();
    hidden.setActive(false);
    world.create("Clouds").add<CloudLayerComponent>().coverage = 0.7f;
    world.create("Settings").add<VolumetricFogComponent>().distance = 64.0f;
    world.updateTransforms();

    RenderSnapshot snap;
    extract(world, snap);
    const auto* ext = snap.findExtension<volumetrics::VolumetricsSnapshot>();
    ASSERT_NE(ext, nullptr);
    ASSERT_EQ(ext->volumes.size(), 1u);
    EXPECT_EQ(ext->volumes[0].volume.shape, FogVolumeShape::Sphere);
    EXPECT_FLOAT_EQ(ext->volumes[0].world[3].y, 2.0f);
    ASSERT_TRUE(ext->clouds.has_value());
    EXPECT_FLOAT_EQ(ext->clouds->coverage, 0.7f);
    EXPECT_TRUE(volumetrics::fogActive(snap));
    EXPECT_FLOAT_EQ(volumetrics::froxelGridFromCVars(&snap).farDistance, 64.0f);

    volumetrics::setWorldWind(snap, {3, 0, 1});
    EXPECT_TRUE(ext->hasWind);
    extract(world, snap); // a new extract clears the world inputs of the previous frame
    EXPECT_FALSE(snap.findExtension<volumetrics::VolumetricsSnapshot>()->hasWind);

    // Serialization round trip through reflection.
    const serial::Value value = serial::toValue(fv);
    FogVolumeComponent copy;
    EXPECT_TRUE(serial::fromValue(value, copy));
    EXPECT_EQ(copy.shape, FogVolumeShape::Sphere);
    EXPECT_FLOAT_EQ(copy.density, 0.5f);
}
