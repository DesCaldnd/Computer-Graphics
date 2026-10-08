// World-skinning area GPU tests: terrain (splat layers, holes, CSM), vegetation (GPU culling, impostors, wind),
// world sky (noon / sunset / night with stars and moon), compute skinning (two poses + motion vectors) and a 1080p
// performance report. Goldens: engine/render/tests/data/golden/world-skinning_*.png (OX_UPDATE_GOLDEN=1 rewrites).
#include "render_fixture.hpp"

#include <oxwald/render/features/world/world_skinning.hpp>

#if OX_RENDER_HAS_WORLD
#include <oxwald/world/terrain_gen.hpp>
#include <oxwald/world/vegetation.hpp>
#include <oxwald/world/weather.hpp>
#endif
#if OX_RENDER_TEST_HAS_ANIMATION
#include <oxwald/animation/animation.hpp>
#endif

#include <cstdio>
#include <cstring>
#include <functional>
#include <ostream>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

#define EXPECT_GOLDEN(name, img, ...)                                                                                  \
    do {                                                                                                               \
        GoldenResult gr_ = compareGolden(name, img, ##__VA_ARGS__);                                                    \
        EXPECT_TRUE(gr_.matched) << gr_.message << "\n" << asciiArt(img);                                              \
    } while (0)

namespace {

class WorldSkinningTest : public RenderTest {
protected:
    // Like RenderTest::render() but lets the test attach snapshot extensions after the ECS extract.
    Image renderWith(const CameraParams& cam, const Options& o, const std::function<void(RenderSnapshot&)>& fill) {
        return renderFrames(cam, o, [&](RenderSnapshot& s, u32 frame) {
            if (frame == 0 && fill) fill(s);
        }, false);
    }
    // Per-frame variant: with `extractEveryFrame` the world is extracted again before every frame and `fill` gets the
    // frame index (animation sequences, motion vectors).
    Image renderFrames(const CameraParams& cam, const Options& o, const std::function<void(RenderSnapshot&, u32)>& fill,
                       bool extractEveryFrame = true) {
        ensureTarget(o.width, o.height, o.format);
        ensureView(o.editor);
        for (u32 f = 0; f < o.frames; ++f) {
            if (f == 0 || extractEveryFrame) {
                extractSnapshot();
                if (fill) fill(snapshot, f);
#if OX_RENDER_HAS_WORLD
                finalizeWorldSnapshot(snapshot);
#endif
                if (f == 0) renderer->resources().flush();
            }
            device->beginFrame();
            renderer->beginFrame(snapshot);
            ViewRenderRequest req;
            req.view = view;
            req.camera = cam;
            req.target.texture = target;
            req.target.finalAccess = rhi::Access::TransferRead;
            renderer->renderView(req);
            renderer->endFrame();
            device->endFrame();
        }
        device->waitIdle();
        Image img;
        img.width = o.width;
        img.height = o.height;
        img.rgba = device->readTexture(target);
        for (usize i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
        return img;
    }

    // Procedural RGBA8 sRGB texture with a box-filtered mip chain.
    Uuid texture(const std::string& name, u32 size, const std::function<glm::vec3(glm::vec2)>& fn) {
        assets::TextureData t;
        t.format = assets::TextureFormat::RGBA8Srgb;
        t.width = t.height = size;
        std::vector<u8> level(usize(size) * size * 4);
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                const glm::vec3 c = glm::clamp(fn({(f32(x) + 0.5f) / f32(size), (f32(y) + 0.5f) / f32(size)}), 0.0f, 1.0f);
                for (u32 k = 0; k < 3; ++k) level[(usize(y) * size + x) * 4 + k] = u8(std::pow(c[k], 1.0f / 2.2f) * 255.0f + 0.5f);
                level[(usize(y) * size + x) * 4 + 3] = 255;
            }
        }
        for (u32 s = size;; s /= 2) {
            assets::TextureMip mip{s, s, std::vector<std::byte>(level.size())};
            std::memcpy(mip.data.data(), level.data(), level.size());
            t.mips.push_back(std::move(mip));
            if (s == 1) break;
            std::vector<u8> next(usize(s / 2) * (s / 2) * 4);
            for (u32 y = 0; y < s / 2; ++y)
                for (u32 x = 0; x < s / 2; ++x)
                    for (u32 k = 0; k < 4; ++k)
                        next[(usize(y) * (s / 2) + x) * 4 + k] =
                            u8((level[((2 * y) * s + 2 * x) * 4 + k] + level[((2 * y) * s + 2 * x + 1) * 4 + k] +
                                level[((2 * y + 1) * s + 2 * x) * 4 + k] + level[((2 * y + 1) * s + 2 * x + 1) * 4 + k] + 2) /
                               4);
            level = std::move(next);
        }
        t.mipCount = u32(t.mips.size());
        const Uuid id = Uuid::fromName("test.world.texture." + name);
        renderer->resources().addTexture(id, t);
        return id;
    }

    Uuid texturedMaterial(const std::string& name, const Uuid& albedo, f32 roughness, glm::vec2 tiling = glm::vec2(1.0f)) {
        assets::MaterialAsset m;
        m.albedoTexture = albedo;
        m.roughness = roughness;
        m.uvTiling = tiling;
        const Uuid id = Uuid::fromName("test.world.material." + name);
        renderer->resources().addMaterial(id, m);
        return id;
    }

    Entity sunFrom(glm::vec3 towardsLight, f32 lux, glm::vec3 color, bool shadows = true) {
        return sun(-glm::normalize(towardsLight), lux, color, shadows);
    }
};

f32 hashf(glm::vec2 p) {
    const f32 h = std::sin(glm::dot(p, glm::vec2(127.1f, 311.7f))) * 43758.5453f;
    return h - std::floor(h);
}
f32 valueNoise(glm::vec2 p) {
    const glm::vec2 i = glm::floor(p), f = p - i;
    const glm::vec2 u = f * f * (3.0f - 2.0f * f);
    return glm::mix(glm::mix(hashf(i), hashf(i + glm::vec2(1, 0)), u.x), glm::mix(hashf(i + glm::vec2(0, 1)), hashf(i + glm::vec2(1, 1)), u.x), u.y);
}

} // namespace

