#include "world_test_utils.hpp"

#include <oxwald/scene/prefab.hpp>
#include <oxwald/world/chunk_data.hpp>

#include <filesystem>
#include <fstream>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

constexpr f32 kChunk = 32.f;

void writeChunks(const std::filesystem::path& dir, i32 lo, i32 hi) {
    std::filesystem::create_directories(dir);
    for (i32 z = lo; z <= hi; ++z) {
        for (i32 x = lo; x <= hi; ++x) {
            world::ChunkData chunk;
            chunk.coord = {x, z};
            world::HeightfieldDesc d;
            d.resolution = 17;
            d.worldSize = kChunk;
            d.heightScale = 10.f;
            d.origin = glm::vec2(f32(x), f32(z)) * kChunk;
            world::Heightfield hf(d);
            for (u32 j = 0; j < 17; ++j)
                for (u32 i = 0; i < 17; ++i) hf.setNormalized(i, j, 0.2f); // 2 m
            chunk.heightfield = std::move(hf);
            world::VegetationInstance inst;
            inst.position = {d.origin.x + 4.f, 2.f, d.origin.y + 4.f};
            chunk.vegetation.push_back(inst);
            const std::vector<u8> bytes = world::serializeChunk(chunk);
            std::ofstream out(dir / ("chunk_" + std::to_string(x) + "_" + std::to_string(z) + ".oxchunk"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
    }
}

world::ChunkStreamerSettings streamerSettings() {
    world::ChunkStreamerSettings s;
    s.chunkSize = kChunk;
    s.loadRadius = 40.f;
    s.unloadRadius = 60.f;
    s.maxLoadRequestsPerUpdate = 64;
    s.maxInFlightLoads = 64;
    s.maxActivationsPerUpdate = 64;
    s.maxUnloadsPerUpdate = 64;
    return s;
}

usize countTerrainChildren(Entity parent) {
    usize n = 0;
    for (Entity c : parent.children()) n += c.has<TerrainComponent>() ? 1 : 0;
    return n;
}

} // namespace

TEST(GameplayWorld, StreamingChunkFilesLoadAndUnload) {
    const auto dir = std::filesystem::temp_directory_path() / "ox_gameplay_world_chunks";
    std::filesystem::remove_all(dir);
    writeChunks(dir, -2, 2);

    WorldHarness h;
    Entity streamingEnt = h.world.create("Streaming");
    auto& ws = streamingEnt.add<WorldStreamingComponent>();
    ws.settings = streamerSettings();
    ws.chunkPath = (dir / "chunk_{x}_{z}.oxchunk").string();
    ws.useCameraAsViewer = false;
    ws.tilePhysicsQuads = 16;
    Entity player = h.world.create("Player");
    player.add<StreamingSourceComponent>();
    player.setPosition({0.f, 0.f, 0.f});

    h.start(false);
    h.tick();
    EXPECT_EQ(streamingEnt.childCount(), 0u); // streaming runs in play mode only
    h.scheduler.setPlaying(true);
    h.tick();
    h.tick();
    auto& rt = h.worldRt();
    // Chunks whose rect is within 40 m of the origin: 4x4 minus the 4 corners.
    EXPECT_EQ(countTerrainChildren(streamingEnt), 12u);
    EXPECT_EQ(streamingEnt.get<WorldStreamingComponent>().loadedChunks, 12u);
    EXPECT_TRUE(rt.isAreaReady({0.f, 0.f, 0.f}, 30.f));
    ASSERT_TRUE(rt.terrainHeight({5.f, -20.f}).has_value());
    EXPECT_NEAR(*rt.terrainHeight({5.f, -20.f}), 2.f, 1e-3f);
    const std::vector<Entity> tile = rt.chunkEntities(streamingEnt, {0, 0});
    ASSERT_EQ(tile.size(), 1u);
    EXPECT_NEAR(tile[0].worldPosition().x, 0.f, 1e-4f);
    h.tick(); // play mode colliders of the tiles
    EXPECT_EQ(rt.terrainBodyCount(tile[0]), 1u);
    auto hit = h.runtime<PhysicsRuntime>().raycast({10.f, 20.f, 10.f}, {0.f, -1.f, 0.f}, 50.f);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->entity, tile[0]);
    // Streamed vegetation instances reach the renderer.
    usize vegItems = 0;
    for (const auto& v : h.renderData().vegetation) vegItems += v.batches.size();
    EXPECT_EQ(vegItems, 12u);

    // Move far away: everything unloads (destroyed), empty areas (no files) spawn nothing.
    const entt::entity tileHandle = tile[0].handle();
    player.setPosition({1000.f, 0.f, 0.f});
    h.tick();
    h.tick();
    EXPECT_EQ(streamingEnt.childCount(), 0u);
    EXPECT_FALSE(h.world.valid(tileHandle));
    EXPECT_GT(streamingEnt.get<WorldStreamingComponent>().loadedChunks, 0u); // empty chunks are loaded
    EXPECT_FALSE(rt.terrainHeight({5.f, -20.f}).has_value());

    // Back: reloaded.
    player.setPosition({0.f, 0.f, 0.f});
    h.tick();
    h.tick();
    EXPECT_EQ(countTerrainChildren(streamingEnt), 12u);

    // Stop play: streamed entities are removed.
    h.scheduler.setPlaying(false);
    h.tick();
    EXPECT_EQ(streamingEnt.childCount(), 0u);
    EXPECT_EQ(h.runtime<PhysicsRuntime>().physicsWorld().bodyCount(), 0u);
    std::filesystem::remove_all(dir);
}

TEST(GameplayWorld, StreamingPrefabChunks) {
    WorldHarness h;
    {
        World proto;
        Entity root = proto.create("ChunkContent");
        Entity rock = proto.create("Rock", root);
        rock.setPosition({1.f, 0.f, 2.f});
        rock.add<ColliderComponent>().halfExtents = glm::vec3(1.f);
        const serial::Document doc = createPrefab(proto, root, {.linkSource = false});
        h.assets.addPrefab("Chunks/chunk_0_0", doc);
        h.assets.addPrefab("Chunks/chunk_1_0", doc);
    }
    Entity streamingEnt = h.world.create("Streaming");
    auto& ws = streamingEnt.add<WorldStreamingComponent>();
    ws.settings = streamerSettings();
    ws.settings.loadRadius = 10.f;
    ws.settings.unloadRadius = 20.f;
    ws.prefabPattern = "Chunks/chunk_{x}_{z}";
    Entity cam = h.world.create("Camera");
    cam.add<CameraComponent>().primary = true; // useCameraAsViewer
    cam.setPosition({32.f, 5.f, 16.f});         // on the border of chunks (0,0) and (1,0)
    h.start(true);
    h.tick();
    std::vector<Entity> contents;
    for (Entity c : streamingEnt.children()) {
        if (c.name() == "ChunkContent") contents.push_back(c);
    }
    ASSERT_EQ(contents.size(), 2u);
    auto& rt = h.worldRt();
    const auto c10 = rt.chunkEntities(streamingEnt, {1, 0});
    ASSERT_EQ(c10.size(), 1u);
    EXPECT_NEAR(c10[0].worldPosition().x, 32.f, 1e-4f);
    ASSERT_EQ(c10[0].childCount(), 1u);
    EXPECT_NEAR(c10[0].children()[0].worldPosition().x, 33.f, 1e-4f);

    cam.setPosition({500.f, 5.f, 16.f});
    h.tick();
    EXPECT_EQ(streamingEnt.childCount(), 0u);
    EXPECT_EQ(rt.spawnedChunkCount(streamingEnt), 0u);
}
