// GPU tests of the reflections-ao area: reflection probes (probe only, + SSR, box projection, bake round trip),
// planar mirror, GTAO, irradiance volume colour bleeding, toggles, 1080p timings.
#include "render_fixture.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_set>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

namespace {

#define EXPECT_GOLDEN(name, img, ...)                                                                                  \
    do {                                                                                                               \
        GoldenResult gr_ = compareGolden(name, img, ##__VA_ARGS__);                                                    \
        EXPECT_TRUE(gr_.matched) << gr_.message << "\n" << asciiArt(img);                                              \
    } while (0)

// Mean linear-ish colour (0..1, display encoded) of a pixel rectangle.
glm::vec3 average(const Image& img, u32 x0, u32 y0, u32 x1, u32 y1) {
    glm::vec3 s(0.0f);
    u32 n = 0;
    for (u32 y = y0; y < y1; ++y) {
        for (u32 x = x0; x < x1; ++x) {
            const glm::u8vec4 c = img.at(x, y);
            s += glm::vec3(c.r, c.g, c.b) / 255.0f;
            ++n;
        }
    }
    return n ? s / f32(n) : s;
}

f64 meanDifference(const Image& a, const Image& b) {
    u64 sum = 0;
    for (usize i = 0; i < a.rgba.size(); ++i) {
        if (i % 4 == 3) continue;
        sum += u64(std::abs(int(a.rgba[i]) - int(b.rgba[i])));
    }
    return f64(sum) / f64(a.rgba.size() / 4 * 3);
}

class ReflectionsTest : public RenderTest {
protected:
    Entity probe(glm::vec3 pos, glm::vec3 extents, bool boxProjection = false,
                 ReflectionProbeUpdate mode = ReflectionProbeUpdate::Baked) {
        Entity e = world->create("ReflectionProbe");
        e.setPosition(pos);
        auto& p = e.add<ReflectionProbeComponent>();
        p.extents = extents;
        p.boxProjection = boxProjection;
        p.update = mode;
        p.blendDistance = 0.5f;
        return e;
    }

    // Chrome + rough metal spheres on a glossy floor, surrounded by coloured boxes (one behind the camera).
    void sphereScene(f32 floorRoughness) {
        mesh(Primitive::Plane, material({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, floorRoughness), {0, 0, 0}, glm::vec3(20.0f));
        mesh(Primitive::Sphere, material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.05f), {-1.1f, 1.0f, 0.0f});
        mesh(Primitive::Sphere, material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.35f), {1.1f, 1.0f, 0.0f});
        mesh(Primitive::Cube, material({0.85f, 0.1f, 0.1f, 1.0f}, 0.0f, 0.6f), {0.0f, 1.0f, -3.0f}, glm::vec3(2.0f));
        mesh(Primitive::Cube, material({0.1f, 0.8f, 0.15f, 1.0f}, 0.0f, 0.6f), {-3.5f, 1.0f, 0.5f}, glm::vec3(1.5f, 2.0f, 1.5f));
        mesh(Primitive::Cube, material({0.1f, 0.25f, 0.9f, 1.0f}, 0.0f, 0.6f), {3.5f, 1.0f, 0.5f}, glm::vec3(1.5f, 2.0f, 1.5f));
        // Emissive yellow wall behind the camera: only visible in reflections.
        mesh(Primitive::Cube, material({0.9f, 0.8f, 0.1f, 1.0f}, 0.0f, 0.6f, glm::vec3(0.9f, 0.75f, 0.05f)), {0.0f, 2.0f, 6.5f},
             glm::vec3(8.0f, 4.0f, 0.5f));
        sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 20000.0f);
        environment(1.0f, 1.0f);
    }
    CameraParams sphereCamera() { return camera({0.0f, 1.5f, 4.4f}, {0.0f, 0.8f, 0.0f}, 13.0f, 50.0f); }
};

} // namespace

