#include "world_test_utils.hpp"

#include <glm/gtc/quaternion.hpp>

#include <unordered_map>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

class FakeHeightmaps final : public IHeightmapProvider {
public:
    std::shared_ptr<const HeightmapData> heightmap(const Uuid& id) override {
        auto it = maps.find(id);
        return it == maps.end() ? nullptr : it->second;
    }
    void set(const Uuid& id, u32 res, f32 value) {
        auto d = std::make_shared<HeightmapData>();
        d->resolution = res;
        d->normalized.assign(usize(res) * res, value);
        maps[id] = std::move(d);
    }
    std::unordered_map<Uuid, std::shared_ptr<const HeightmapData>> maps;
};

} // namespace

TEST(GameplayWorld, ProceduralTerrainCollidesInPlayMode) {
    WorldHarness h;
    Entity terrain = h.terrain();
    h.start(false);
    h.tick();
    auto& rt = h.worldRt();
    ASSERT_NE(rt.heightfield(terrain), nullptr);
    EXPECT_EQ(rt.heightfield(terrain)->resolution(), 129u);
    EXPECT_EQ(rt.terrainBodyCount(terrain), 0u); // edit mode: no bodies
    EXPECT_EQ(terrain.get<TerrainComponent>().builtResolution, 129u);
    const auto h0 = rt.terrainHeight({10.f, 12.f});
    ASSERT_TRUE(h0.has_value());
    EXPECT_FALSE(rt.terrainHeight({500.f, 0.f}).has_value());
    EXPECT_GT(terrain.get<TerrainComponent>().maxHeight, terrain.get<TerrainComponent>().minHeight);

    Entity box = h.box("Crate", {10.f, *h0 + 4.f, 12.f}, glm::vec3(0.5f));
    h.scheduler.setPlaying(true);
    h.run(3.0);
    EXPECT_EQ(rt.terrainBodyCount(terrain), 4u); // 128 quads / 64 per tile -> 2x2
    const glm::vec3 p = box.worldPosition();
    const auto ground = rt.terrainHeight({p.x, p.z});
    ASSERT_TRUE(ground.has_value());
    EXPECT_NEAR(p.y, *ground + 0.5f, 0.35f);
    EXPECT_LT(glm::length(glm::vec2(p.x - 10.f, p.z - 12.f)), 3.f);

    auto hit = h.runtime<PhysicsRuntime>().raycast({-20.f, 50.f, 30.f}, {0.f, -1.f, 0.f}, 100.f);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->entity, terrain);
    EXPECT_NEAR(hit->point.y, *rt.terrainHeight({-20.f, 30.f}), 0.1f);

    // Stop: colliders are removed.
    h.scheduler.setPlaying(false);
    h.tick();
    EXPECT_EQ(rt.terrainBodyCount(terrain), 0u);
    EXPECT_EQ(h.runtime<PhysicsRuntime>().physicsWorld().bodyCount(), 0u);
}

TEST(GameplayWorld, BrushEditsUpdateCollidersAndRenderDirtyRect) {
    WorldHarness h;
    Entity terrain = h.terrain();
    terrain.get<TerrainComponent>().source = TerrainSource::Flat;
    h.start(true);
    h.tick();
    auto& rt = h.worldRt();
    ASSERT_EQ(rt.terrainBodyCount(terrain), 4u);
    EXPECT_TRUE(h.renderData().terrains.at(0).fullUpload);
    h.tick();
    EXPECT_FALSE(h.renderData().terrains.at(0).fullUpload);
    EXPECT_TRUE(h.renderData().terrains.at(0).dirtyRect.empty());
    const u64 v0 = rt.terrainVersion(terrain);

    world::BrushSettings b{.op = world::BrushOp::Raise, .radius = 6.f, .strength = 3.f, .falloff = world::BrushFalloff::Constant};
    const world::IRect dirty = rt.applyBrush(terrain, {5.f, 5.f}, b);
    ASSERT_FALSE(dirty.empty());
    EXPECT_GT(rt.terrainVersion(terrain), v0);
    EXPECT_NEAR(*rt.terrainHeight({5.f, 5.f}), 3.f, 1e-3f);
    EXPECT_EQ(rt.terrainBodyCount(terrain), 4u);
    h.tick();
    const TerrainRenderItem& item = h.renderData().terrains.at(0);
    EXPECT_FALSE(item.fullUpload);
    EXPECT_EQ(item.dirtyRect, dirty);
    // The rebuilt physics tile sees the bump.
    auto hit = h.runtime<PhysicsRuntime>().raycast({5.f, 20.f, 5.f}, {0.f, -1.f, 0.f}, 50.f);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->point.y, 3.f, 0.05f);
}