#if OX_RENDER_HAS_WORLD

namespace {

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

world::SkyState skyAt(f64 localHours, i32 year = 2024, i32 month = 6, i32 day = 21) {
    world::TimeOfDay tod({.location = {52.37, 4.90}, .year = year, .month = month, .day = day, .localHours = localHours,
                          .utcOffsetHours = 2.0, .timeScale = 0.0, .paused = true});
    return tod.state();
}

struct TerrainScene {
    std::shared_ptr<world::Heightfield> heightfield;
    std::shared_ptr<world::SplatMap> splat;
    std::vector<Uuid> layers;
};

} // namespace

TEST_F(WorldSkinningTest, TerrainSplatLayersAndCsm) {
    // 256 m procedural terrain, 4 auto-painted layers (grass, rock on slopes, sand low, snow high), a square hole,
    // shadows from the sun on the terrain and from the terrain onto meshes.
    world::HeightfieldDesc d;
    d.resolution = 257;
    d.worldSize = 256.0f;
    d.heightScale = 36.0f;
    d.origin = {-128.0f, -128.0f};
    auto hf = std::make_shared<world::Heightfield>(d);
    world::TerrainNoiseSettings ns;
    ns.fractal = {.basis = world::NoiseBasis::Simplex, .type = world::FractalType::Fbm, .seed = 11,
                  .frequency = 1.0f / 150.0f, .octaves = 5};
    ns.exponent = 1.6f;
    world::generateNoise(*hf, ns);
    for (u32 z = 152; z < 162; ++z)
        for (u32 x = 96; x < 110; ++x) hf->setHole(x, z, true);
    auto splat = std::make_shared<world::SplatMap>(257, 4, d.origin, d.worldSize);
    splat->fill(0);
    const world::SplatRule rules[] = {
        {.layer = 2, .maxHeight = 5.0f, .heightBlend = 2.0f, .maxSlopeDeg = 30.0f},
        {.layer = 1, .minSlopeDeg = 24.0f, .slopeBlendDeg = 6.0f, .noiseAmount = 0.3f, .noiseFrequency = 0.08f},
        {.layer = 3, .minHeight = 21.0f, .heightBlend = 3.0f, .maxSlopeDeg = 40.0f},
    };
    world::autoPaint(*splat, *hf, rules);

    const Uuid grassTex = texture("grass", 128, [](glm::vec2 uv) {
        const f32 n = valueNoise(uv * 24.0f) * 0.6f + valueNoise(uv * 64.0f) * 0.4f;
        return glm::mix(glm::vec3(0.07f, 0.16f, 0.03f), glm::vec3(0.22f, 0.34f, 0.08f), n);
    });
    const Uuid rockTex = texture("rock", 128, [](glm::vec2 uv) {
        const f32 n = valueNoise(uv * 10.0f) * 0.5f + valueNoise(uv * 30.0f) * 0.3f + valueNoise(uv * 90.0f) * 0.2f;
        return glm::mix(glm::vec3(0.16f, 0.15f, 0.14f), glm::vec3(0.45f, 0.42f, 0.38f), n);
    });
    const Uuid sandTex = texture("sand", 128, [](glm::vec2 uv) {
        return glm::mix(glm::vec3(0.55f, 0.47f, 0.32f), glm::vec3(0.70f, 0.62f, 0.45f), valueNoise(uv * 48.0f));
    });
    const Uuid snowTex = texture("snow", 64, [](glm::vec2 uv) {
        return glm::mix(glm::vec3(0.82f, 0.85f, 0.9f), glm::vec3(0.95f, 0.96f, 0.98f), valueNoise(uv * 20.0f));
    });
    const std::vector<Uuid> layers = {texturedMaterial("grass", grassTex, 0.9f), texturedMaterial("rock", rockTex, 0.8f, glm::vec2(0.5f)),
                                      texturedMaterial("sand", sandTex, 0.85f), texturedMaterial("snow", snowTex, 0.5f)};

    const Uuid red = material({0.8f, 0.12f, 0.08f, 1.0f}, 0.0f, 0.5f);
    const Uuid white = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.4f);
    const glm::vec3 p0(-10.0f, hf->sampleHeight({-10.0f, 20.0f}) + 2.0f, 20.0f);
    mesh(Primitive::Cube, red, p0, glm::vec3(4.0f));
    mesh(Primitive::Sphere, white, {12.0f, hf->sampleHeight({12.0f, 30.0f}) + 2.5f, 30.0f}, glm::vec3(5.0f));
    sunFrom({0.85f, 0.2f, 0.35f}, 60000.0f, {1.0f, 0.9f, 0.78f});
    environment(1.0f, 1.0f);
    CVarScope dist("r.Shadows.CSM.Distance", "300");

    const TerrainScene scene{hf, splat, layers};
    auto fill = [&](RenderSnapshot& s) {
        WorldSnapshot& w = s.extension<WorldSnapshot>();
        TerrainSnapshot t;
        t.entityId = 77;
        t.heightfield = scene.heightfield;
        t.heightfieldVersion = 1;
        t.splat = scene.splat;
        t.splatVersion = 1;
        t.layerMaterials = scene.layers;
        t.lod = {.leafNodeSize = 16, .lodCount = 5, .viewDistance = 900.0f};
        t.settings.tessellationHeight = 0.4f;
        w.terrains.push_back(t);
    };
    const glm::vec3 eye(0.0f, std::max(hf->sampleHeight({0.0f, 100.0f}), 10.0f) + 26.0f, 100.0f);
    Image img = renderWith(camera(eye, {0.0f, 2.0f, 15.0f}, 13.5f, 60.0f, 2000.0f), {.width = 384, .height = 256, .frames = 3}, fill);
    EXPECT_GOLDEN("world-skinning_terrain_splat_csm", img);
    // Terrain covers the lower half of the image (not the clear colour), the sky the top rows.
    EXPECT_GT(img.luminance(192, 220), 0.02f);
    EXPECT_GT(img.luminance(192, 5), img.luminance(192, 220) * 0.5f);
    // Optional tessellation (Ultra): same scene with displaced detail near the camera.
    if (device->caps().tessellationShader) {
        CVarScope tess("r.Terrain.Tessellation", "true");
        Image t = renderWith(camera(eye, {0.0f, 2.0f, 15.0f}, 13.5f, 60.0f, 2000.0f), {.width = 384, .height = 256, .frames = 3}, fill);
        EXPECT_GOLDEN("world-skinning_terrain_tessellation", t);
        f64 diff = 0;
        for (usize i = 0; i < t.rgba.size(); ++i) diff += std::abs(int(t.rgba[i]) - int(img.rgba[i]));
        EXPECT_LT(diff / f64(t.rgba.size()), 12.0) << "tessellation only adds detail";
    }
    // Sun visibility: terrain self-shadowing and the meshes' shadows on the terrain (rebuilt ShadowMask).
    CVarScope dv("r.DebugView", "ShadowMask");
    Image mask = renderWith(camera(eye, {0.0f, 2.0f, 15.0f}, 13.5f, 60.0f, 2000.0f), {.width = 384, .height = 256, .frames = 2}, fill);
    EXPECT_GOLDEN("world-skinning_terrain_shadowmask", mask);
    auto shadowed = [](const Image& m) {
        u32 n = 0;
        for (u32 y = 40; y < m.height; ++y)
            for (u32 x = 0; x < m.width; ++x) n += m.at(x, y).r < 128 ? 1u : 0u;
        return n;
    };
    {
        CVarScope off("r.Terrain.Shadows", "false");
        Image noTerrainShadows = renderWith(camera(eye, {0.0f, 2.0f, 15.0f}, 13.5f, 60.0f, 2000.0f),
                                            {.width = 384, .height = 256, .frames = 2}, fill);
        EXPECT_GT(shadowed(mask), shadowed(noTerrainShadows) + 200u) << "terrain casts shadows into the cascades";
    }
}