TEST_F(ReflectionsTest, ProbeOnlyChromeAndRoughSpheres) {
    CVarScope ssr("r.SSR", "false");
    sphereScene(0.35f);
    const CameraParams cam = sphereCamera();
    const Image noProbe = render(cam, {.frames = 3});
    probe({0.0f, 1.0f, 0.0f}, {9.0f, 5.0f, 9.0f});
    const Image img = render(cam, {.frames = 3});
    EXPECT_GOLDEN("reflections_probe_spheres", img);
    // The chrome sphere's centre reflects the yellow wall behind the camera instead of the sky.
    const glm::vec3 c = average(img, 50, 111, 58, 116);
    const glm::vec3 c0 = average(noProbe, 50, 111, 58, 116);
    EXPECT_GT(c.r, c.b + 0.15f) << "probe reflection: " << c.r << " " << c.g << " " << c.b;
    EXPECT_GT(c.g, c.b + 0.1f);
    EXPECT_GT(meanDifference(img, noProbe), 1.0);
    (void)c0;
}

TEST_F(ReflectionsTest, SsrPlusProbe) {
    CVarScope ssr("r.SSR", "true");
    CVarScope steps("r.SSR.MaxSteps", "64");
    CVarScope half("r.SSR.HalfRes", "false");
    sphereScene(0.15f);
    probe({0.0f, 1.0f, 0.0f}, {9.0f, 5.0f, 9.0f});
    const CameraParams cam = sphereCamera();
    const Image img = render(cam, {.frames = 16});
    EXPECT_GOLDEN("reflections_ssr_probe", img);
    CVarScope off("r.SSR", "false");
    const Image probeOnly = render(cam, {.frames = 4});
    // SSR adds on-screen detail (the spheres and boxes in the floor) on top of the probe.
    EXPECT_GT(meanDifference(img, probeOnly), 0.5);
}

TEST_F(ReflectionsTest, SsrMirrorFloor) {
    // SSR only (no probe): a chrome floor mirrors the on-screen objects; off-screen directions fall back to the sky.
    CVarScope ssr("r.SSR", "true");
    CVarScope half("r.SSR.HalfRes", "false");
    mesh(Primitive::Plane, material({0.9f, 0.9f, 0.9f, 1.0f}, 1.0f, 0.03f), {0, 0, 0}, glm::vec3(20.0f));
    mesh(Primitive::Cube, material({0.85f, 0.1f, 0.1f, 1.0f}, 0.0f, 0.6f), {-1.0f, 0.6f, -1.0f}, glm::vec3(1.2f));
    mesh(Primitive::Sphere, material({0.1f, 0.8f, 0.15f, 1.0f}, 0.0f, 0.5f), {1.0f, 0.6f, 0.0f}, glm::vec3(1.2f));
    mesh(Primitive::Cylinder, material({0.1f, 0.25f, 0.9f, 1.0f}, 0.0f, 0.5f), {0.2f, 1.0f, -2.5f}, glm::vec3(0.6f, 2.0f, 0.6f));
    sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 20000.0f);
    environment(1.0f, 1.0f);
    const CameraParams cam = camera({0.0f, 1.6f, 4.5f}, {0.0f, 0.5f, 0.0f}, 13.0f, 50.0f);
    const Image img = render(cam, {.frames = 12});
    EXPECT_GOLDEN("reflections_ssr_mirror_floor", img);
    CVarScope off("r.SSR", "false");
    const Image skyOnly = render(cam, {.frames = 2});
    // The reflection of the red cube below it (SSR) vs the sky reflection without SSR.
    const glm::vec3 a = average(img, 70, 175, 90, 185), b = average(skyOnly, 70, 175, 90, 185);
    EXPECT_GT(a.r - a.b, b.r - b.b + 0.1f) << a.r << "," << a.b << " vs " << b.r << "," << b.b;
}

TEST_F(ReflectionsTest, SsrHalfResolutionRuns) {
    CVarScope ssr("r.SSR", "true");
    CVarScope half("r.SSR.HalfRes", "true");
    CVarScope q("r.SSR.Quality", "0");
    sphereScene(0.15f);
    const Image img = render(sphereCamera(), {.frames = 6});
    EXPECT_GT(img.luminance(128, 200), 0.02f);
}

