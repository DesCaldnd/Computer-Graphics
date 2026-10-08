#include "world_test_utils.hpp"

#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <algorithm>
#include <filesystem>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

std::vector<const ComponentInfo*> worldComponents() {
    std::vector<const ComponentInfo*> out;
    for (const ComponentInfo* info : ComponentRegistry::instance().all()) {
        if (info->category == "World") out.push_back(info);
    }
    return out;
}

void fillEverything(Entity e, Entity other) {
    auto& t = e.add<TerrainComponent>();
    t.source = TerrainSource::Heightmap;
    t.heightmap = Uuid::generate();
    t.noise.fractal.type = world::FractalType::Ridged;
    t.noise.fractal.octaves = 5;
    t.noise.exponent = 1.5f;
    t.hydraulicErosion = true;
    t.hydraulic.droplets = 1234;
    t.thermal.talusAngleDeg = 40.f;
    t.resolution = 513;
    t.format = world::HeightFormat::UNorm16;
    t.centered = false;
    t.offset = {-5.f, 7.f};
    t.layers = {Uuid::generate(), Uuid::generate()};
    t.splatRules = {{.layer = 1, .minSlopeDeg = 30.f}, {.layer = 0, .maxHeight = 50.f}};
    t.lod.viewDistance = 3210.f;
    t.physicsTileQuads = 32;
    t.minHeight = 5.f; // runtime mirror: not persisted

    auto& v = e.add<VegetationComponent>();
    world::VegetationLayer pine{.name = "pine", .kind = world::VegetationKind::Tree, .prototype = 3, .minDistance = 6.f};
    pine.lod.lodDistances[0] = 11.f;
    pine.lod.lodDistances[1] = 22.f;
    pine.collider = true;
    pine.tintB = {0.5f, 0.6f, 0.7f, 1.f};
    v.layers = {pine, world::VegetationLayer{.name = "grass"}};
    v.terrain = other.ref();
    v.exclusions = {{.shape = world::ExclusionZone::Shape::Rect, .center = {1.f, 2.f}, .halfExtents = {3.f, 4.f}, .layerMask = 2u}};
    v.instanceCount = 99;

    auto& s = e.add<SkyComponent>();
    s.turbidity = 4.f;
    s.stars = false;

    auto& tod = e.add<TimeOfDayComponent>();
    tod.latitudeDeg = -33.9;
    tod.localHours = 17.25;
    tod.month = 12;
    tod.sun = other.ref();
    tod.curveDriver = world::CurveDriver::LocalHour;
    tod.fogDensityCurve = {{0.f, 0.02f}, {12.f, 0.001f}};
    tod.fogColorGradient = {{0.f, {0.1f, 0.2f, 0.3f, 1.f}}};
    tod.isDay = false;

    auto& w = e.add<WaterComponent>();
    w.waves = {{.direction = {0.f, 1.f}, .wavelength = 12.f, .amplitude = 0.3f}};
    w.size = {50.f, 60.f};
    w.fluidDensity = 1025.f;

    auto& wind = e.add<WindComponent>();
    wind.speed = 9.f;
    wind.weatherEnabled = true;
    wind.weather = world::WeatherPreset::storm();
    wind.weatherState.wetness = 0.7f;

    auto& b = e.add<BuoyancyComponent>();
    b.halfExtents = {1.f, 0.4f, 2.f};
    b.points = {{.localPosition = {0.f, -0.2f, 0.f}, .volume = 0.5f, .height = 0.2f}};
    b.submergedFraction = 0.3f;

    e.add<StreamingSourceComponent>().radiusScale = 0.5f;

    auto& ws = e.add<WorldStreamingComponent>();
    ws.settings.chunkSize = 64.f;
    ws.settings.loadRadius = 300.f;
    ws.chunkPath = "project://World/chunk_{x}_{z}.oxchunk";
    ws.prefabPattern = "Chunks/chunk_{x}_{z}";
    ws.tileLayers = {Uuid::generate()};
    ws.vegetationLayers = {pine};
    ws.loadedChunks = 4;
}

} // namespace

TEST(GameplayWorld, ComponentsRegisteredAndSerializeRoundTrip) {
    registerGameplayTypes();
    registerWorldGameplayTypes();
    registerWorldGameplayTypes(); // idempotent
    const auto infos = worldComponents();
    std::vector<std::string> names;
    for (auto* i : infos) names.push_back(i->name);
    for (const char* expected : {"Terrain", "Vegetation", "Sky", "TimeOfDay", "Water", "Wind", "Buoyancy", "StreamingSource",
                                 "WorldStreaming"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end()) << expected;
        const ComponentInfo* info = ComponentRegistry::instance().find(expected);
        ASSERT_NE(info, nullptr) << expected;
        EXPECT_FALSE(info->icon.empty()) << expected;
    }
    EXPECT_EQ(infos.size(), 9u);

    World world;
    Entity other = world.create("Other");
    Entity e = world.create("Everything");
    fillEverything(e, other);

    const auto dir = std::filesystem::temp_directory_path() / "ox_gameplay_world_tests";
    std::filesystem::create_directories(dir);
    for (const char* name : {"world.oxscene", "world.oxscene.json"}) {
        const auto path = dir / name;
        ASSERT_TRUE(saveScene(world, path)) << name;
        World loaded;
        ASSERT_TRUE(loadScene(loaded, path)) << name;
        const Entity le = loaded.find(e.uuid());
        ASSERT_TRUE(le.valid());
        for (const ComponentInfo* info : infos) {
            ASSERT_TRUE(info->has(loaded, le.handle())) << info->name << " in " << name;
            EXPECT_EQ(info->serialize(world, e.handle()), info->serialize(loaded, le.handle())) << info->name << " in " << name;
        }
        const auto& t = le.get<TerrainComponent>();
        EXPECT_EQ(t.source, TerrainSource::Heightmap);
        EXPECT_EQ(t.splatRules.size(), 2u);
        EXPECT_FLOAT_EQ(t.lod.viewDistance, 3210.f);
        EXPECT_FLOAT_EQ(t.minHeight, 0.f); // NoSerialize
        const auto& v = le.get<VegetationComponent>();
        ASSERT_EQ(v.layers.size(), 2u);
        EXPECT_FLOAT_EQ(v.layers[0].lod.lodDistances[1], 22.f);
        EXPECT_EQ(v.terrain, other.ref());
        EXPECT_EQ(v.instanceCount, 0u);
        EXPECT_DOUBLE_EQ(le.get<TimeOfDayComponent>().localHours, 17.25);
        EXPECT_EQ(le.get<TimeOfDayComponent>().fogColorGradient.size(), 1u);
        EXPECT_FLOAT_EQ(le.get<WindComponent>().weather.rain, 1.f);
        EXPECT_FLOAT_EQ(le.get<WindComponent>().weatherState.wetness, 0.f);
        EXPECT_FLOAT_EQ(le.get<BuoyancyComponent>().submergedFraction, 0.f);
        EXPECT_EQ(le.get<WorldStreamingComponent>().chunkPath, "project://World/chunk_{x}_{z}.oxchunk");
        EXPECT_EQ(le.get<WorldStreamingComponent>().loadedChunks, 0u);
    }
    auto bytes = serial::readFileBytes(dir / "world.oxscene");
    ASSERT_TRUE(bytes);
    auto json = serial::binaryToJson(*bytes);
    ASSERT_TRUE(json);
    auto back = serial::jsonToBinary(*json);
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, *bytes);
}
