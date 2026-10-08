// Глава 28: ландшафт (splat, CDLOD, тени), растительность (GPU-отсечение, импосторы, ветер), небо по времени
// суток и compute-скиннинг на headless-устройстве. Без Vulkan тесты пропускаются.
#include "guide_world_scene.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/world/terrain_gen.hpp>
#include <oxwald/world/time_of_day.hpp>
#include <oxwald/world/vegetation.hpp>
#include <oxwald/world/weather.hpp>

#include <cstdio>
#include <cstring>

using namespace ox;
using namespace ox::render;

namespace {

bool hasPass(const RenderStats& s, std::string_view name) {
    for (const PassTiming& p : s.passes)
        if (p.name.find(name) != std::string::npos) return true;
    return false;
}

f32 luminance(const std::vector<u8>& rgba, u32 width, u32 x, u32 y) {
    const usize i = (usize(y) * width + x) * 4;
    return (0.2126f * rgba[i] + 0.7152f * rgba[i + 1] + 0.0722f * rgba[i + 2]) / 255.0f;
}

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

using WorldRendering = guide::RenderScene;

} // namespace

TEST_F(WorldRendering, TerrainVegetationSky) {
    // Ландшафт 256 м (CPU-часть — глава 16): шум + автораскраска двух слоёв.
    world::HeightfieldDesc d;
    d.resolution = 129;
    d.worldSize = 256.0f;
    d.heightScale = 24.0f;
    d.origin = {-128.0f, -128.0f};
    auto hf = std::make_shared<world::Heightfield>(d);
    world::TerrainNoiseSettings ns;
    ns.fractal = {.seed = 7, .frequency = 1.0f / 120.0f, .octaves = 4};
    world::generateNoise(*hf, ns);
    auto splat = std::make_shared<world::SplatMap>(129, 2, d.origin, d.worldSize);
    splat->fill(0);
    const world::SplatRule rock[] = {{.layer = 1, .minSlopeDeg = 20.0f, .slopeBlendDeg = 6.0f}};
    world::autoPaint(*splat, *hf, rock);
    const std::vector<Uuid> layers = {material({0.12f, 0.25f, 0.06f, 1}, 0.9f), material({0.35f, 0.33f, 0.3f, 1}, 0.8f)};

    // Растительность: деревья (встроенный процедурный прототип 0) с импосторами, трава у камеры.
    world::VegetationLayer trees{.name = "tree", .kind = world::VegetationKind::Tree, .prototype = 0, .seed = 3,
                                 .minDistance = 10.0f, .maxSlopeDeg = 35.0f, .boundingRadius = 5.0f};
    trees.lod = {.lodDistances = {20.0f, 40.0f}, .impostorDistance = 70.0f, .cullDistance = 400.0f, .fadeRange = 6.0f};
    world::VegetationLayer grass{.name = "grass", .kind = world::VegetationKind::Grass, .prototype = 1, .seed = 9,
                                 .minDistance = 0.6f, .maxSlopeDeg = 40.0f, .boundingRadius = 0.5f};
    grass.lod = {.lodDistances = {8.0f, 16.0f}, .impostorDistance = 0.0f, .cullDistance = 25.0f, .fadeRange = 3.0f};
    const world::ScatterContext sc{.heightfield = hf.get()};
    const world::VegetationChunk treeChunk = world::VegetationScatterer({trees}).scatter({-120.0f, -120.0f}, 240.0f, sc, 32.0f);
    const world::VegetationChunk grassChunk = world::VegetationScatterer({grass}).scatter({-15.0f, 40.0f}, 30.0f, sc, 8.0f);
    auto gpuOf = [](const world::VegetationChunk& c, const world::VegetationLayer& l) {
        auto v = std::make_shared<std::vector<world::VegetationInstanceGpu>>();
        for (const world::VegetationInstance& i : c.instances) v->push_back(world::toGpu(i, l));
        return v;
    };
    auto treeGpu = gpuOf(treeChunk, trees);
    auto grassGpu = gpuOf(grassChunk, grass);
    auto treeCells = std::make_shared<std::vector<world::VegetationCell>>(treeChunk.cells);
    auto grassCells = std::make_shared<std::vector<world::VegetationCell>>(grassChunk.cells);

    // Небо и солнце из времени суток (Амстердам, 21 июня, 16:30).
    world::TimeOfDay tod({.location = {52.37, 4.90}, .year = 2024, .month = 6, .day = 21, .localHours = 16.5,
                          .utcOffsetHours = 2.0, .timeScale = 0.0, .paused = true});
    const world::SkyState sky = tod.state();
    Entity sun = world->create("Sun");
    sun.setRotation(lookRotation(-sky.mainLightDirection));
    auto& light = sun.add<LightComponent>();
    light.type = LightType::Directional;
    light.intensity = sky.mainLightIlluminance;
    light.color = sky.mainLightColor;
    world->create("Environment").add<EnvironmentComponent>();
    const world::WindField wind({.direction = {1.0f, 0.3f}, .speed = 6.0f});

    auto fill = [&](RenderSnapshot& s, u32) {
        WorldSnapshot& w = s.extension<WorldSnapshot>();
        w.time = 10.0;
        TerrainSnapshot t;
        t.entityId = 1;
        t.heightfield = hf;
        t.heightfieldVersion = 1; // новая версия → заливка (dirtyRect + dirtySinceVersion — частичная)
        t.splat = splat;
        t.splatVersion = 1;
        t.layerMaterials = layers;
        t.lod = {.leafNodeSize = 16, .lodCount = 4, .viewDistance = 1200.0f};
        w.terrains.push_back(t);
        VegetationSnapshot tv;
        tv.layers.push_back({0, world::VegetationKind::Tree, trees.boundingRadius, trees.lod, /*castsShadow*/ true});
        tv.batches.push_back({/*key*/ 1, /*version*/ 1, treeGpu, treeCells});
        w.vegetation.push_back(tv);
        VegetationSnapshot gv;
        gv.layers.push_back({1, world::VegetationKind::Grass, grass.boundingRadius, grass.lod, false});
        gv.batches.push_back({2, 1, grassGpu, grassCells});
        w.vegetation.push_back(gv);
        w.sky = skyFromState(sky);
        w.hasWind = true;
        w.wind = wind.toGpu(10.0f);
        w.interactors.push_back({0.0f, hf->sampleHeight({0.0f, 60.0f}), 60.0f, 1.0f}); // игрок пригибает траву
    };

    const glm::vec3 eye(0.0f, hf->sampleHeight({0.0f, 70.0f}) + 3.0f, 70.0f);
    CameraParams cam = CameraParams::lookAt(eye, {0.0f, hf->sampleHeight({0.0f, 0.0f}) + 5.0f, 0.0f}, 65.0f, 0.1f, 3000.0f);
    cam.ev100 = 13.5f;
    const std::vector<u8> img = renderWith(cam, 320, 180, 3, fill);

    const RenderStats& s = renderer->stats();
    std::printf("%zu trees, %zu grass clumps: GPU %.2f ms, %u draws\n", treeGpu->size(), grassGpu->size(), s.gpuFrameMs,
                s.drawCalls);
    for (const PassTiming& p : s.passes)
        if (p.name.find("World") != std::string::npos || p.name.find("Vegetation") != std::string::npos ||
            p.name.find("Sky") != std::string::npos)
            std::printf("  %-36s %.3f ms\n", p.name.c_str(), p.gpuMs);
    EXPECT_TRUE(hasPass(s, "World.Prepass"));
    EXPECT_TRUE(hasPass(s, "World.Forward"));
    EXPECT_GT(luminance(img, 320, 160, 5), 0.05f);   // небо сверху
    EXPECT_GT(luminance(img, 320, 160, 170), 0.01f); // ландшафт снизу

    // Качество: Foliage Low выключает траву и прореживает деревья — кадр продолжает рисоваться.
    scalability::setGroup(Scalability::Foliage, QualityLevel::Low);
    renderWith(cam, 320, 180, 2, fill);
    EXPECT_TRUE(hasPass(renderer->stats(), "World.Forward"));
    scalability::setGroup(Scalability::Foliage, QualityLevel::High);
}