TEST_F(ReflectionsTest, BoxProjectedRoom) {
    CVarScope ssr("r.SSR", "false");
    const Uuid white = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.7f);
    mesh(Primitive::Plane, material({0.9f, 0.9f, 0.9f, 1.0f}, 1.0f, 0.12f), {0, 0, 0}, glm::vec3(8.0f));
    mesh(Primitive::Cube, material({0.85f, 0.1f, 0.1f, 1.0f}, 0.0f, 0.7f), {-4.1f, 2.0f, 0.0f}, glm::vec3(0.2f, 4.0f, 8.4f));
    mesh(Primitive::Cube, material({0.1f, 0.75f, 0.15f, 1.0f}, 0.0f, 0.7f), {4.1f, 2.0f, 0.0f}, glm::vec3(0.2f, 4.0f, 8.4f));
    mesh(Primitive::Cube, material({0.1f, 0.2f, 0.85f, 1.0f}, 0.0f, 0.7f), {0.0f, 2.0f, -4.1f}, glm::vec3(8.4f, 4.0f, 0.2f));
    mesh(Primitive::Cube, white, {0.0f, 2.0f, 4.1f}, glm::vec3(8.4f, 4.0f, 0.2f));
    mesh(Primitive::Cube, white, {0.0f, 4.1f, 0.0f}, glm::vec3(8.4f, 0.2f, 8.4f));
    mesh(Primitive::Cube, material({0.9f, 0.6f, 0.1f, 1.0f}, 0.0f, 0.5f), {1.5f, 0.5f, -1.5f}, glm::vec3(1.0f));
    pointLight({0.0f, 3.2f, 0.0f}, 30000.0f, 12.0f, glm::vec3(1.0f, 0.95f, 0.85f), false);
    pointLight({-2.0f, 2.0f, 2.0f}, 8000.0f, 8.0f, glm::vec3(1.0f), false);
    environment(1.0f, 0.2f);
    Entity p = probe({0.0f, 2.0f, 0.0f}, {4.0f, 2.0f, 4.0f}, true);
    p.get<ReflectionProbeComponent>().captureOffset = {0.0f, -0.5f, 0.0f};
    const CameraParams cam = camera({0.0f, 2.2f, 3.6f}, {0.0f, 0.4f, -2.0f}, 8.5f, 70.0f);
    const Image boxed = render(cam, {.frames = 3});
    EXPECT_GOLDEN("reflections_box_projection", boxed);
    p.get<ReflectionProbeComponent>().boxProjection = false;
    const Image infinite = render(cam, {.frames = 3});
    // Without the parallax correction the floor reflections slide: the images differ noticeably.
    EXPECT_GT(meanDifference(boxed, infinite), 1.0);
}

TEST_F(ReflectionsTest, PlanarMirror) {
    CVarScope ssr("r.SSR", "false");
    mesh(Primitive::Plane, material({0.4f, 0.4f, 0.4f, 1.0f}, 0.0f, 0.8f), {0, -0.01f, 0}, glm::vec3(30.0f));
    Entity mirror = mesh(Primitive::Plane, material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.02f), {0, 0, 0}, glm::vec3(5.0f));
    mirror.add<PlanarReflectorComponent>().size = {0.5f, 0.5f};
    mesh(Primitive::Cube, material({0.85f, 0.1f, 0.1f, 1.0f}, 0.0f, 0.5f), {-1.0f, 0.6f, -0.5f}, glm::vec3(1.2f));
    mesh(Primitive::Sphere, material({0.1f, 0.8f, 0.2f, 1.0f}, 0.0f, 0.4f), {1.0f, 0.7f, 0.0f}, glm::vec3(1.4f));
    mesh(Primitive::Cylinder, material({0.1f, 0.25f, 0.9f, 1.0f}, 0.0f, 0.5f), {0.2f, 1.0f, -1.8f}, glm::vec3(0.6f, 2.0f, 0.6f));
    sun(glm::normalize(glm::vec3(-0.3f, -0.8f, -0.5f)), 20000.0f);
    environment(1.0f, 1.0f);
    const CameraParams cam = camera({0.0f, 2.2f, 5.0f}, {0.0f, 0.4f, 0.0f}, 13.0f, 50.0f);
    const Image img = render(cam, {.frames = 2});
    EXPECT_GOLDEN("reflections_planar_mirror", img);
    CVarScope off("r.PlanarReflections", "false");
    const Image noPlanar = render(cam, {.frames = 2});
    EXPECT_GT(meanDifference(img, noPlanar), 1.0);
}

