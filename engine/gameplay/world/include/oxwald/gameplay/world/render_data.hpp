#pragma once

// Renderer contract of the world integration. WorldRenderData is a service filled on the game thread by the
// `Gameplay.World.Extract` system (Extract phase). The renderer reads it in IRenderer::extract() right after the
// scheduler tick (same thread) and copies what it needs; shared_ptr members point at runtime-owned CPU data that
// may be modified by the game thread after extract returns (brush edits), so the render thread must copy the
// dirty rectangles / instance buffers instead of keeping raw pointers into them.
//
// Conventions: world space metres, Y up, north = -Z, east = +X. Directions point *towards* the light source.
// Entity ids are gameplay::toRuntimeId(entity) (0 = none); they are stable while the entity lives.

#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/world/common.hpp>
#include <oxwald/world/heightfield.hpp>
#include <oxwald/world/sky.hpp>
#include <oxwald/world/splat_map.hpp>
#include <oxwald/world/terrain_lod.hpp>
#include <oxwald/world/time_of_day.hpp>
#include <oxwald/world/vegetation.hpp>
#include <oxwald/world/water.hpp>
#include <oxwald/world/weather.hpp>

#include <glm/mat4x4.hpp>

#include <memory>
#include <string>
#include <vector>

namespace ox::gameplay {

struct TerrainRenderItem {
    u64 entity = 0;
    // Height samples (normalized; world = desc.heightOffset + h * desc.heightScale, origin/worldSize in desc).
    // GPU: Heightfield::rawBytes() as R32_SFLOAT (Float32) or R16_UNORM (UNorm16), or extractR16(rect).
    std::shared_ptr<const world::Heightfield> heightfield;
    u64 heightfieldVersion = 0; // bumps on rebuild and on every brush edit
    // Samples changed since the previous extract (sample coords, half-open). fullUpload = re-create the texture
    // (resolution/format may have changed); otherwise upload only dirtyRect (may be empty).
    world::IRect dirtyRect{};
    bool fullUpload = false;
    // Hole mask (Heightfield::holeMask, 1 byte per sample) is part of the heightfield data.

    // Splat weights (null when the terrain has no layers): two RGBA8 textures, SplatMap::packRgba8(0|1, rect).
    std::shared_ptr<const world::SplatMap> splat;
    u64 splatVersion = 0;
    world::IRect splatDirtyRect{};
    bool splatFullUpload = false;
    std::vector<Uuid> layerMaterials; // material asset per splat layer (index = layer)

    // CDLOD: instanced grid mesh (shared between terrains with the same leafNodeSize; version changes only when
    // the mesh object changes), quadtree for re-selection from other views (shadow cascades: tree->select with
    // the cascade frustum), skirt depth per LOD (metres, push skirt vertices down) and the patches selected for the
    // primary camera (32 B std430 each, see world::TerrainPatchGpu).
    std::shared_ptr<const world::TerrainGridMesh> gridMesh;
    u64 gridMeshVersion = 0;
    std::shared_ptr<const world::TerrainQuadtree> quadtree;
    std::vector<f32> skirtDepth;
    std::vector<world::TerrainPatchGpu> patches;
    world::LodRanges lodRanges;
    world::Aabb bounds;
};

// Per vegetation layer: what the renderer needs to pick meshes/materials (prototype index) and LODs.
struct VegetationPrototypeInfo {
    std::string name;
    u16 layer = 0;
    u16 prototype = 0;
    world::VegetationKind kind = world::VegetationKind::Grass;
    f32 boundingRadius = 1.f;
    world::VegetationLodSettings lod{};
    bool castsShadow = false; // trees
};

// Instances of one scattered (or streamed) chunk. 64-byte world::VegetationInstanceGpu: rows of the 3x4
// object->world matrix (VkTransformMatrixKHR layout), packed tint, random, prototype|layer<<16|flags, radius.
// cells index into the instance array (first/count) with world AABBs for CPU pre-culling.
struct VegetationBatch {
    glm::vec2 origin{0.f};
    f32 size = 0.f;
    u64 version = 0; // changes when the instance array object changes (re-upload)
    std::shared_ptr<const std::vector<world::VegetationInstanceGpu>> instances;
    std::shared_ptr<const std::vector<world::VegetationCell>> cells;
};

struct VegetationRenderItem {
    u64 entity = 0;
    std::vector<VegetationPrototypeInfo> layers; // index = instance layer
    std::vector<VegetationBatch> batches;        // chunks near the camera / streaming sources
};

struct SkyRenderData {
    bool valid = false;               // a SkyComponent exists
    world::PreethamSky::Gpu preetham{}; // 128 B std140 for world/preetham.glsl
    glm::vec3 sunDirection{0.f, 1.f, 0.f};
    glm::vec3 moonDirection{0.f, 1.f, 0.f};
    world::MoonPhase moonPhase{};
    glm::quat starsRotation{1.f, 0.f, 0.f, 0.f}; // celestial -> world (rotate the star cubemap)
    world::AtmosphereParams atmosphere{};        // fog density (1/m), ambient, exposure (EV), stars, colours (linear)
    world::CelestialLight sunLight{};            // illuminance lux, colour normalised to max 1
    world::CelestialLight moonLight{};
    glm::vec3 mainLightDirection{0.f, 1.f, 0.f}; // dominant light (sun by day, moon by night), towards the light
    glm::vec3 mainLightColor{1.f};
    f32 mainLightIlluminance = 0.f; // lux
    bool isDay = true;
    f64 localHours = 12.0;
    // SkyComponent parameters.
    f32 skyIntensity = 1.f;
    bool sunDisc = true;
    f32 sunDiscIntensity = 1.f;
    f32 sunAngularDiameterDeg = 0.53f;
    bool moon = true;
    f32 moonIntensity = 1.f;
    f32 moonAngularDiameterDeg = 0.52f;
    bool stars = true;
    f32 starsIntensity = 1.f; // already multiplied by atmosphere.starsIntensity
};

struct WaterRenderItem {
    u64 entity = 0;
    world::GerstnerParamsGpu params{}; // 528 B std140; info = (count, base height, time = game time, 0)
    glm::mat4 transform{1.f};          // entity world matrix (base plane at its Y)
    glm::vec2 size{0.f};               // XZ extent centred on the entity, <= 0 = unbounded
};

class WorldRenderData {
public:
    u64 frame = 0;   // scheduler frame of the extract
    u64 version = 0; // increments on every extract
    f64 time = 0.0;  // game time in seconds (water, wind)
    f32 aspect = 16.f / 9.f; // aspect used for the primary camera projection (set by the renderer)

    // Primary camera used for terrain/vegetation selection (CameraComponent::primary, else the first camera).
    bool hasCamera = false;
    u64 cameraEntity = 0;
    glm::vec3 cameraPosition{0.f};
    glm::mat4 view{1.f};
    glm::mat4 projection{1.f}; // reversed-Z (CameraComponent::projectionMatrix(aspect))

    std::vector<TerrainRenderItem> terrains;
    std::vector<VegetationRenderItem> vegetation;
    SkyRenderData sky;
    std::vector<WaterRenderItem> water;
    bool hasWind = false;
    world::WindGpu wind{}; // 32 B std140 for world/wind.glsl (time = game time)
    bool hasWeather = false;
    world::WeatherState weather{};

    void clear() {
        hasCamera = false;
        terrains.clear();
        vegetation.clear();
        sky = {};
        water.clear();
        hasWind = false;
        wind = {};
        hasWeather = false;
        weather = {};
    }
};

} // namespace ox::gameplay
