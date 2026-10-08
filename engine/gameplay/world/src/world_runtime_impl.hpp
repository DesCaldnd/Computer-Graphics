#pragma once

// Private state of WorldRuntime, shared by the world_*.cpp implementation files.

#include <oxwald/core/events.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/providers.hpp>
#include <oxwald/gameplay/world.hpp>
#include <oxwald/physics/physics.hpp>
#include <oxwald/world/chunk_data.hpp>
#include <oxwald/world/water.hpp>

#include <entt/signal/sigh.hpp>

#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ox::gameplay {

struct TerrainState {
    std::shared_ptr<world::Heightfield> hf;
    std::shared_ptr<world::SplatMap> splat;
    std::shared_ptr<world::TerrainQuadtree> quadtree;
    std::shared_ptr<const world::TerrainGridMesh> grid;
    u64 gridVersion = 0;
    ExternalTerrainData external;
    bool hasExternal = false;
    // External vegetation converted for the renderer (built on rebuild).
    std::shared_ptr<const std::vector<world::VegetationInstanceGpu>> externalVegGpu;
    std::shared_ptr<const std::vector<world::VegetationCell>> externalVegCells;
    u64 externalVegVersion = 0;

    bool dirty = true;
    glm::vec3 builtPosition{0.f};
    u64 version = 0;
    u64 splatVersion = 0;
    world::IRect dirtyRect{};
    bool fullUpload = false;
    world::IRect splatDirtyRect{};
    bool splatFullUpload = false;

    std::map<std::pair<i32, i32>, physics::BodyHandle> bodies;
    bool bodiesBuilt = false;
};

struct VegetationChunkState {
    world::VegetationChunk chunk;
    std::shared_ptr<const std::vector<world::VegetationInstanceGpu>> gpu;
    std::shared_ptr<const std::vector<world::VegetationCell>> cells;
    u64 version = 0;
    std::vector<physics::BodyHandle> bodies;
    bool bodiesBuilt = false;
};

struct VegetationState {
    std::unique_ptr<world::VegetationScatterer> scatterer;
    bool dirty = true;
    entt::entity terrain{entt::null};
    u64 terrainVersion = 0;
    std::map<std::pair<i32, i32>, VegetationChunkState> chunks;
};

struct TimeOfDayState {
    std::unique_ptr<world::TimeOfDay> tod;
    bool dirty = true;
};

struct WaterState {
    world::GerstnerWaves waves;
    bool dirty = true;
    glm::vec2 center{0.f};
    glm::vec2 size{0.f};
    f32 density = 1000.f;
    glm::vec3 current{0.f};
};

struct WindState {
    world::WeatherController weather;
    world::WeatherPreset target{};
    bool weatherInitialized = false;
    bool dirty = true;
    world::WindField field;
};

struct BuoyancyState {
    world::BuoyancySettings settings;
    bool dirty = true;
    f32 submergedFraction = 0.f;
};

struct StreamingState {
    std::unique_ptr<world::ChunkStreamer> streamer;
    std::unordered_map<world::ChunkCoord, std::vector<entt::entity>, world::ChunkCoordHash> spawned;
    bool dirty = true;
};

// What a streaming load job produces (worker thread) and onLoaded consumes (main thread).
struct StreamedChunk final : world::ChunkPayload {
    std::unique_ptr<world::ChunkData> data;
    std::string prefabName;
};

struct WorldRuntime::Impl {
    explicit Impl(WorldRuntime& owner, const WorldSystemsConfig& c) : self(owner), config(c) {}

    WorldRuntime& self;
    WorldSystemsConfig config;
    World* world = nullptr;
    Services* services = nullptr;
    PhysicsRuntime* physicsRt = nullptr;
    IHeightmapProvider* heightmaps = nullptr;
    IPrefabProvider* prefabs = nullptr;
    EventBus* bus = nullptr;
    Vfs* vfs = nullptr;
    std::shared_ptr<world::IChunkExecutor> executor;
    bool playing = false;
    f64 time = 0.0;

    std::unordered_map<entt::entity, TerrainState> terrains;
    std::unordered_map<entt::entity, VegetationState> vegetation;
    std::unordered_map<entt::entity, TimeOfDayState> timeOfDays;
    std::unordered_map<entt::entity, WaterState> waters;
    std::unordered_map<entt::entity, WindState> winds;
    std::unordered_map<entt::entity, BuoyancyState> buoyancy;
    std::unordered_map<entt::entity, StreamingState> streaming;
    std::unordered_map<u32, std::shared_ptr<const world::TerrainGridMesh>> gridMeshes;
    u64 gridMeshCounter = 0;
    u64 vegetationCounter = 0;
    entt::entity globalWind{entt::null};