TEST_F(ReflectionsTest, GtaoCorner) {
    const Uuid white = material({0.85f, 0.85f, 0.85f, 1.0f}, 0.0f, 0.8f);
    mesh(Primitive::Plane, white, {0, 0, 0}, glm::vec3(20.0f));
    mesh(Primitive::Cube, white, {-2.0f, 1.5f, 0.0f}, glm::vec3(0.2f, 3.0f, 6.0f));
    mesh(Primitive::Cube, white, {0.0f, 1.5f, -2.0f}, glm::vec3(6.0f, 3.0f, 0.2f));
    mesh(Primitive::Cube, white, {-1.3f, 0.5f, -1.3f}, glm::vec3(1.0f));
    mesh(Primitive::Sphere, white, {0.6f, 0.5f, 0.2f}, glm::vec3(1.0f));
    environment(1.0f, 1.0f);
    const CameraParams cam = camera({2.5f, 2.6f, 3.2f}, {-0.6f, 0.4f, -0.6f}, 11.0f, 55.0f);
    CVarScope method("r.AO.Method", "2");
    CVarScope quality("r.AO.Quality", "2");
    CVarScope half("r.AO.HalfRes", "false");
    const Image img = render(cam, {.frames = 12});
    EXPECT_GOLDEN("ao_gtao_corner", img);
    {
        CVarScope dv("r.DebugView", "AO");
        const Image aoOnly = render(cam, {.frames = 12});
        EXPECT_GOLDEN("ao_gtao_corner_debug", aoOnly);
        // Contact under the sphere and the cube/wall creases are much darker than the open floor.
        EXPECT_LT(aoOnly.luminance(155, 188), aoOnly.luminance(60, 220) - 0.5f);
        EXPECT_LT(aoOnly.luminance(95, 128), aoOnly.luminance(60, 220) - 0.3f);
    }
    CVarScope off("r.AO.Method", "0");
    const Image noAo = render(cam, {.frames = 2});
    EXPECT_GT(meanDifference(img, noAo), 1.0);
    // AO only darkens.
    u64 brighter = 0;
    for (usize i = 0; i < img.rgba.size(); i += 4) brighter += img.rgba[i + 1] > noAo.rgba[i + 1] + 3 ? 1 : 0;
    EXPECT_LT(f64(brighter) / f64(img.rgba.size() / 4), 0.01);
}

TEST_F(ReflectionsTest, AoMethodsAndResolutionsRun) {
    mesh(Primitive::Plane, material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(10.0f));
    mesh(Primitive::Cube, material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.8f), {0, 0.5f, 0}, glm::vec3(1.0f));
    environment();
    const CameraParams cam = camera({2.0f, 2.0f, 3.0f}, {0, 0.3f, 0}, 11.0f);
    for (const char* m : {"1", "2"}) {
        for (const char* h : {"true", "false"}) {
            CVarScope method("r.AO.Method", m);
            CVarScope half("r.AO.HalfRes", h);
            const Image img = render(cam, {.frames = 3});
            EXPECT_GT(img.luminance(128, 200), 0.05f) << m << " " << h;
        }
    }
}

