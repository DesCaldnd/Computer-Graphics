#pragma once

#include <oxwald/core/events.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/world/components.hpp>
#include <oxwald/scene/world.hpp>
#include <oxwald/world/heightfield.hpp>
#include <oxwald/world/sky.hpp>
#include <oxwald/world/splat_map.hpp>
#include <oxwald/world/streaming.hpp>
#include <oxwald/world/terrain_brush.hpp>
#include <oxwald/world/terrain_lod.hpp>
#include <oxwald/world/time_of_day.hpp>
#include <oxwald/world/vegetation.hpp>
#include <oxwald/world/weather.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace ox::gameplay {

class WorldRenderData;

struct WorldSystemsConfig {
    bool physics = true;    // terrain / tree colliders + buoyancy (uses the PhysicsRuntime service when present)
    bool vegetation = true;
    bool streaming = true;
    bool bindLua = true;    // `world` Lua API table when a script::ScriptVM service exists
    f32 aspect = 16.f / 9.f; // initial WorldRenderData::aspect (primary camera projection)
    u32 maxVegetationChunksPerFrame = 16;
    enum class Executor : u8 {
        Auto,       // core JobSystem service when present (and it has worker threads), else Inline
        Inline,     // load jobs run synchronously inside ChunkStreamer::update (deterministic)
        JobSystem,  // core JobSystem service (falls back to Inline when missing)
        ThreadPool, // world::ThreadPoolExecutor(2)
    };
    Executor streamingExecutor = Executor::Auto;
};

// Sunrise / Sunset / Noon / Midnight of a TimeOfDayComponent (WorldRuntime::onTimeOfDayEvent + EventBus).
struct WorldTimeEvent {
    Entity entity;
    world::TimeOfDayEvent event = world::TimeOfDayEvent::Sunrise;
    f64 localHours = 0.0;
    bool isDay = true;
};

// Spawned-from-data terrain tile (TerrainSource::External): streamed chunks, tools, procedural generators.
struct ExternalTerrainData {
    std::shared_ptr<world::Heightfield> heightfield;
    std::shared_ptr<world::SplatMap> splat;
    std::vector<world::VegetationInstance> vegetation;
    std::vector<world::VegetationLayer> vegetationLayers; // prototype info for `vegetation` by layer index
};

// Service holding every runtime object of the world integration, keyed by entity. Created by addWorldSystems().
// All functions are game-thread only.
class WorldRuntime {
public:
    explicit WorldRuntime(const WorldSystemsConfig& config = {});
    ~WorldRuntime();
    WorldRuntime(const WorldRuntime&) = delete;
    WorldRuntime& operator=(const WorldRuntime&) = delete;

    [[nodiscard]] World* world() const;
    [[nodiscard]] bool simulating() const;
    [[nodiscard]] f64 gameTime() const; // seconds since attach / play start (water and wind time)
    [[nodiscard]] const WorldSystemsConfig& config() const;

    // ---- terrain ----
    // Height / normal of the first terrain containing worldXZ (nullopt outside every terrain or in a hole).
    [[nodiscard]] std::optional<f32> terrainHeight(glm::vec2 worldXZ) const;
    [[nodiscard]] std::optional<glm::vec3> terrainNormal(glm::vec2 worldXZ) const;
    [[nodiscard]] Entity terrainAt(glm::vec2 worldXZ) const;
    [[nodiscard]] std::shared_ptr<const world::Heightfield> heightfield(Entity terrain) const;
    [[nodiscard]] std::shared_ptr<const world::SplatMap> splatMap(Entity terrain) const;
    [[nodiscard]] std::shared_ptr<const world::TerrainQuadtree> quadtree(Entity terrain) const;
    [[nodiscard]] u64 terrainVersion(Entity terrain) const; // bumps on rebuild and brush edits
    [[nodiscard]] u32 terrainBodyCount(Entity terrain) const;
    // Sculpt (runtime only; a component change rebuilds from the component). Updates the CDLOD bounds, rebuilds the
    // overlapping physics tiles, accumulates the render dirty rect and rescatters vegetation. Returns the dirty rect.
    world::IRect applyBrush(Entity terrain, glm::vec2 centerXZ, const world::BrushSettings& brush, f32 dt = 1.f);
    world::IRect paintSplat(Entity terrain, glm::vec2 centerXZ, u32 layer, const world::BrushSettings& brush, f32 dt = 1.f);
    // Data for TerrainSource::External terrains (may be called before the component is added).
    void setExternalTerrain(Entity terrain, ExternalTerrainData data);
    void rebuildTerrain(Entity terrain);

    // ---- vegetation ----
    [[nodiscard]] u32 vegetationInstanceCount(Entity vegetation) const;
    [[nodiscard]] u32 vegetationBodyCount(Entity vegetation) const;

    // ---- water / buoyancy ----
    // Highest water surface over worldXZ among water entities containing it, at game time (or `time`).
    [[nodiscard]] std::optional<f32> waterHeight(glm::vec2 worldXZ) const;
    [[nodiscard]] std::optional<f32> waterHeight(glm::vec2 worldXZ, f32 time) const;
    [[nodiscard]] f32 submergedFraction(Entity e) const;

    // ---- wind / weather ----
    [[nodiscard]] glm::vec3 windAt(glm::vec3 position) const; // m/s, zero without an active WindComponent
    [[nodiscard]] const world::WeatherState* weather() const;  // global wind entity's weather (null when disabled)
    // Sets the global wind entity's target preset (enables weather) with a transition.
    bool setWeather(const world::WeatherPreset& preset, f32 seconds);

    // ---- time of day ----
    [[nodiscard]] Entity timeOfDayEntity() const; // first TimeOfDayComponent
    [[nodiscard]] std::optional<f64> timeOfDay() const;
    bool setTimeOfDay(f64 localHours); // jumps (no events), updates the component
    [[nodiscard]] const world::SkyState* skyState() const;
    Signal<const WorldTimeEvent&> onTimeOfDayEvent;

    // ---- streaming ----
    // True when every chunk within radius is loaded for every streaming entity (true without streaming).
    [[nodiscard]] bool isAreaReady(glm::vec3 position, f32 radius) const;
    [[nodiscard]] world::ChunkStreamer* streamer(Entity streaming) const;
    [[nodiscard]] std::vector<Entity> chunkEntities(Entity streaming, world::ChunkCoord coord) const;
    [[nodiscard]] u32 spawnedChunkCount(Entity streaming) const;

    // ---- driven by the world systems ----
    void attach(World& world, Services& services);
    void detach();
    void syncPlayState(bool playing);
    void advanceTime(f32 dt);
    void updateStreaming();
    void updateTerrains();
    void updateEnvironment(f32 dt, bool playing);
    void updateVegetation();
    void fixedBuoyancy(f32 dt);
    void extract(WorldRenderData& out, u64 frame);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace ox::gameplay
