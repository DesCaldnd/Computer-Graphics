// Глава 28: CPU-часть рендера мира — cvar'ы по группам, компоненты рендера, WorldSnapshot и троттлинг IBL неба.
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/world/world_skinning.hpp>
#include <oxwald/world/time_of_day.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::render;

namespace {
std::string cvar(const char* name) { return CVarRegistry::instance().find(name)->toString(); }

world::SkyState skyAt(f64 localHours) {
    world::TimeOfDay tod({.location = {52.37, 4.90}, .year = 2024, .month = 6, .day = 21, .localHours = localHours,
                          .utcOffsetHours = 2.0, .timeScale = 0.0, .paused = true});
    return tod.state();
}

// SkyState мира → то, что рисует WorldSky (в рантайме это делает мост gameplay → render).
WorldSkySnapshot skyFromState(const world::SkyState& s) {
    WorldSkySnapshot k;
    k.valid = true;
    k.preetham = s.preetham.toGpu();
    k.sunDirection = s.sunDirection;
    k.moonDirection = s.moonDirection;
    k.moonPhase = s.moonPhase;
    k.starsRotation = s.starsRotation;
    k.atmosphere = s.atmosphere;
    k.sunLight = s.sunLight;
    k.moonLight = s.moonLight;
    k.starsIntensity = s.atmosphere.starsIntensity;
    return k;
}
} // namespace

TEST(WorldRenderCpu, QualityGroupsDriveTerrainAndFoliage) {
    registerWorldSkinningTypes(); // подтягивает cvar'ы области (Renderer::create делает это сам)
    scalability::setGroup(Scalability::Foliage, QualityLevel::Low);
    EXPECT_EQ(cvar("r.Foliage.Grass"), "false");
    EXPECT_EQ(cvar("r.Foliage.Density"), "0.35");
    scalability::setGroup(Scalability::Shading, QualityLevel::Medium);
    EXPECT_EQ(cvar("r.Terrain.MaxLayers"), "3");
    scalability::setGroup(Scalability::Shading, QualityLevel::Ultra);
    EXPECT_EQ(cvar("r.Terrain.Tessellation"), "true"); // нужен DeviceCaps::tessellationShader
    scalability::setGroup(Scalability::ViewDistance, QualityLevel::Low);
    EXPECT_EQ(cvar("r.Terrain.LODScale"), "0.5");
    scalability::setOverall(QualityLevel::High);
    EXPECT_EQ(cvar("r.Terrain.Tessellation"), "false");
}

TEST(WorldRenderCpu, RenderComponentsAreReflected) {
    registerWorldSkinningTypes();
    EXPECT_NE(reflect::TypeRegistry::instance().find("TerrainRender"), nullptr);
    EXPECT_NE(reflect::TypeRegistry::instance().find("VegetationPrototypes"), nullptr);

    TerrainRenderComponent look; // на сущности с gameplay TerrainComponent
    look.triplanarSlopeDeg = 40.0f;
    look.layerTileMeters = 6.0f;
    VegetationPrototypeDesc birch; // прототип 0 слоёв растительности
    birch.prototype = 0;
    birch.name = "Birch";
    birch.windSway = 0.6f;
    VegetationPrototypesComponent protos;
    protos.prototypes.push_back(birch);
    EXPECT_TRUE(protos.prototypes[0].impostor); // импостор печётся из последнего LOD
    EXPECT_TRUE(protos.prototypes[0].lods.empty()); // пусто = встроенный процедурный меш по типу слоя
}

TEST(WorldRenderCpu, SkyIblIsThrottledAndEnvironmentAdded) {
    const WorldSkySnapshot noon = skyFromState(skyAt(12.0));
    const WorldSkySnapshot halfMinute = skyFromState(skyAt(12.0 + 0.5 / 60.0)); // солнце сдвинулось ~0.1°
    const WorldSkySnapshot later = skyFromState(skyAt(12.5));                   // на несколько градусов
    EXPECT_EQ(worldSkyIblKey(noon, 1.0f), worldSkyIblKey(halfMinute, 1.0f));  // куб неба не перерисовывается
    EXPECT_NE(worldSkyIblKey(noon, 1.0f), worldSkyIblKey(later, 1.0f));

    RenderSnapshot snapshot;
    WorldSnapshot& w = snapshot.extension<WorldSnapshot>();
    w.sky = noon;
    finalizeWorldSnapshot(snapshot); // после заполнения: настройки ландшафтов, iblKey, окружение для неба
    ASSERT_TRUE(snapshot.environment.has_value());
    EXPECT_TRUE(snapshot.environment->iblKey.has_value());
}