TEST_F(WorldSkinningTest, TerrainDirtyRectUpload) {
    // A brush edit (dirty rect) must update only the changed area and keep the rest identical.
    world::HeightfieldDesc d;
    d.resolution = 129;
    d.worldSize = 64.0f;
    d.heightScale = 8.0f;
    d.origin = {-32.0f, -32.0f};
    d.format = world::HeightFormat::UNorm16;
    auto hf = std::make_shared<world::Heightfield>(d);
    sunFrom({0.3f, 0.8f, 0.2f}, 40000.0f, glm::vec3(1.0f), false);
    environment();
    std::shared_ptr<const world::Heightfield> current = hf;
    u64 version = 1, since = 0;
    world::IRect dirty{};
    auto fill = [&](RenderSnapshot& s) {
        TerrainSnapshot t;
        t.entityId = 5;
        t.heightfield = current;
        t.heightfieldVersion = version;
        t.dirtyRect = dirty;
        t.dirtySinceVersion = since;
        t.lod = {.leafNodeSize = 16, .lodCount = 3, .viewDistance = 400.0f};
        s.extension<WorldSnapshot>().terrains.push_back(t);
    };
    const CameraParams cam = camera({0, 30, 30}, {0, 0, 0}, 13.0f, 55.0f, 500.0f);
    const Image flat = renderWith(cam, {.width = 128, .height = 128, .frames = 2}, fill);
    // Raise a bump in the centre: only that rect is uploaded.
    auto edited = std::make_shared<world::Heightfield>(*hf);
    for (u32 z = 54; z < 74; ++z)
        for (u32 x = 54; x < 74; ++x) {
            const f32 r = glm::length(glm::vec2(f32(x) - 64.0f, f32(z) - 64.0f)) / 10.0f;
            edited->setNormalized(x, z, std::max(0.0f, 1.0f - r * r) * 0.8f);
        }
    current = edited;
    since = version;
    version = 2;
    dirty = {54, 54, 74, 74};
    const Image bump = renderWith(cam, {.width = 128, .height = 128, .frames = 2}, fill);
    // Same result as a full upload of the edited terrain.
    since = 0;
    version = 3;
    dirty = {};
    const Image full = renderWith(cam, {.width = 128, .height = 128, .frames = 2}, fill);
    f64 diffBump = 0, diffFull = 0;
    for (usize i = 0; i < bump.rgba.size(); ++i) {
        diffBump += std::abs(int(bump.rgba[i]) - int(flat.rgba[i]));
        diffFull += std::abs(int(bump.rgba[i]) - int(full.rgba[i]));
    }
    EXPECT_GT(diffBump / f64(bump.rgba.size()), 0.3) << "the edit is visible";
    EXPECT_LT(diffFull / f64(bump.rgba.size()), 0.05) << "partial upload == full upload";
}