TEST(GameplayWorld, HeightmapTerrainRebuildsOnAssetChange) {
    FakeHeightmaps maps;
    GameplayAssetEvents* events = nullptr;
    WorldHarness h(worldTestConfig(), [&](Services& s) {
        s.addExternal<IHeightmapProvider>(maps);
        events = &s.emplace<GameplayAssetEvents>();
    });
    const Uuid id = Uuid::generate();
    maps.set(id, 33, 0.5f);
    Entity terrain = h.world.create("Island");
    terrain.setPosition({0.f, 2.f, 0.f});
    auto& c = terrain.add<TerrainComponent>();
    c.source = TerrainSource::Heightmap;
    c.heightmap = id;
    c.worldSize = 64.f;
    c.heightScale = 20.f;
    c.lod = {.leafNodeSize = 8, .lodCount = 2, .viewDistance = 200.f};
    h.start(false);
    h.tick();
    auto& rt = h.worldRt();
    ASSERT_NE(rt.heightfield(terrain), nullptr);
    EXPECT_EQ(rt.heightfield(terrain)->resolution(), 33u);
    EXPECT_NEAR(*rt.terrainHeight({3.f, -4.f}), 12.f, 1e-3f); // entity Y + 0.5 * 20
    const u64 v0 = rt.terrainVersion(terrain);

    maps.set(id, 65, 0.25f);
    h.tick();
    EXPECT_EQ(rt.terrainVersion(terrain), v0); // no event yet
    events->changed.emit(GameplayAssetChange{GameplayAssetKind::Heightmap, id, "Terrain/island.png"});
    events->changed.emit(GameplayAssetChange{GameplayAssetKind::Script, id, ""}); // ignored kind
    h.tick();
    EXPECT_GT(rt.terrainVersion(terrain), v0);
    EXPECT_EQ(rt.heightfield(terrain)->resolution(), 65u);
    EXPECT_NEAR(*rt.terrainHeight({3.f, -4.f}), 7.f, 1e-3f);
    EXPECT_TRUE(h.renderData().terrains.at(0).fullUpload);

    // Moving the entity rebuilds at the new place.
    terrain.setPosition({100.f, 0.f, 0.f});
    h.tick();
    EXPECT_FALSE(rt.terrainHeight({0.f, 0.f}).has_value());
    EXPECT_NEAR(*rt.terrainHeight({100.f, 0.f}), 5.f, 1e-3f);
}