TEST_F(ReflectionsTest, IrradianceVolumeColourBleeding) {
    const Uuid white = material({0.85f, 0.85f, 0.85f, 1.0f}, 0.0f, 0.9f);
    mesh(Primitive::Plane, white, {0, 0, 0}, glm::vec3(30.0f));
    mesh(Primitive::Cube, material({0.9f, 0.05f, 0.05f, 1.0f}, 0.0f, 0.9f), {-2.0f, 1.5f, 0.0f}, glm::vec3(0.3f, 3.0f, 6.0f));
    sun(glm::normalize(glm::vec3(-0.9f, -0.3f, 0.25f)), 20000.0f);
    environment(1.0f, 0.4f);
    const CameraParams cam = camera({3.5f, 2.5f, 4.0f}, {-1.0f, 0.5f, 0.0f}, 13.0f, 55.0f);
    CVarScope perFrame("r.GI.IrradianceVolumes.ProbesPerFrame", "64");
    const Image noGi = render(cam, {.frames = 2});
    Entity v = world->create("IrradianceVolume");
    v.setPosition({0.0f, 1.5f, 0.0f});
    auto& vol = v.add<IrradianceVolumeComponent>();
    vol.extents = {3.0f, 1.5f, 3.0f};
    vol.probeCount = {6, 3, 6};
    vol.captureResolution = 16;
    const Image img = render(cam, {.frames = 6});
    EXPECT_FALSE(reflections::bakeInProgress(*renderer));
    EXPECT_GOLDEN("gi_irradiance_bleeding", img);
    // Floor next to the lit red wall picks up red bounce light.
    const glm::vec3 a = average(img, 90, 140, 130, 160), b = average(noGi, 90, 140, 130, 160);
    EXPECT_GT(a.r - a.g, b.r - b.g + 0.06f) << "with GI " << a.r << "," << a.g << " without " << b.r << "," << b.g;
}

TEST_F(ReflectionsTest, BakeReadbackAndInstall) {
    CVarScope ssr("r.SSR", "false");
    sphereScene(0.35f);
    Entity p = probe({0.0f, 1.0f, 0.0f}, {9.0f, 5.0f, 9.0f});
    const Uuid id = p.get<IdComponent>().id;
    const CameraParams cam = sphereCamera();
    reflections::requestBake(*renderer);
    const Image baked = render(cam, {.frames = 3});
    EXPECT_FALSE(reflections::bakeInProgress(*renderer));
    auto probes = reflections::readBakedProbes(*renderer);
    ASSERT_EQ(probes.size(), 1u);
    EXPECT_EQ(probes[0].first, id);
    ASSERT_TRUE(probes[0].second.valid());
    const auto path = std::filesystem::temp_directory_path() / "oxwald_reflections_test" / "bake.oxcube";
    ASSERT_TRUE(reflections::saveOxCube(path, probes[0].second));
    auto loaded = reflections::loadOxCube(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->data, probes[0].second.data);

    // A fresh renderer uses installed data instead of capturing: install a uniform green cube.
    reflections::BakedCubemap green = *loaded;
    for (usize i = 0; i + 8 <= green.data.size(); i += 8) {
        const u16 px[4] = {0x0000, 0x7000, 0x0000, 0x3C00}; // (0, 8192, 0, 1) half floats
        std::memcpy(&green.data[i], px, 8);
    }
    device->waitIdle();
    renderer.reset();
    renderer = Renderer::create(*device);
    view = 0;
    // Materials live in the resource cache: rebuild the scene.
    world = std::make_unique<World>();
    materialCounter = 0;
    sphereScene(0.35f);
    Entity p2 = probe({0.0f, 1.0f, 0.0f}, {9.0f, 5.0f, 9.0f});
    p2.get<IdComponent>().id = id;
    reflections::setBakedProbe(*renderer, id, green);
    const Image installed = render(cam, {.frames = 2});
    const glm::vec3 c = average(installed, 50, 111, 58, 116);
    EXPECT_GT(c.g, c.r + 0.2f) << c.r << " " << c.g << " " << c.b;
    (void)baked;
}