TEST_F(WorldSkinningTest, VegetationFieldWithImpostors) {
    world::HeightfieldDesc d;
    d.resolution = 257;
    d.worldSize = 1024.0f;
    d.heightScale = 18.0f;
    d.origin = {-512.0f, -512.0f};
    auto hf = std::make_shared<world::Heightfield>(d);
    world::TerrainNoiseSettings ns;
    ns.fractal = {.seed = 4, .frequency = 1.0f / 300.0f, .octaves = 4};
    world::generateNoise(*hf, ns);

    world::VegetationLayer trees{.name = "tree", .kind = world::VegetationKind::Tree, .prototype = 0, .seed = 3,
                                 .minDistance = 11.0f, .maxSlopeDeg = 40.0f, .minScale = 0.8f, .maxScale = 1.25f,
                                 .tintA = {0.9f, 1.0f, 0.85f, 1.0f}, .tintB = {1.0f, 0.95f, 1.0f, 1.0f}, .boundingRadius = 5.0f};
    trees.lod = {.lodDistances = {18.0f, 40.0f}, .impostorDistance = 70.0f, .cullDistance = 600.0f, .fadeRange = 6.0f};
    world::VegetationLayer grass{.name = "grass", .kind = world::VegetationKind::Grass, .prototype = 1, .seed = 9,
                                 .minDistance = 0.45f, .maxSlopeDeg = 45.0f, .minScale = 0.8f, .maxScale = 1.3f,
                                 .alignToNormal = 0.6f, .tintA = {0.9f, 1.0f, 0.8f, 1.0f}, .tintB = {1.1f, 1.0f, 0.75f, 1.0f},
                                 .boundingRadius = 0.5f};
    grass.lod = {.lodDistances = {8.0f, 16.0f}, .impostorDistance = 0.0f, .cullDistance = 26.0f, .fadeRange = 3.0f};
    world::ScatterContext sc{.heightfield = hf.get()};
    const world::VegetationScatterer treeScatter({trees});
    const world::VegetationScatterer grassScatter({grass});
    world::VegetationChunk treeChunk = treeScatter.scatter({-400.0f, -400.0f}, 800.0f, sc, 32.0f);
    world::VegetationChunk grassChunk = grassScatter.scatter({-20.0f, 60.0f}, 40.0f, sc, 8.0f);
    auto gpuOf = [](const world::VegetationChunk& c, const world::VegetationLayer& l) {
        auto v = std::make_shared<std::vector<world::VegetationInstanceGpu>>();
        for (const world::VegetationInstance& i : c.instances) v->push_back(world::toGpu(i, l));
        return v;
    };
    auto treeGpu = gpuOf(treeChunk, trees);
    auto grassGpu = gpuOf(grassChunk, grass);
    auto treeCells = std::make_shared<std::vector<world::VegetationCell>>(treeChunk.cells);
    auto grassCells = std::make_shared<std::vector<world::VegetationCell>>(grassChunk.cells);
    ASSERT_GT(treeGpu->size(), 1000u);
    ASSERT_GT(grassGpu->size(), 2000u);
    const world::WindField wind({.direction = {1.0f, 0.3f}, .speed = 6.0f});

    sunFrom({0.6f, 0.55f, 0.45f}, 70000.0f, {1.0f, 0.96f, 0.9f});
    environment(1.0f, 1.0f);
    CVarScope dist("r.Shadows.CSM.Distance", "150");
    auto fill = [&](RenderSnapshot& s) {
        WorldSnapshot& w = s.extension<WorldSnapshot>();
        w.time = 12.0;
        TerrainSnapshot t;
        t.entityId = 3;
        t.heightfield = hf;
        t.heightfieldVersion = 1;
        t.lod = {.leafNodeSize = 16, .lodCount = 6, .viewDistance = 2400.0f};
        w.terrains.push_back(t);
        VegetationSnapshot tv;
        tv.entityId = 4;
        tv.layers.push_back({0, world::VegetationKind::Tree, trees.boundingRadius, trees.lod, true});
        tv.batches.push_back({1, 1, treeGpu, treeCells});
        w.vegetation.push_back(tv);
        VegetationSnapshot gv;
        gv.entityId = 5;
        gv.layers.push_back({1, world::VegetationKind::Grass, grass.boundingRadius, grass.lod, false});
        gv.batches.push_back({2, 1, grassGpu, grassCells});
        w.vegetation.push_back(gv);
        w.hasWind = true;
        w.wind = wind.toGpu(12.0f);
    };
    const glm::vec3 eye(0.0f, hf->sampleHeight({0.0f, 100.0f}) + 2.2f, 100.0f);
    const glm::vec3 at(0.0f, hf->sampleHeight({0.0f, 0.0f}) + 6.0f, 0.0f);
    Image img = renderWith(camera(eye, at, 13.5f, 65.0f, 3000.0f), {.width = 384, .height = 256, .frames = 4}, fill);
    EXPECT_GOLDEN("world-skinning_vegetation_impostors", img);
    // Impostors kick in after the first frame (baked in-frame); the culling dispatch produced draws.
    EXPECT_GT(renderer->stats().drawCalls, 10u);
}