TEST(GameplayWorld, RenderDataAfterTick) {
    WorldHarness h;
    Entity terrain = h.terrain();
    {
        auto& tc = terrain.get<TerrainComponent>();
        tc.layers = {Uuid::generate(), Uuid::generate()};
        tc.splatRules = {{.layer = 1, .minSlopeDeg = 10.f}};
    }
    auto& veg = terrain.add<VegetationComponent>();
    world::VegetationLayer grass{.name = "grass", .prototype = 2, .minDistance = 2.f, .maxSlopeDeg = 90.f};
    world::VegetationLayer tree{.name = "tree", .kind = world::VegetationKind::Tree, .prototype = 5, .minDistance = 12.f,
                                .maxSlopeDeg = 90.f, .collider = true};
    veg.layers = {grass, tree};
    veg.scatterRadius = 64.f;

    Entity env = h.world.create("Environment");
    env.add<SkyComponent>();
    env.add<TimeOfDayComponent>().localHours = 13.0;
    env.add<WindComponent>().speed = 7.f;
    Entity sea = h.world.create("Sea");
    sea.setPosition({0.f, 1.f, 0.f});
    sea.add<WaterComponent>().waveCount = 4;

    Entity cam = h.world.create("Camera");
    cam.add<CameraComponent>().primary = true;
    cam.setPosition({0.f, 40.f, 60.f});
    cam.setRotation(glm::quatLookAt(glm::normalize(glm::vec3(0.f, -40.f, -60.f)), glm::vec3(0.f, 1.f, 0.f)));

    h.start(false); // editor: visual data without simulation
    h.tick();
    const WorldRenderData& rd = h.renderData();
    EXPECT_GT(rd.version, 0u);
    EXPECT_TRUE(rd.hasCamera);
    ASSERT_EQ(rd.terrains.size(), 1u);
    const TerrainRenderItem& t = rd.terrains[0];
    EXPECT_EQ(t.entity, toRuntimeId(terrain));
    ASSERT_NE(t.heightfield, nullptr);
    ASSERT_NE(t.splat, nullptr);
    EXPECT_EQ(t.splat->layerCount(), 2u);
    EXPECT_EQ(t.layerMaterials.size(), 2u);
    ASSERT_NE(t.gridMesh, nullptr);
    EXPECT_EQ(t.gridMesh->gridDim, 16u);
    ASSERT_NE(t.quadtree, nullptr);
    EXPECT_EQ(t.skirtDepth.size(), 3u);
    EXPECT_FALSE(t.patches.empty());
    EXPECT_TRUE(t.fullUpload);
    EXPECT_TRUE(t.splatFullUpload);

    ASSERT_TRUE(rd.sky.valid);
    EXPECT_GT(rd.sky.preetham.zenith.w, 0.f);
    EXPECT_GT(rd.sky.sunDirection.y, 0.f);
    EXPECT_GT(rd.sky.mainLightIlluminance, 1000.f);
    EXPECT_TRUE(rd.sky.isDay);

    ASSERT_EQ(rd.water.size(), 1u);
    EXPECT_FLOAT_EQ(rd.water[0].params.info.x, 4.f);
    EXPECT_FLOAT_EQ(rd.water[0].params.info.y, 1.f);

    ASSERT_EQ(rd.vegetation.size(), 1u);
    const VegetationRenderItem& v = rd.vegetation[0];
    ASSERT_EQ(v.layers.size(), 2u);
    EXPECT_EQ(v.layers[1].prototype, 5u);
    EXPECT_TRUE(v.layers[1].castsShadow);
    usize instances = 0;
    for (const auto& b : v.batches) instances += b.instances->size();
    EXPECT_GT(instances, 100u);
    EXPECT_EQ(instances, h.worldRt().vegetationInstanceCount(terrain));
    EXPECT_EQ(terrain.get<VegetationComponent>().instanceCount, instances);
    EXPECT_EQ(h.worldRt().vegetationBodyCount(terrain), 0u); // edit mode

    EXPECT_TRUE(rd.hasWind);
    EXPECT_FLOAT_EQ(rd.wind.dirSpeedTime.z, 7.f);
    EXPECT_FALSE(rd.hasWeather);

    // Next frame: no re-upload, same grid mesh version.
    const u64 gridVersion = t.gridMeshVersion;
    h.tick();
    EXPECT_FALSE(rd.terrains[0].fullUpload);
    EXPECT_EQ(rd.terrains[0].gridMeshVersion, gridVersion);

    // Play: tree colliders appear.
    h.scheduler.setPlaying(true);
    h.tick();
    EXPECT_GT(h.worldRt().vegetationBodyCount(terrain), 0u);
}