namespace {

// Вертикальная «труба» 3 м на трёх суставах (0, 1, 2 м); веса — по высоте между двумя ближайшими суставами.
assets::MeshData tubeMesh() {
    assets::MeshData m;
    constexpr u32 kRings = 24, kSeg = 16;
    for (u32 y = 0; y <= kRings; ++y) {
        const f32 h = 3.0f * f32(y) / f32(kRings);
        for (u32 s = 0; s <= kSeg; ++s) {
            const f32 a = 6.2831853f * f32(s) / f32(kSeg);
            const glm::vec3 n(std::cos(a), 0.0f, std::sin(a));
            m.positions.push_back(n * 0.2f + glm::vec3(0.0f, h, 0.0f));
            assets::VertexAttributes at;
            at.normal = n;
            at.uv0 = {f32(s) / kSeg, h / 3.0f};
            m.attributes.push_back(at);
            assets::SkinVertex sv;
            const f32 jf = std::clamp(h - 0.5f, 0.0f, 2.0f);
            const u16 j0 = u16(std::min(jf, 1.999f));
            const f32 w1 = glm::smoothstep(0.0f, 1.0f, jf - f32(j0));
            sv.joints[0] = j0;
            sv.joints[1] = u16(j0 + 1);
            sv.weights[0] = u16(std::lround((1.0f - w1) * 65535.0f));
            sv.weights[1] = u16(65535 - sv.weights[0]);
            m.skin.push_back(sv);
        }
    }
    for (u32 y = 0; y < kRings; ++y)
        for (u32 s = 0; s < kSeg; ++s) {
            const u32 i0 = y * (kSeg + 1) + s, i1 = i0 + kSeg + 1;
            m.indices.insert(m.indices.end(), {i0, i1, i1 + 1, i0, i1 + 1, i0 + 1});
        }
    assets::Submesh sm;
    sm.vertexCount = u32(m.positions.size());
    sm.lods.push_back({0, u32(m.indices.size())});
    m.submeshes.push_back(sm);
    m.materials.push_back({"Default", Uuid{}});
    computeTangents(m);
    computeBounds(m);
    return m;
}

// Палитра = model[j] · inverseBind[j]; обычно её считает модуль animation (computeSkinningMatrices, глава 10).
std::vector<glm::mat4> bendPalette(f32 degrees) {
    auto bendAt = [](f32 pivotY, f32 deg) {
        return glm::translate(glm::mat4(1.0f), {0.0f, pivotY, 0.0f}) *
               glm::rotate(glm::mat4(1.0f), glm::radians(deg), {0.0f, 0.0f, 1.0f}) *
               glm::translate(glm::mat4(1.0f), {0.0f, -pivotY, 0.0f});
    };
    const glm::mat4 mid = bendAt(1.0f, degrees);
    return {glm::mat4(1.0f), mid, mid * bendAt(2.0f, degrees * 0.8f)};
}

} // namespace