namespace {

struct SkyCase {
    const char* name;
    f64 localHours;
    i32 year, month, day;
    f32 ev100;
    bool lookAtMoon;
};

void PrintTo(const SkyCase& c, std::ostream* os) { *os << c.name; }

} // namespace

class WorldSkyTest : public WorldSkinningTest, public ::testing::WithParamInterface<SkyCase> {};

TEST_P(WorldSkyTest, SkyTimeOfDay) {
    const SkyCase sc = GetParam();
    const world::SkyState st = skyAt(sc.localHours, sc.year, sc.month, sc.day);
    std::printf("%s: sun elev %.2f az %.2f lux %.1f | moon elev %.2f az %.2f lux %.4f phase %.2f | stars %.2f\n", sc.name,
                st.sun.elevationDeg, st.sun.azimuthDeg, st.sunLight.illuminance, st.moon.elevationDeg, st.moon.azimuthDeg,
                st.moonLight.illuminance, st.moonPhase.illuminatedFraction, st.atmosphere.starsIntensity);
    const Uuid ground = material({0.35f, 0.33f, 0.3f, 1.0f}, 0.0f, 0.9f);
    const Uuid chrome = material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.15f);
    const Uuid white = material({0.85f, 0.85f, 0.85f, 1.0f}, 0.0f, 0.6f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(400.0f));
    sunFrom(st.mainLightDirection, st.mainLightIlluminance, st.mainLightColor);
    environment(1.0f, 1.0f);
    // Look towards the sun (or the moon) azimuth, horizon in the lower third.
    const glm::vec3 target = sc.lookAtMoon ? st.moonDirection : st.sunDirection;
    glm::vec3 flat = glm::normalize(glm::vec3(target.x, 0.0f, target.z));
    const glm::vec3 eye(0.0f, 1.7f, 0.0f);
    const f32 pitch = sc.lookAtMoon ? std::clamp(std::asin(target.y), 0.1f, 1.0f) * 0.8f : 0.18f;
    const glm::vec3 dir = glm::normalize(flat * std::cos(pitch) + glm::vec3(0.0f, std::sin(pitch), 0.0f));
    mesh(Primitive::Sphere, chrome, eye + flat * 9.0f + glm::vec3(-2.2f, -0.7f, 0.0f), glm::vec3(1.6f));
    mesh(Primitive::Sphere, white, eye + flat * 9.0f + glm::vec3(2.2f, -0.7f, 0.0f), glm::vec3(1.6f));
    auto fill = [&](RenderSnapshot& s) {
        WorldSnapshot& w = s.extension<WorldSnapshot>();
        w.time = 100.0;
        w.sky = skyFromState(st);
        // Games usually enlarge the moon; 3.5° makes the phase readable at this resolution.
        if (sc.lookAtMoon) w.sky.moonAngularDiameterDeg = 3.5f;
    };
    Image img = renderWith(camera(eye, eye + dir, sc.ev100, 75.0f, 5000.0f), {.width = 384, .height = 256, .frames = 2}, fill);
    EXPECT_GOLDEN(std::string("world-skinning_sky_") + sc.name, img);
}

INSTANTIATE_TEST_SUITE_P(Golden, WorldSkyTest,
                         ::testing::Values(SkyCase{"noon", 13.5, 2024, 6, 21, 14.5f, false},
                                           SkyCase{"sunset", 21.85, 2024, 6, 21, 12.0f, false},
                                           SkyCase{"night", 20.5, 2024, 1, 18, -3.5f, true}),
                         [](const ::testing::TestParamInfo<SkyCase>& i) { return std::string(i.param.name); });

TEST_F(WorldSkinningTest, SkyIblKeyIsThrottled) {
    WorldSkySnapshot a = skyFromState(skyAt(12.0));
    WorldSkySnapshot b = skyFromState(skyAt(12.0 + 0.5 / 60.0)); // sun moves ~0.1°
    WorldSkySnapshot c = skyFromState(skyAt(12.5));              // several degrees
    EXPECT_EQ(worldSkyIblKey(a, 1.0f), worldSkyIblKey(b, 1.0f));
    EXPECT_NE(worldSkyIblKey(a, 1.0f), worldSkyIblKey(c, 1.0f));
    RenderSnapshot s;
    s.extension<WorldSnapshot>().sky = a;
    finalizeWorldSnapshot(s);
    ASSERT_TRUE(s.environment.has_value()) << "a sky without an Environment gets a default one (IBL)";
    EXPECT_EQ(s.environment->iblKey.value_or(0), worldSkyIblKey(a, 1.0f));
}

