#pragma once

// ECS components of the world integration (terrain, vegetation, sky, time of day, water, wind/weather, buoyancy,
// streaming). Plain serializable settings only: every derived/runtime object (heightfields, quadtrees, splat maps,
// vegetation chunks, physics bodies, TimeOfDay/WeatherController, chunk streamers) lives in the WorldRuntime
// service keyed by entity, so World::clone() / save / load never carry stale runtime state. Runtime mirrors are
// NoSerialize + ReadOnly. Reflected and registered by ox::registerWorldGameplayTypes() (category "World").

#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/scene/entity_ref.hpp>
#include <oxwald/world/streaming.hpp>
#include <oxwald/world/terrain_gen.hpp>
#include <oxwald/world/terrain_lod.hpp>
#include <oxwald/world/time_of_day.hpp>
#include <oxwald/world/vegetation.hpp>
#include <oxwald/world/water.hpp>
#include <oxwald/world/weather.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace ox::gameplay {

enum class TerrainSource : u8 {
    Procedural, // fractal noise (+ optional erosion) from `noise`
    Heightmap,  // heightmap asset `heightmap` resolved through IHeightmapProvider (resolution from the asset)
    Flat,       // all samples 0 (height = heightOffset + entity Y), e.g. a sculpting start point
    External,   // data injected through WorldRuntime::setExternalTerrain (streamed chunk tiles); desc fields ignored
};

// Heightfield terrain. Placement: heights are `entityWorldY + heightOffset + normalized * heightScale`; the sample
// grid covers `worldSize` metres along X and Z, centred on the entity's world XZ when `centered`, otherwise its
// minimum corner (sample 0,0) is at entity XZ + `offset`. Rotation and scale of the entity are ignored.
// Any change (component patch or moving the entity) rebuilds the terrain; brush edits made through
// WorldRuntime::applyBrush are runtime-only and are lost on rebuild.
struct TerrainComponent {
    TerrainSource source = TerrainSource::Procedural;
    Uuid heightmap; // Heightmap source
    world::TerrainNoiseSettings noise{};
    bool hydraulicErosion = false;
    world::HydraulicErosionSettings hydraulic{};
    bool thermalErosion = false;
    world::ThermalErosionSettings thermal{};

    // Heightfield description (resolution comes from the asset for the Heightmap source).
    u32 resolution = 257; // samples per side
    f32 worldSize = 512.f;
    f32 heightScale = 64.f;
    f32 heightOffset = 0.f;
    world::HeightFormat format = world::HeightFormat::Float32;
    bool centered = true;
    glm::vec2 offset{0.f};

    // Splat map: material asset per layer (<= 8) + auto-paint rules. No layers and no rules = no splat map.
    std::vector<Uuid> layers;
    std::vector<world::SplatRule> splatRules;
    u32 splatResolution = 0; // 0 = heightfield resolution

    world::TerrainLodSettings lod{.leafNodeSize = 32, .lodCount = 4, .viewDistance = 2000.f};

    // Static Jolt heightfield bodies per tile (play mode). Raycasts / collision events report this entity.
    bool collision = true;
    u32 physicsTileQuads = 64;
    f32 friction = 0.6f;
    f32 restitution = 0.f;

    // runtime mirrors
    u32 builtResolution = 0;
    f32 minHeight = 0.f;
    f32 maxHeight = 0.f;
};

// Procedural vegetation scattered on a terrain around the viewers (streaming sources + primary camera; the terrain
// centre when there is none). Deterministic and seamless across chunks (world::VegetationScatterer).
struct VegetationComponent {
    std::vector<world::VegetationLayer> layers;
    EntityRef terrain; // empty = the TerrainComponent of this entity
    f32 chunkSize = 64.f;
    f32 cellSize = 32.f;
    f32 patternPeriod = 64.f;
    f32 scatterRadius = 256.f; // chunks closer than this to a viewer are scattered (dropped beyond 1.25x)
    bool colliders = true;     // static bodies for layers with collider = true (play mode)
    std::vector<world::ExclusionZone> exclusions;
    // runtime mirrors
    u32 chunkCount = 0;
    u32 instanceCount = 0;
};

// Analytic sky (Preetham) + sun disc / moon / stars parameters for the renderer. Driven by the world's first
// TimeOfDayComponent when there is one; otherwise uses the static `sunDirection` and `turbidity`.
struct SkyComponent {
    f32 turbidity = 2.5f;
    glm::vec3 sunDirection{0.3f, 0.8f, -0.5f}; // towards the sun (no TimeOfDay)
    f32 skyIntensity = 1.f;
    bool sunDisc = true;
    f32 sunDiscIntensity = 1.f;
    f32 sunAngularDiameterDeg = 0.53f;
    bool moon = true;
    f32 moonIntensity = 1.f;
    f32 moonAngularDiameterDeg = 0.52f;
    bool stars = true;
    f32 starsIntensity = 1.f; // multiplies the atmosphere's starsIntensity
};