TEST_F(ReflectionsTest, BakedDataFilesSaveAndInstall) {
    CVarScope ssr("r.SSR", "false");
    sphereScene(0.35f);
    Entity p = probe({0.0f, 1.0f, 0.0f}, {9.0f, 5.0f, 9.0f});
    const Uuid id = p.get<IdComponent>().id;
    const CameraParams cam = sphereCamera();
    reflections::requestBake(*renderer);
    (void)render(cam, {.frames = 3});
    ASSERT_FALSE(reflections::bakeInProgress(*renderer));
    const auto dir = std::filesystem::temp_directory_path() / ("oxwald_baked_" + Uuid::generate().toString());
    auto written = reflections::saveBakedData(*renderer, dir);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(*written, 1u);
    const auto file = dir / reflections::bakedProbeFileName(id);
    auto cube = reflections::loadOxCube(file);
    ASSERT_TRUE(cube) << cube.error().message;
    for (usize i = 0; i + 8 <= cube->data.size(); i += 8) { // recolour: uniform green
        const u16 px[4] = {0x0000, 0x7000, 0x0000, 0x3C00};
        std::memcpy(&cube->data[i], px, 8);
    }
    ASSERT_TRUE(reflections::saveOxCube(file, *cube));

    // Fresh renderer + scene (what the player / a reopened editor sees): data comes from the directory.
    device->waitIdle();
    renderer.reset();
    renderer = Renderer::create(*device);
    view = 0;
    world = std::make_unique<World>();
    materialCounter = 0;
    sphereScene(0.35f);
    probe({0.0f, 1.0f, 0.0f}, {9.0f, 5.0f, 9.0f}).get<IdComponent>().id = id;
    world->updateTransforms();
    RenderSnapshot snap;
    extract(*world, snap, {});
    std::unordered_set<Uuid> attempted;
    usize reads = 0;
    auto reader = [&](const std::string& name) -> std::optional<std::vector<u8>> {
        ++reads;
        std::ifstream in(dir / name, std::ios::binary);
        if (!in) return std::nullopt;
        return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    EXPECT_EQ(reflections::installBakedData(*renderer, snap, reader, attempted), 1u);
    EXPECT_EQ(reflections::installBakedData(*renderer, snap, reader, attempted), 0u) << "each UUID is tried once";
    EXPECT_EQ(reads, 1u);
    const Image installed = render(cam, {.frames = 2});
    const glm::vec3 c = average(installed, 50, 111, 58, 116);
    EXPECT_GT(c.g, c.r + 0.2f) << c.r << " " << c.g << " " << c.b;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(ReflectionsTest, RealtimeProbeFollowsTheScene) {
    CVarScope ssr("r.SSR", "false");
    sphereScene(0.35f);
    probe({0.0f, 1.0f, 0.0f}, {9.0f, 5.0f, 9.0f}, false, ReflectionProbeUpdate::Realtime);
    const CameraParams cam = sphereCamera();
    const Image a = render(cam, {.frames = 2});
    // Recolour the wall behind the camera; a realtime probe picks it up within 6 frames (one face per frame).
    Entity wall = mesh(Primitive::Cube, material({0.1f, 0.9f, 0.9f, 1.0f}, 0.0f, 0.6f, glm::vec3(0.05f, 0.8f, 0.9f)),
                       {0.0f, 1.5f, 5.8f}, glm::vec3(5.0f, 3.5f, 0.3f));
    const Image b = render(cam, {.frames = 8});
    const glm::vec3 ca = average(a, 50, 111, 58, 116), cb = average(b, 50, 111, 58, 116);
    EXPECT_GT(cb.b, ca.b + 0.1f) << ca.b << " -> " << cb.b;
    (void)wall;
}

TEST_F(ReflectionsTest, TogglesRebuildWithoutErrors) {
    sphereScene(0.2f);
    probe({0.0f, 1.0f, 0.0f}, {9.0f, 5.0f, 9.0f});
    Entity mirror = world->create("Mirror");
    mirror.add<PlanarReflectorComponent>().size = {3.0f, 3.0f};
    const CameraParams cam = sphereCamera();
    for (const char* ssr : {"true", "false"}) {
        for (const char* ao : {"0", "2"}) {
            for (const char* feature : {"true", "false"}) {
                CVarScope a("r.SSR", ssr);
                CVarScope b("r.AO.Method", ao);
                CVarScope c("r.Feature.Reflections", feature);
                const Image img = render(cam, {.frames = 2});
                EXPECT_GT(img.luminance(128, 128), 0.01f);
            }
        }
    }
}

TEST_F(ReflectionsTest, QualityPresetsRender) {
    sphereScene(0.2f);
    probe({0.0f, 1.0f, 0.0f}, {9.0f, 5.0f, 9.0f});
    Entity mirror = world->create("Mirror");
    mirror.add<PlanarReflectorComponent>().size = {3.0f, 3.0f};
    Entity v = world->create("IrradianceVolume");
    v.setPosition({0.0f, 1.5f, 0.0f});
    auto& vol = v.add<IrradianceVolumeComponent>();
    vol.probeCount = {3, 2, 3};
    vol.captureResolution = 8;
    const CameraParams cam = sphereCamera();
    for (QualityLevel level : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
        scalability::setGroup(Scalability::Reflections, level);
        scalability::setGroup(Scalability::GlobalIllumination, level);
        const Image img = render(cam, {.frames = 3});
        EXPECT_GT(img.luminance(128, 128), 0.01f) << int(level);
    }
    auto& reg = CVarRegistry::instance();
    for (Scalability g : {Scalability::Reflections, Scalability::GlobalIllumination}) {
        if (ICVar* sg = reg.find("sg." + std::string(scalability::groupName(g)))) sg->reset();
        for (ICVar* c : reg.inGroup(g)) c->reset();
    }
}

TEST_F(ReflectionsTest, PerfReport1080p) {
    // PerfReport1080p scene (400 instances, sun + CSM, 64 point lights) plus 4 probes, a planar reflector, SSR, GTAO
    // and a baked 8×4×8 irradiance volume at the High preset values. Prints per-pass GPU time.
    Random rng(5);
    const Uuid ground = material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.3f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(120.0f));
    for (int i = 0; i < 400; ++i) {
        const Primitive p = Primitive(rng.rangeInt(0, int(Primitive::Count) - 1));
        if (p == Primitive::Plane) continue;
        const Uuid m = material({rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 1.0f}, rng.chance(0.3f) ? 1.0f : 0.0f,
                                rng.range(0.1f, 0.9f));
        mesh(p, m, {rng.range(-40.0f, 40.0f), 0.6f, rng.range(-40.0f, 40.0f)}, glm::vec3(rng.range(0.6f, 2.0f)));
    }
    for (int i = 0; i < 64; ++i) {
        pointLight({rng.range(-30.0f, 30.0f), 1.5f, rng.range(-30.0f, 30.0f)}, 3000.0f, 8.0f,
                   {rng.range(0.3f, 1.0f), rng.range(0.3f, 1.0f), rng.range(0.3f, 1.0f)}, i < 8);
    }
    sun(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 30000.0f);
    environment();
    for (int i = 0; i < 4; ++i) probe({-15.0f + 10.0f * f32(i), 2.0f, 0.0f}, {6.0f, 3.0f, 12.0f});
    Entity mirror = world->create("Mirror");
    mirror.setPosition({0.0f, 0.01f, 10.0f});
    mirror.add<PlanarReflectorComponent>().size = {6.0f, 6.0f};
    Entity v = world->create("IrradianceVolume");
    v.setPosition({0.0f, 2.0f, 0.0f});
    auto& vol = v.add<IrradianceVolumeComponent>();
    vol.extents = {20.0f, 2.0f, 20.0f};
    vol.probeCount = {8, 4, 8};
    vol.captureResolution = 16;
    const CameraParams cam = camera({0, 8, 30}, {0, 0, 0}, 13.0f, 60.0f, 300.0f);
    CVarScope cache("r.Shadows.Caching", "false");
    CVarScope ssr("r.SSR", "true");
    CVarScope ao("r.AO.Method", "2");
    CVarScope perFrame("r.GI.IrradianceVolumes.ProbesPerFrame", "256");
    render(cam, {.width = 1920, .height = 1080, .frames = 4}); // probe captures + volume bake
    render(cam, {.width = 1920, .height = 1080, .frames = 6});
    const RenderStats& s = renderer->stats();
    std::printf("1080p perf with reflections-ao (Apple M4 Pro):\n%s", s.toString().c_str());
    EXPECT_GT(s.gpuFrameMs, 0.0);
}