#endif // OX_RENDER_HAS_WORLD

#if OX_RENDER_TEST_HAS_ANIMATION

namespace {

// Vertical tube (3 m, 3 joints along +Y) skinned by height, plus a clip bending the middle and top joints.
struct SkinnedRig {
    anim::Skeleton skeleton;
    anim::AnimationClip clip;
    assets::MeshData mesh;
};

SkinnedRig makeRig() {
    SkinnedRig r;
    anim::Transform t;
    r.skeleton.addJoint("root", -1, t);
    t.translation = {0.0f, 1.0f, 0.0f};
    r.skeleton.addJoint("mid", 0, t);
    r.skeleton.addJoint("top", 1, t);
    r.skeleton.finalize();
    r.clip.tracks.resize(3);
    for (u32 j = 1; j < 3; ++j) {
        anim::Track<glm::quat>& rot = r.clip.tracks[j].rotation;
        rot.times = {0.0f, 1.0f};
        rot.values = {glm::quat(1, 0, 0, 0), glm::angleAxis(glm::radians(j == 1 ? 50.0f : 40.0f), glm::vec3(0, 0, 1))};
    }
    r.clip.computeDuration();
    assets::MeshData& m = r.mesh;
    constexpr u32 kRings = 30, kSeg = 20;
    for (u32 y = 0; y <= kRings; ++y) {
        const f32 h = 3.0f * f32(y) / f32(kRings);
        for (u32 s = 0; s <= kSeg; ++s) {
            const f32 a = 6.2831853f * f32(s) / f32(kSeg);
            const glm::vec3 n(std::cos(a), 0.0f, std::sin(a));
            m.positions.push_back(n * 0.22f + glm::vec3(0.0f, h, 0.0f));
            assets::VertexAttributes at;
            at.normal = n;
            at.uv0 = {f32(s) / kSeg, h / 3.0f};
            m.attributes.push_back(at);
            // Linear blend between the two nearest joints (centred at heights 0.5, 1.5, 2.5).
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
    return r;
}

std::vector<glm::mat4> paletteAt(const SkinnedRig& r, f32 time) {
    anim::Pose pose;
    r.clip.sample(r.skeleton, time, pose);
    std::vector<glm::mat4> model, palette;
    anim::localToModel(r.skeleton, pose, model);
    anim::computeSkinningMatrices(r.skeleton, model, palette);
    return palette;
}

} // namespace

class SkinningTest : public WorldSkinningTest {
protected:
    // Renders the rig animated through `times` (one frame per entry: palette(times[f]), previous palette
    // times[f - 1]); returns the last frame.
    Image renderRig(const SkinnedRig& rig, std::vector<f32> times, bool dualQuat, Options o) {
        const Uuid meshId = Uuid::fromName("test.world.skinnedTube");
        if (renderer->resources().state(meshId) != ResourceState::Ready) renderer->resources().addMesh(meshId, rig.mesh);
        if (!m_rigMaterial.isValid()) m_rigMaterial = material({0.85f, 0.45f, 0.15f, 1.0f}, 0.0f, 0.45f);
        const Uuid mat = m_rigMaterial;
        o.frames = u32(times.size());
        return renderFrames(camera({0.0f, 1.6f, 6.0f}, {0.4f, 1.4f, 0.0f}, 12.0f, 50.0f), o, [&](RenderSnapshot& s, u32 f) {
            const std::vector<glm::mat4> cur = paletteAt(rig, times[f]), prev = paletteAt(rig, times[f > 0 ? f - 1 : 0]);
            SnapshotMesh m;
            m.entityId = 900;
            m.mesh = meshId;
            m.materialOffset = u32(s.materials.size());
            m.materialCount = 1;
            s.materials.push_back(mat);
            m.paletteOffset = u32(s.palettes.size());
            m.paletteCount = u32(cur.size());
            s.palettes.insert(s.palettes.end(), cur.begin(), cur.end());
            m.prevPaletteOffset = u32(s.palettes.size());
            s.palettes.insert(s.palettes.end(), prev.begin(), prev.end());
            s.meshes.push_back(m);
            if (dualQuat) s.extension<SkinningSnapshot>().methods[900] = GpuSkinningMethod::DualQuaternion;
        });
    }
    Uuid m_rigMaterial;
};

TEST_F(SkinningTest, ComputeSkinnedPosesAndMotionVectors) {
    const SkinnedRig rig = makeRig();
    const Uuid floorMat = material({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f);
    mesh(Primitive::Plane, floorMat, {0, 0, 0}, glm::vec3(12.0f));
    sun({-0.3f, -0.8f, -0.5f}, 30000.0f);
    environment();
    // Two poses of the clip (t = 0.2 and t = 0.9), each rendered as a golden.
    const Options o{.width = 192, .height = 192};
    Image a = renderRig(rig, {0.2f, 0.2f, 0.2f}, false, o);
    EXPECT_GOLDEN("world-skinning_skinned_pose_a", a);
    Image b = renderRig(rig, {0.9f, 0.9f, 0.9f}, false, o);
    EXPECT_GOLDEN("world-skinning_skinned_pose_b", b);
    // The bent top swings to the left (−X): where pose b has the limb, pose a shows the background; the base of the
    // tube (bottom joint, not animated) is unchanged.
    EXPECT_GT(std::abs(a.luminance(40, 78) - b.luminance(40, 78)), 0.1f);
    EXPECT_LT(std::abs(a.luminance(84, 132) - b.luminance(84, 132)), 0.05f);

    // Motion vectors: frame 0 = pose a, frame 1 = pose b (previous output of the double buffer). The velocity view
    // shows the moving top in colour while the base and floor keep the neutral (zero motion) colour.
    {
        CVarScope dv("r.DebugView", "Velocity");
        Image v = renderRig(rig, {0.2f, 0.9f}, false, o);
        const glm::u8vec4 zero = v.at(8, 186);
        auto deviation = [&](u32 x0, u32 y0, u32 x1, u32 y1) {
            i32 m = 0;
            for (u32 y = y0; y < y1; ++y)
                for (u32 x = x0; x < x1; ++x) {
                    const glm::u8vec4 c = v.at(x, y);
                    m = std::max(m, std::abs(int(c.r) - int(zero.r)) + std::abs(int(c.g) - int(zero.g)));
                }
            return m;
        };
        EXPECT_GOLDEN("world-skinning_skinned_velocity", v);
        EXPECT_GT(deviation(20, 40, 120, 100), 20) << "moving limb has motion vectors";
        EXPECT_LT(deviation(0, 170, 192, 192), 4) << "static floor has none";
    }
}

TEST_F(SkinningTest, DualQuaternionMatchesLinearForRigidBends) {
    const SkinnedRig rig = makeRig();
    sun({-0.3f, -0.8f, -0.5f}, 30000.0f);
    environment();
    Image lbs = renderRig(rig, {0.9f, 0.9f}, false, {.width = 128, .height = 128});
    Image dqs = renderRig(rig, {0.9f, 0.9f}, true, {.width = 128, .height = 128});
    // Same silhouette (blend regions differ slightly: DQS preserves volume at the joints).
    u64 differ = 0;
    for (u32 y = 0; y < 128; ++y)
        for (u32 x = 0; x < 128; ++x) differ += std::abs(lbs.luminance(x, y) - dqs.luminance(x, y)) > 0.25f ? 1 : 0;
    EXPECT_LT(differ, 128u * 128u / 20u);
    EXPECT_GT(differ, 0u) << "dual quaternion path is active";
}

TEST_F(SkinningTest, OutputsExposedForRayTracing) {
    const SkinnedRig rig = makeRig();
    environment();
    renderRig(rig, {0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f}, false, {.width = 64, .height = 64});
    auto* outputs = dynamic_cast<const ISkinnedOutputs*>(renderer->features().find("Skinning"));
    ASSERT_NE(outputs, nullptr);
    bool timed = false;
    for (const PassTiming& p : renderer->stats().passes) timed = timed || p.name.ends_with("/Skinning");
    std::string names;
    for (const PassTiming& p : renderer->stats().passes) names += p.name + " ";
    EXPECT_TRUE(timed) << "the compute skinning pass reports its GPU time (\"<view>/Skinning\"): " << names;
    const SkinnedOutputs& o = outputs->skinnedOutputs();
    ASSERT_EQ(o.items.size(), 1u);
    EXPECT_TRUE(o.buffer);
    EXPECT_EQ(o.items[0].vertexCount, u32(rig.mesh.positions.size()));
    EXPECT_NE(o.items[0].currentOffset, o.items[0].previousOffset) << "double buffered";
    // The skinned top vertex (last ring) moved away from its bind position.
    device->waitIdle();
    const std::vector<u8> bytes = device->readBuffer(o.buffer, o.items[0].currentOffset, u64(o.items[0].vertexCount) * o.stride);
    GpuSkinnedVertex top{};
    std::memcpy(&top, bytes.data() + (bytes.size() - o.stride), sizeof(top));
    EXPECT_LT(top.position.x, -0.3f);
    EXPECT_LT(top.position.y, 3.0f);
}

#endif // OX_RENDER_TEST_HAS_ANIMATION

#if OX_RENDER_HAS_WORLD

TEST_F(WorldSkinningTest, PerfReport1080p) {
    // 2 km terrain (1025² heightfield, 4 layers), ~25k trees (impostors beyond 80 m), grass around the camera, world sky
    // with aerial perspective, sun + 4 cascades. Prints per-pass GPU timings (Apple M4 Pro in the module notes).
    world::HeightfieldDesc d;
    d.resolution = 1025;
    d.worldSize = 2048.0f;
    d.heightScale = 120.0f;
    d.origin = {-1024.0f, -1024.0f};
    auto hf = std::make_shared<world::Heightfield>(d);
    world::TerrainNoiseSettings ns;
    ns.fractal = {.seed = 21, .frequency = 1.0f / 700.0f, .octaves = 6};
    world::generateNoise(*hf, ns);
    auto splat = std::make_shared<world::SplatMap>(513, 4, d.origin, d.worldSize);
    splat->fill(0);
    const world::SplatRule rules[] = {{.layer = 1, .minSlopeDeg = 22.0f}, {.layer = 2, .maxHeight = 30.0f},
                                      {.layer = 3, .minHeight = 90.0f}};
    world::autoPaint(*splat, *hf, rules);
    const Uuid t0 = texture("perf0", 128, [](glm::vec2 uv) { return glm::vec3(0.1f, 0.2f, 0.05f) * (0.7f + 0.3f * valueNoise(uv * 30.0f)); });
    const Uuid t1 = texture("perf1", 128, [](glm::vec2 uv) { return glm::vec3(0.3f) * (0.6f + 0.4f * valueNoise(uv * 30.0f)); });
    const std::vector<Uuid> layers = {texturedMaterial("p0", t0, 0.9f), texturedMaterial("p1", t1, 0.8f),
                                      texturedMaterial("p2", t0, 0.8f), texturedMaterial("p3", t1, 0.5f)};
    world::VegetationLayer trees{.name = "tree", .kind = world::VegetationKind::Tree, .prototype = 0, .minDistance = 9.0f,
                                 .maxSlopeDeg = 30.0f, .boundingRadius = 5.0f};
    trees.lod = {.lodDistances = {20.0f, 45.0f}, .impostorDistance = 80.0f, .cullDistance = 900.0f, .fadeRange = 6.0f};
    world::VegetationLayer grass{.name = "grass", .kind = world::VegetationKind::Grass, .prototype = 1, .minDistance = 0.4f,
                                 .maxSlopeDeg = 40.0f, .boundingRadius = 0.5f};
    grass.lod = {.lodDistances = {10.0f, 20.0f}, .impostorDistance = 0.0f, .cullDistance = 35.0f, .fadeRange = 3.0f};
    world::ScatterContext sc{.heightfield = hf.get()};
    const world::VegetationScatterer ts({trees}), gs({grass});
    world::VegetationChunk tc = ts.scatter({-800.0f, -800.0f}, 1600.0f, sc, 64.0f);
    world::VegetationChunk gc = gs.scatter({-40.0f, -40.0f}, 80.0f, sc, 8.0f);
    auto gpuOf = [](const world::VegetationChunk& c, const world::VegetationLayer& l) {
        auto v = std::make_shared<std::vector<world::VegetationInstanceGpu>>();
        for (const world::VegetationInstance& i : c.instances) v->push_back(world::toGpu(i, l));
        return v;
    };
    auto tg = gpuOf(tc, trees), gg = gpuOf(gc, grass);
    auto tcells = std::make_shared<std::vector<world::VegetationCell>>(tc.cells);
    auto gcells = std::make_shared<std::vector<world::VegetationCell>>(gc.cells);
    const world::SkyState st = skyAt(16.5);
    sunFrom(st.mainLightDirection, st.mainLightIlluminance, st.mainLightColor);
    environment();
    auto fill = [&](RenderSnapshot& s) {
        WorldSnapshot& w = s.extension<WorldSnapshot>();
        w.time = 30.0;
        TerrainSnapshot t;
        t.entityId = 1;
        t.heightfield = hf;
        t.heightfieldVersion = 1;
        t.splat = splat;
        t.splatVersion = 1;
        t.layerMaterials = layers;
        t.lod = {.leafNodeSize = 32, .lodCount = 6, .viewDistance = 4000.0f};
        w.terrains.push_back(t);
        VegetationSnapshot tv;
        tv.layers.push_back({0, world::VegetationKind::Tree, 5.0f, trees.lod, true});
        tv.batches.push_back({1, 1, tg, tcells});
        w.vegetation.push_back(tv);
        VegetationSnapshot gv;
        gv.layers.push_back({1, world::VegetationKind::Grass, 0.5f, grass.lod, false});
        gv.batches.push_back({2, 1, gg, gcells});
        w.vegetation.push_back(gv);
        w.sky = skyFromState(st);
        w.hasWind = true;
        w.wind = world::WindField({.speed = 5.0f}).toGpu(30.0f);
    };
    const glm::vec3 eye(0.0f, hf->sampleHeight({0.0f, 0.0f}) + 3.0f, 0.0f);
    renderWith(camera(eye, eye + glm::vec3(0.3f, -0.05f, -1.0f), 13.0f, 60.0f, 5000.0f), {.width = 1920, .height = 1080, .frames = 8},
               fill);
    const RenderStats& s = renderer->stats();
    std::printf("world-skinning 1080p perf (%zu trees, %zu grass):\n%s", tg->size(), gg->size(), s.toString().c_str());
    EXPECT_GT(s.gpuFrameMs, 0.0);
    // Breakdown by subsystem (same view).
    const CameraParams cam = camera(eye, eye + glm::vec3(0.3f, -0.05f, -1.0f), 13.0f, 60.0f, 5000.0f);
    for (const char* cv : {"r.Foliage", "r.Terrain"}) {
        CVarScope off(cv, "false");
        renderWith(cam, {.width = 1920, .height = 1080, .frames = 8}, fill);
        std::printf("  without %s: GPU %.2f ms\n", cv, renderer->stats().gpuFrameMs);
        for (const PassTiming& t : renderer->stats().passes) {
            if (t.gpuMs > 0.05) std::printf("    %-28s %.3f ms\n", t.name.c_str(), t.gpuMs);
        }
    }
    {
        CVarScope low("r.Foliage.Density", "0.35");
        CVarScope layers("r.Terrain.MaxLayers", "2");
        renderWith(cam, {.width = 1920, .height = 1080, .frames = 8}, fill);
        std::printf("  foliage density 0.35 + 2 terrain layers: GPU %.2f ms\n", renderer->stats().gpuFrameMs);
    }
}

#endif // OX_RENDER_HAS_WORLD