// Day/night cycle (world::TimeOfDay). Advances in play mode; in edit mode the state is evaluated from the fields
// (changing localHours in the inspector moves the sun). Drives a directional light, the EnvironmentComponent and
// the primary camera's exposure compensation.
struct TimeOfDayComponent {
    f64 latitudeDeg = 52.37;
    f64 longitudeDeg = 4.90;
    i32 year = 2024;
    i32 month = 6;
    i32 day = 21;
    f64 localHours = 9.0; // SaveGame; written back while time advances
    f64 utcOffsetHours = 2.0;
    f64 timeScale = 60.0; // game seconds per real second
    bool paused = false;
    f32 turbidity = 2.5f;

    // Directional LightComponent to drive; empty = EnvironmentComponent::sun of this entity, else the first
    // directional light of the world.
    EntityRef sun;
    bool driveLight = true;
    f32 illuminanceScale = 1.f; // light intensity = mainLightIlluminance (lux) * scale
    bool driveEnvironment = true; // fog density/colour, ambient intensity (+ weather fog boost)
    bool driveExposure = true;    // primary camera exposureCompensation

    // Curve overrides (empty = AtmosphereCurves::defaults()); times are sun elevation (deg) or local hour.
    world::CurveDriver curveDriver = world::CurveDriver::SunElevation;
    world::CurveInterp curveInterp = world::CurveInterp::Smooth;
    std::vector<world::CurveKey> fogDensityCurve;
    std::vector<world::CurveKey> ambientIntensityCurve;
    std::vector<world::CurveKey> exposureCurve;
    std::vector<world::CurveKey> starsCurve;
    std::vector<world::GradientKey> ambientColorGradient;
    std::vector<world::GradientKey> fogColorGradient;

    // runtime mirrors
    f32 sunElevationDeg = 0.f;
    bool isDay = true;
    f32 moonIllumination = 0.f;
};

// Gerstner water surface: base height = entity world Y, rectangle `size` (XZ, metres) centred on the entity
// (size <= 0 = unbounded). Waves from `waves`, or generated with GerstnerWaves::fromWind when the list is empty.
struct WaterComponent {
    std::vector<world::GerstnerWave> waves;
    glm::vec2 windDirection{1.f, 0.f};
    f32 windSpeed = 6.f;
    u32 waveCount = 6;
    u32 seed = 1;
    f32 steepness = 0.5f;
    glm::vec2 size{200.f, 200.f};
    f32 fluidDensity = 1000.f; // kg/m^3
    glm::vec3 current{0.f};    // m/s, water velocity used for buoyancy drag
};

// Global wind (first active WindComponent) + optional weather blending towards `weather`.
struct WindComponent {
    bool active = true;
    glm::vec2 direction{1.f, 0.f};
    f32 speed = 4.f;
    f32 gustStrength = 0.4f;
    f32 gustWavelength = 60.f;
    f32 turbulence = 0.15f;
    f32 turbulenceFrequency = 0.6f;
    bool weatherEnabled = false;
    world::WeatherPreset weather{};  // target preset (wind speed/gusts override the base when enabled)
    f32 transitionSeconds = 10.f;    // blend time when the target changes at runtime
    f32 temperatureCelsius = 15.f;
    // runtime mirror
    world::WeatherState weatherState{};
};

// Floating body: needs a dynamic RigidBody + Collider on the same entity. Points are relative to the centre of mass.
struct BuoyancyComponent {
    glm::vec3 halfExtents{0.5f}; // box sampled with subdivisions^3 points (when `points` is empty)
    u32 subdivisions = 3;
    std::vector<world::BuoyancyPoint> points;
    f32 fluidDensity = 0.f; // 0 = from the WaterComponent under the body
    f32 linearDrag = 1.f;
    f32 angularDrag = 0.5f;
    // runtime mirror
    f32 submergedFraction = 0.f;
};

// Marks a streaming viewer (player, remote players, cinematic cameras).
struct StreamingSourceComponent {
    bool enabled = true;
    f32 radiusScale = 1.f;
};

// World-level chunk streaming (play mode). Each loaded chunk spawns children of this entity:
//  * chunkPath (if set): world::ChunkData file; `{x}`/`{z}` are replaced by the chunk coordinate. URIs with a scheme
//    ("project://World/chunk_{x}_{z}.oxchunk") are read through the ox::Vfs service, plain paths from disk. The
//    heightfield becomes a TerrainComponent tile (source External, positioned at the chunk origin), the splat map
//    and vegetation instances are attached to it. Missing files = empty chunk.
//  * prefabPattern (if set): prefab name ("Chunks/chunk_{x}_{z}") resolved through IPrefabProvider and instantiated
//    with its root at the chunk origin (x, 0, z). Missing prefab = nothing spawned.
// Unloading destroys the spawned entities.
struct WorldStreamingComponent {
    bool enabled = true;
    world::ChunkStreamerSettings settings{};
    std::string chunkPath;
    std::string prefabPattern;
    bool useCameraAsViewer = true; // primary camera is a viewer in addition to StreamingSource entities
    // Template for spawned terrain tiles.
    world::TerrainLodSettings tileLod{.leafNodeSize = 16, .lodCount = 3, .viewDistance = 1000.f};
    bool tileCollision = true;
    u32 tilePhysicsQuads = 64;
    std::vector<Uuid> tileLayers;
    std::vector<world::VegetationLayer> vegetationLayers; // prototype info for chunk vegetation (by layer index)
    // runtime mirror
    u32 loadedChunks = 0;
};

} // namespace ox::gameplay
