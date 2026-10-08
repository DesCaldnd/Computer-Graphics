// World-skinning area CPU tests: cvar scalability tables, reflected components, snapshot extension helpers.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/world/world_skinning.hpp>
#include <oxwald/scene/component_registry.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::render;

namespace {
f32 floatCVar(const char* name) { return std::stof(CVarRegistry::instance().find(name)->toString()); }
std::string strCVar(const char* name) { return CVarRegistry::instance().find(name)->toString(); }
} // namespace

TEST(WorldSkinning, CVarsHaveScalabilityTables) {
    registerWorldSkinningTypes(); // links the area's translation unit (cvars are static objects there)
    auto& reg = CVarRegistry::instance();
    for (const char* n : {"r.Terrain", "r.Terrain.LODScale", "r.Terrain.MaxLayers", "r.Terrain.Triplanar", "r.Terrain.Shadows",
                          "r.Terrain.Tessellation", "r.Foliage", "r.Foliage.Density", "r.Foliage.DrawDistanceScale",
                          "r.Foliage.ImpostorDistanceScale", "r.Foliage.Grass", "r.Foliage.Shadows", "r.Foliage.Impostors",
                          "r.Sky.AerialPerspective", "r.Sky.IBLUpdateDegrees", "r.Sky.CubeSize", "r.Skinning.Compute"}) {
        EXPECT_NE(reg.find(n), nullptr) << n;
    }
    scalability::setGroup(Scalability::Foliage, QualityLevel::Low);
    EXPECT_FLOAT_EQ(floatCVar("r.Foliage.Density"), 0.35f);
    EXPECT_EQ(strCVar("r.Foliage.Grass"), "false");
    EXPECT_FLOAT_EQ(floatCVar("r.Foliage.DrawDistanceScale"), 0.5f);
    scalability::setGroup(Scalability::Foliage, QualityLevel::Ultra);
    EXPECT_FLOAT_EQ(floatCVar("r.Foliage.Density"), 1.0f);
    EXPECT_FLOAT_EQ(floatCVar("r.Foliage.ImpostorDistanceScale"), 1.5f);
    scalability::setGroup(Scalability::ViewDistance, QualityLevel::Low);
    EXPECT_FLOAT_EQ(floatCVar("r.Terrain.LODScale"), 0.5f);
    scalability::setGroup(Scalability::Shading, QualityLevel::Ultra);
    EXPECT_EQ(strCVar("r.Terrain.MaxLayers"), "8");
    EXPECT_EQ(strCVar("r.Terrain.Tessellation"), "true");
    scalability::setGroup(Scalability::Shading, QualityLevel::Low);
    EXPECT_EQ(strCVar("r.Terrain.MaxLayers"), "2");
    EXPECT_EQ(strCVar("r.Terrain.Triplanar"), "false");
    for (Scalability g : {Scalability::Foliage, Scalability::ViewDistance, Scalability::Shading}) {
        scalability::setGroup(g, QualityLevel::High);
    }
    EXPECT_EQ(strCVar("r.Terrain.Tessellation"), "false");
}

TEST(WorldSkinning, ComponentsAreReflected) {
    registerWorldSkinningTypes();
    EXPECT_NE(reflect::TypeRegistry::instance().find("VegetationPrototypes"), nullptr);
    EXPECT_NE(reflect::TypeRegistry::instance().find("TerrainRender"), nullptr);
}

TEST(WorldSkinning, SkinningSnapshotClears) {
    RenderSnapshot s;
    s.extension<SkinningSnapshot>().methods[3] = GpuSkinningMethod::DualQuaternion;
    s.clear();
    ASSERT_NE(s.findExtension<SkinningSnapshot>(), nullptr);
    EXPECT_TRUE(s.findExtension<SkinningSnapshot>()->methods.empty());
}