TEST_F(WorldRendering, ComputeSkinningOutputs) {
    const Uuid tube = Uuid::fromName("guide.world.tube");
    renderer->resources().addMesh(tube, tubeMesh());
    const Uuid orange = material({0.85f, 0.45f, 0.15f, 1.0f}, 0.45f);
    world->create("Environment").add<EnvironmentComponent>();

    // Кадр 0 — прямая труба, дальше — согнутая: предыдущая палитра даёт векторы движения.
    const f32 angles[] = {0.0f, 50.0f, 50.0f};
    auto fill = [&](RenderSnapshot& s, u32 frame) {
        const std::vector<glm::mat4> cur = bendPalette(angles[frame]), prev = bendPalette(angles[frame > 0 ? frame - 1 : 0]);
        SnapshotMesh m;
        m.entityId = encodeEntityId(41);
        m.mesh = tube;
        m.materialOffset = u32(s.materials.size());
        m.materialCount = 1;
        s.materials.push_back(orange);
        m.paletteOffset = u32(s.palettes.size());
        m.paletteCount = u32(cur.size());
        s.palettes.insert(s.palettes.end(), cur.begin(), cur.end());
        m.prevPaletteOffset = u32(s.palettes.size());
        s.palettes.insert(s.palettes.end(), prev.begin(), prev.end());
        s.meshes.push_back(m);
        s.extension<SkinningSnapshot>().methods[m.entityId] = GpuSkinningMethod::DualQuaternion; // по умолч. Linear
    };
    CameraParams cam = CameraParams::lookAt({0.0f, 1.6f, 6.0f}, {0.4f, 1.4f, 0.0f}, 50.0f, 0.1f, 100.0f);
    cam.ev100 = 12.0f;
    renderWith(cam, 128, 128, 3, fill);

    // Выходы скиннинга (для BLAS refit трассировки лучей и своих фич): GpuSkinnedVertex, 40 байт, model space.
    const auto* skinning = dynamic_cast<const ISkinnedOutputs*>(renderer->features().find("Skinning"));
    ASSERT_NE(skinning, nullptr);
    const SkinnedOutputs& out = skinning->skinnedOutputs();
    ASSERT_EQ(out.items.size(), 1u);
    EXPECT_EQ(out.stride, 40u);
    EXPECT_NE(out.items[0].currentOffset, out.items[0].previousOffset); // двойной буфер: текущий и прошлый кадр
    device->waitIdle();
    const std::vector<u8> bytes =
        device->readBuffer(out.buffer, out.items[0].currentOffset, u64(out.items[0].vertexCount) * out.stride);
    GpuSkinnedVertex top{};
    std::memcpy(&top, bytes.data() + (bytes.size() - out.stride), sizeof(top));
    EXPECT_LT(top.position.x, -0.3f) << "верх трубы согнулся в −X";
}