    std::vector<entt::scoped_connection> connections;
    ScopedConnection assetConnection;

    // ---- helpers (world_runtime.cpp) ----
    [[nodiscard]] bool physicsEnabled() const { return playing && config.physics && physicsRt != nullptr; }
    physics::BodyHandle createStaticBody(const physics::ShapeDesc& shape, glm::vec3 position, glm::quat rotation, u64 userData,
                                         f32 friction, f32 restitution, bool cacheShape);
    void destroyBody(physics::BodyHandle& body);
    [[nodiscard]] Entity primaryCamera() const;
    // Streaming sources (enabled) + the primary camera.
    [[nodiscard]] std::vector<world::StreamingViewer> viewers(bool includeCamera) const;
    template <class C>
    [[nodiscard]] entt::entity firstWith() const {
        entt::entity best{entt::null};
        for (auto e : world->registry().view<C>()) {
            if (best == entt::null || entt::to_integral(e) < entt::to_integral(best)) best = e;
        }
        return best;
    }

    // ---- terrain (world_terrain.cpp) ----
    void buildTerrain(Entity e, TerrainState& st);
    void buildTerrainBodies(Entity e, TerrainState& st);
    void destroyTerrainBodies(TerrainState& st);
    void rebuildTerrainTiles(Entity e, TerrainState& st, const world::IRect& dirty);
    std::shared_ptr<const world::TerrainGridMesh> gridMesh(u32 dim, u64& version);
    [[nodiscard]] const TerrainState* terrainState(Entity e) const;
    void onAssetChanged(const GameplayAssetChange& change);

    // ---- vegetation (world_vegetation.cpp) ----
    void updateVegetation(Entity e, VegetationState& st, const std::vector<glm::vec3>& viewerPositions);
    void clearVegetation(VegetationState& st);
    void buildVegetationBodies(Entity e, VegetationChunkState& chunk);

    // ---- environment (world_environment.cpp) ----
    void syncTimeOfDay(Entity e, TimeOfDayState& st);
    void applyTimeOfDay(Entity e, const world::SkyState& s, const TimeOfDayComponent& c);
    void syncWind(Entity e, WindState& st, f32 dt, bool isPlaying);
    [[nodiscard]] std::optional<f32> waterHeightAt(glm::vec2 xz, f32 t, const WaterState** which = nullptr) const;

    // ---- streaming (world_streaming.cpp) ----
    void createStreamer(Entity e, StreamingState& st);
    void stopStreaming(StreamingState& st);
    void onChunkLoaded(entt::entity owner, world::ChunkCoord c, world::ChunkPayload& payload);
    void onChunkUnload(entt::entity owner, world::ChunkCoord c);

    // ---- entt signal handlers ----
    void onTerrainChanged(entt::registry&, entt::entity e);
    void onTerrainDestroyed(entt::registry&, entt::entity e);
    void onVegetationChanged(entt::registry&, entt::entity e);
    void onVegetationDestroyed(entt::registry&, entt::entity e);
    void onTimeOfDayChanged(entt::registry&, entt::entity e);
    void onTimeOfDayDestroyed(entt::registry&, entt::entity e) { timeOfDays.erase(e); }
    void onWaterChanged(entt::registry&, entt::entity e) { waters[e].dirty = true; }
    void onWaterDestroyed(entt::registry&, entt::entity e) { waters.erase(e); }
    void onWindChanged(entt::registry&, entt::entity e) { winds[e].dirty = true; }
    void onWindDestroyed(entt::registry&, entt::entity e) { winds.erase(e); }
    void onBuoyancyChanged(entt::registry&, entt::entity e) { buoyancy[e].dirty = true; }
    void onBuoyancyDestroyed(entt::registry&, entt::entity e) { buoyancy.erase(e); }
    void onStreamingChanged(entt::registry&, entt::entity e) { streaming[e].dirty = true; }
    void onStreamingDestroyed(entt::registry&, entt::entity e);

    void clearAll();
};

// "{x}" / "{z}" placeholders -> chunk coordinate.
std::string formatChunkPattern(std::string_view pattern, world::ChunkCoord c);

} // namespace ox::gameplay
