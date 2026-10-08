// World-skinning area: cvars (with scalability tables), feature registration, reflected components, the render-only
// extract hook and the WorldSnapshot helpers.
#include "world_internal.hpp"

#if OX_RENDER_HAS_WORLD
#include "world_geometry.hpp"
#endif

#include <oxwald/core/hash.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/render/components/world.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/world.hpp>

#include <cmath>
#include <mutex>

namespace ox::render {

namespace {

using S = Scalability;

// --- terrain (ViewDistance: LOD ranges; Shading: layers / triplanar) ---
CVar<bool> cvTerrain("r.Terrain", true, "Draw terrains");
CVar<float> cvTerrainLodScale("r.Terrain.LODScale", 1.0f, "Scales every CDLOD range (distance of terrain detail)",
                              S::ViewDistance, {0.5f, 0.75f, 1.0f, 1.5f});
CVar<int> cvTerrainMaxLayers("r.Terrain.MaxLayers", 8, "Splat layers blended per pixel (strongest first)", S::Shading,
                             {2, 3, 4, 8});
CVar<bool> cvTerrainTriplanar("r.Terrain.Triplanar", true, "Triplanar projection on steep terrain slopes", S::Shading,
                              {false, true, true, true});
CVar<bool> cvTerrainShadows("r.Terrain.Shadows", true, "Terrain casts sun (cascade) shadows");
CVar<bool> cvTerrainTess("r.Terrain.Tessellation", false,
                         "Tessellated detail displacement near the camera (needs DeviceCaps::tessellationShader)",
                         S::Shading, {false, false, false, true});

// --- vegetation (Foliage) ---
CVar<bool> cvFoliage("r.Foliage", true, "Draw vegetation");
CVar<float> cvFoliageDensity("r.Foliage.Density", 1.0f, "Fraction of grass / detail instances drawn", S::Foliage,
                             {0.35f, 0.6f, 0.85f, 1.0f});
CVar<float> cvFoliageDistance("r.Foliage.DrawDistanceScale", 1.0f, "Scales vegetation LOD and cull distances",
                              S::Foliage, {0.5f, 0.75f, 1.0f, 1.5f});
CVar<float> cvFoliageImpostorDistance("r.Foliage.ImpostorDistanceScale", 1.0f,
                                      "Scales the mesh → impostor switch distance of trees", S::Foliage,
                                      {0.5f, 0.75f, 1.0f, 1.5f});
CVar<bool> cvFoliageGrass("r.Foliage.Grass", true, "Draw grass layers", S::Foliage, {false, true, true, true});
CVar<bool> cvFoliageShadows("r.Foliage.Shadows", true, "Trees cast cascade shadows", S::Foliage, {false, true, true, true});
CVar<bool> cvFoliageImpostors("r.Foliage.Impostors", true, "Octahedral impostors for distant trees");

// --- sky ---
CVar<bool> cvAerial("r.Sky.AerialPerspective", true, "Aerial perspective (skipped when volumetric fog is active)",
                    S::Volumetrics, {false, true, true, true});
CVar<float> cvAerialDensity("r.Sky.AerialPerspective.Density", 1.0f, "Multiplier of the clear-air extinction", 0.0f, 100.0f);
CVar<float> cvIblDegrees("r.Sky.IBLUpdateDegrees", 1.0f, "Sun / moon motion (degrees) that refreshes the sky IBL", 0.05f,
                         45.0f);
CVar<int> cvSkyCube("r.Sky.CubeSize", 128, "World sky radiance cube face size (IBL source, aerial perspective)",
                    S::Shading, {64, 128, 128, 256});

// --- skinning ---
CVar<bool> cvSkinCompute("r.Skinning.Compute", true,
                         "Compute skinning into per-instance vertex buffers (else vertex-shader skinning; dual "
                         "quaternion skinning needs it)");

} // namespace

namespace worldfx {

WorldCVars WorldCVars::read() {
    WorldCVars c;
    c.terrain = cvTerrain;
    c.terrainLodScale = cvTerrainLodScale;
    c.terrainMaxLayers = cvTerrainMaxLayers;
    c.terrainTriplanar = cvTerrainTriplanar;
    c.terrainShadows = cvTerrainShadows;
    c.terrainTessellation = cvTerrainTess;
    c.foliage = cvFoliage;
    c.foliageDensity = cvFoliageDensity;
    c.foliageDrawDistanceScale = cvFoliageDistance;
    c.foliageImpostorDistanceScale = cvFoliageImpostorDistance;
    c.foliageGrass = cvFoliageGrass;
    c.foliageShadows = cvFoliageShadows;
    c.foliageImpostors = cvFoliageImpostors;
    c.aerialPerspective = cvAerial;
    c.aerialDensity = cvAerialDensity;
    c.iblUpdateDegrees = cvIblDegrees;
    c.skyCubeSize = cvSkyCube;
    c.computeSkinning = cvSkinCompute;
    return c;
}

std::vector<std::string> worldGeometryCVarNames() {
    return {"r.Terrain", "r.Terrain.LODScale", "r.Terrain.MaxLayers", "r.Terrain.Triplanar", "r.Terrain.Tessellation", "r.Foliage",
            "r.Foliage.Density", "r.Foliage.DrawDistanceScale", "r.Foliage.ImpostorDistanceScale", "r.Foliage.Grass",
            "r.Foliage.Impostors"};
}
std::vector<std::string> worldSkyCVarNames() {
    return {"r.Sky", "r.Sky.AerialPerspective", "r.Sky.AerialPerspective.Density", "r.Sky.IBLUpdateDegrees", "r.Sky.CubeSize"};
}
std::vector<std::string> skinningCVarNames() { return {"r.Skinning.Compute"}; }

u32 packOctUnorm(glm::vec3 n) {
    n /= std::max(std::abs(n.x) + std::abs(n.y) + std::abs(n.z), 1e-6f);
    glm::vec2 e(n.x, n.y);
    if (n.z < 0.0f) {
        e = (1.0f - glm::abs(glm::vec2(n.y, n.x))) * glm::vec2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    }
    e = glm::clamp(e * 0.5f + 0.5f, 0.0f, 1.0f);
    return u32(std::lround(e.x * 65535.0f)) | (u32(std::lround(e.y * 65535.0f)) << 16);
}

void extractWorldComponents(const World& world, RenderSnapshot& out) {
#if OX_RENDER_HAS_WORLD
    const entt::registry& reg = world.registry();
    auto protos = reg.view<const VegetationPrototypesComponent>();
    auto terrains = reg.view<const TerrainRenderComponent>();
    if (protos.begin() == protos.end() && terrains.begin() == terrains.end()) return;
    WorldSnapshot& ws = out.extension<WorldSnapshot>();
    for (auto [e, c] : protos.each()) {
        for (const VegetationPrototypeDesc& d : c.prototypes) ws.prototypes.push_back(d);
    }
    for (auto [e, c] : terrains.each()) ws.terrainSettings[encodeEntityId(u32(entt::to_integral(e)))] = c;
#else
    (void)world;
    (void)out;
#endif
}

} // namespace worldfx

// --- reflection -------------------------------------------------------------------------------------------------

void registerWorldSkinningTypes() {
    static std::once_flag once;
    std::call_once(once, [] {
        using attr::AssetRef;
        using attr::Category;
        using attr::Range;
        using attr::Tooltip;
        OX_REFLECT_TYPE(VegetationPrototypeDesc, "VegetationPrototypeDesc")
            .field("prototype", &VegetationPrototypeDesc::prototype, Tooltip{"world::VegetationLayer::prototype it renders"})
            .field("name", &VegetationPrototypeDesc::name)
            .field("lods", &VegetationPrototypeDesc::lods, AssetRef{"Mesh"},
                   Tooltip{"Mesh per LOD 0..2; empty = built-in mesh for the layer kind"})
            .field("material", &VegetationPrototypeDesc::material, AssetRef{"Material"})
            .field("impostor", &VegetationPrototypeDesc::impostor)
            .field("windSway", &VegetationPrototypeDesc::windSway, Range{0.0, 10.0})
            .field("windFlutter", &VegetationPrototypeDesc::windFlutter, Range{0.0, 10.0})
            .field("translucency", &VegetationPrototypeDesc::translucency, Range{0.0, 4.0})
            .field("castShadows", &VegetationPrototypeDesc::castShadows);
        OX_REFLECT_TYPE(VegetationPrototypesComponent, "VegetationPrototypes")
            .attributes(Category{"World"})
            .field("prototypes", &VegetationPrototypesComponent::prototypes);
        OX_REFLECT_TYPE(TerrainRenderComponent, "TerrainRender")
            .attributes(Category{"World"})
            .field("triplanarSlopeDeg", &TerrainRenderComponent::triplanarSlopeDeg, Range{1.0, 89.0})
            .field("heightBlend", &TerrainRenderComponent::heightBlend, Range{0.0, 2.0})
            .field("macroVariation", &TerrainRenderComponent::macroVariation, Range{0.0, 1.0})
            .field("tilingBreakup", &TerrainRenderComponent::tilingBreakup, Range{0.0, 1.0})
            .field("layerTileMeters", &TerrainRenderComponent::layerTileMeters, Range{0.05, 1000.0})
            .field("castShadows", &TerrainRenderComponent::castShadows)
            .field("tessellationHeight", &TerrainRenderComponent::tessellationHeight, Range{0.0, 2.0});
        ComponentRegistry& reg = ComponentRegistry::instance();
        reg.add<VegetationPrototypesComponent>();
        reg.add<TerrainRenderComponent>();
    });
}

// --- WorldSnapshot ----------------------------------------------------------------------------------------------

#if OX_RENDER_HAS_WORLD

void WorldSnapshot::clear() {
    time = 0.0;
    terrains.clear();
    vegetation.clear();
    prototypes.clear();
    terrainSettings.clear();
    sky = {};
    hasWind = false;
    wind = {};
    interactors.clear();
}

u64 worldSkyIblKey(const WorldSkySnapshot& sky, f32 degrees) {
    if (!sky.valid) return 0;
    const f32 step = std::sin(glm::radians(std::max(degrees, 0.01f)));
    auto qdir = [&](glm::vec3 d) { return glm::ivec3(glm::round(d / step)); };
    auto qlog = [](f32 v) { return i32(std::lround(std::log2(std::max(v, 1e-6f)) * 8.0f)); };
    const glm::ivec3 s = qdir(sky.sunDirection), m = qdir(sky.moonDirection);
    const i32 values[10] = {s.x, s.y, s.z, m.x, m.y, m.z, qlog(sky.sunLight.illuminance), qlog(sky.moonLight.illuminance),
                            qlog(sky.skyIntensity), qlog(sky.moonIntensity)};
    u64 h = fnv1a64(std::as_bytes(std::span(values)));
    return h == 0 ? 1 : h;
}

void finalizeWorldSnapshot(RenderSnapshot& snapshot) {
    WorldSnapshot* ws = snapshot.extensions.count(std::type_index(typeid(WorldSnapshot)))
                            ? &snapshot.extension<WorldSnapshot>()
                            : nullptr;
    if (!ws) return;
    for (TerrainSnapshot& t : ws->terrains) {
        auto it = ws->terrainSettings.find(t.entityId);
        if (it != ws->terrainSettings.end()) t.settings = it->second;
    }
    if (!ws->sky.valid) return;
    if (!snapshot.environment) {
        SnapshotEnvironment env;
        for (usize i = 0; i < snapshot.lights.size(); ++i) {
            if (snapshot.lights[i].light.type == LightType::Directional &&
                (env.sunLight < 0 || snapshot.lights[i].light.intensity > snapshot.lights[usize(env.sunLight)].light.intensity)) {
                env.sunLight = i32(i);
            }
        }
        snapshot.environment = env;
    }
    snapshot.environment->iblKey = worldSkyIblKey(ws->sky, worldfx::WorldCVars::read().iblUpdateDegrees);
}

#endif

// --- registration -----------------------------------------------------------------------------------------------

void registerWorldSkinningFeatures(FeatureRegistry& features) {
    registerWorldSkinningTypes();
    static std::once_flag hooks;
    std::call_once(hooks, [] {
        addExtractHook(&worldfx::extractWorldComponents);
        worldfx::registerGameplayBridge();
    });
#if OX_RENDER_HAS_WORLD
    auto shared = std::make_shared<worldfx::WorldGeometryShared>();
    features.add(worldfx::makeWorldGeometryFeature(shared));
    features.add(worldfx::makeWorldShadowsFeature(shared));
    features.add(worldfx::makeWorldSkyFeature());
#endif
    features.add(worldfx::makeSkinningFeature());
}

} // namespace ox::render
