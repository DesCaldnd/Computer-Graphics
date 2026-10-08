// Volumetrics GPU tests: froxel fog goldens (height fog, light shafts, spot cone, local fog box), volumetric
// clouds (noon, sunset), the ray traced visibility hook, toggles and the 1080p per-quality timing report.
#include "render_fixture.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>

#include <cstdio>
#include <map>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

namespace {

class VolumetricsTest : public RenderTest {
protected:
    void SetUp() override {
        RenderTest::SetUp();
        scalability::setGroup(Scalability::Volumetrics, QualityLevel::High);
    }
    void TearDown() override {
        scalability::setGroup(Scalability::Volumetrics, QualityLevel::High);
        RenderTest::TearDown();
    }

    Entity fogEnvironment(f32 density, f32 falloff, glm::vec3 color = glm::vec3(0.8f), f32 sky = 1.0f, f32 ambient = 1.0f) {
        Entity e = environment(sky, ambient);
        auto& env = e.get<EnvironmentComponent>();
        env.fogEnabled = true;
        env.fogDensity = density;
        env.fogHeightFalloff = falloff;
        env.fogColor = color;
        return e;
    }

    // Sum of GPU time of the passes whose name contains `part`.
    f64 passMs(std::string_view part) const {
        f64 ms = 0.0;
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.find(part) != std::string::npos) ms += p.gpuMs;
        }
        return ms;
    }
    bool ranPass(std::string_view part) const {
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.find(part) != std::string::npos) return true;
        }
        return false;
    }

    static f32 meanLuminance(const Image& img, u32 x0, u32 y0, u32 x1, u32 y1) {
        f64 sum = 0.0;
        for (u32 y = y0; y < y1; ++y)
            for (u32 x = x0; x < x1; ++x) sum += img.luminance(x, y);
        return f32(sum / f64((x1 - x0) * (y1 - y0)));
    }

    // A tall wall with a row of small windows between the camera and the low sun. The camera stands in the wall's
    // shadow; each window lets a tube of sunlight through the fog (god rays reaching the ground).
    void windowWallScene() {
        const Uuid stone = material({0.55f, 0.52f, 0.48f, 1.0f}, 0.0f, 0.8f);
        mesh(Primitive::Plane, stone, {0, 0, 0}, glm::vec3(200.0f));
        const f32 z = -12.0f, depth = 0.6f, bottom = 6.0f, top = 7.2f, half = 0.6f;
        mesh(Primitive::Cube, stone, {0.0f, bottom * 0.5f, z}, {40.0f, bottom, depth});
        mesh(Primitive::Cube, stone, {0.0f, (top + 16.0f) * 0.5f, z}, {40.0f, 16.0f - top, depth});
        const f32 windows[] = {-6.0f, -2.0f, 2.0f, 6.0f};
        f32 left = -20.0f;
        for (f32 w : windows) {
            mesh(Primitive::Cube, stone, {(left + w - half) * 0.5f, (bottom + top) * 0.5f, z}, {w - half - left, top - bottom, depth});
            left = w + half;
        }
        mesh(Primitive::Cube, stone, {(left + 20.0f) * 0.5f, (bottom + top) * 0.5f, z}, {20.0f - left, top - bottom, depth});
        sun(glm::normalize(glm::vec3(-0.45f, -0.45f, 1.0f)), 60000.0f, {1.0f, 0.9f, 0.75f});
    }

    void cloudScene(glm::vec3 sunDir, f32 lux, glm::vec3 sunColor, f32 coverage) {
        const Uuid ground = material({0.25f, 0.3f, 0.2f, 1.0f}, 0.0f, 0.9f);
        mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(400.0f));
        sun(sunDir, lux, sunColor);
        environment();
        Entity c = world->create("Clouds");
        auto& cl = c.add<CloudLayerComponent>();
        cl.coverage = coverage;
        cl.windSpeed = 0.0f;
    }
};

} // namespace

TEST_F(VolumetricsTest, HeightFogFade) {
    const Uuid stone = material({0.6f, 0.6f, 0.62f, 1.0f}, 0.0f, 0.7f);
    const Uuid red = material({0.7f, 0.15f, 0.1f, 1.0f}, 0.0f, 0.5f);
    mesh(Primitive::Plane, stone, {0, 0, 0}, glm::vec3(400.0f));
    // Towers that keep about the same angular size with distance, fanned out left / right.
    const f32 distances[] = {10.0f, 18.0f, 30.0f, 48.0f, 75.0f, 115.0f, 170.0f};
    const f32 angles[] = {-4.0f, 8.0f, -12.0f, 16.0f, -20.0f, 24.0f, -27.0f};
    for (int i = 0; i < 7; ++i) {
        const f32 d = distances[i], s = d / 10.0f;
        const f32 x = d * std::tan(glm::radians(angles[i]));
        mesh(Primitive::Cube, i % 2 ? red : stone, {x, 2.0f * s, -d}, {0.8f * s, 4.0f * s, 0.8f * s});
    }
    sun(glm::normalize(glm::vec3(-0.4f, -0.6f, -0.5f)), 30000.0f);
    fogEnvironment(0.015f, 0.12f, {0.75f, 0.8f, 0.9f});
    const Image img = render(camera({0, 2.0f, 6.0f}, {0, 3.0f, -100.0f}, 13.0f, 60.0f, 400.0f), {.frames = 12});
    EXPECT_TRUE(ranPass("Volumetrics.FogIntegrate"));
    GoldenResult g = compareGolden("volumetrics_height_fog", img);
    EXPECT_TRUE(g.matched) << g.message << "\n" << asciiArt(img);
}

TEST_F(VolumetricsTest, LightShaftsThroughWindows) {
    windowWallScene();
    fogEnvironment(0.08f, 0.02f, glm::vec3(0.9f), 0.25f, 0.25f);
    const Image img = render(camera({7.0f, 1.7f, 9.0f}, {-2.0f, 3.5f, -8.0f}, 10.5f, 70.0f, 300.0f), {.width = 384, .height = 256, .frames = 16});
    GoldenResult g = compareGolden("volumetrics_light_shafts", img);
    EXPECT_TRUE(g.matched) << g.message << "\n" << asciiArt(img);
}

TEST_F(VolumetricsTest, SpotLightConeInFog) {
    const Uuid floorMat = material({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f);
    const Uuid ball = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.4f);
    mesh(Primitive::Plane, floorMat, {0, 0, 0}, glm::vec3(60.0f));
    mesh(Primitive::Sphere, ball, {0.4f, 2.5f, -6.0f}, glm::vec3(1.2f));
    spotLight({0, 7.0f, -6.0f}, {0, -1, 0}, 60000.0f, 14.0f, 14.0f, 26.0f, true, {1.0f, 0.85f, 0.6f});
    fogEnvironment(0.06f, 0.0f, glm::vec3(1.0f), 0.002f, 0.002f);
    world->create("FogSettings").add<VolumetricFogComponent>().anisotropy = 0.2f;
    const Image img = render(camera({0, 3.0f, 6.0f}, {0, 3.5f, -6.0f}, 4.0f, 60.0f, 100.0f), {.frames = 16});
    GoldenResult g = compareGolden("volumetrics_spot_cone", img);
    EXPECT_TRUE(g.matched) << g.message << "\n" << asciiArt(img);
    // The cone is lit fog: brighter than the fog next to it.
    EXPECT_GT(meanLuminance(img, 120, 60, 136, 100), meanLuminance(img, 10, 60, 26, 100) + 0.05f);
}

TEST_F(VolumetricsTest, PointLightsInLocalFogBox) {
    const Uuid floorMat = material({0.06f, 0.06f, 0.06f, 1.0f}, 0.0f, 0.8f);
    mesh(Primitive::Plane, floorMat, {0, 0, 0}, glm::vec3(60.0f));
    pointLight({-2.2f, 1.0f, -6.0f}, 6000.0f, 6.0f, {1.0f, 0.2f, 0.1f});
    pointLight({0.0f, 1.0f, -6.3f}, 6000.0f, 6.0f, {0.2f, 1.0f, 0.2f});
    pointLight({2.2f, 1.0f, -6.0f}, 6000.0f, 6.0f, {0.2f, 0.3f, 1.0f});
    sun(glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f)), 40.0f, {0.6f, 0.7f, 1.0f}, false); // dim moonlight
    Entity box = world->create("FogBox");
    box.setPosition({0.0f, 1.2f, -6.0f});
    auto& fv = box.add<FogVolumeComponent>();
    fv.shape = FogVolumeShape::Box;
    fv.extents = {3.5f, 1.2f, 1.5f};
    fv.density = 0.35f;
    fv.falloff = 0.2f;
    fv.noiseIntensity = 0.6f;
    fv.noiseScale = 4.0f;
    const Image img = render(camera({0, 1.6f, 5.0f}, {0, 1.3f, -6.0f}, 4.0f, 60.0f, 100.0f), {.frames = 16});
    GoldenResult g = compareGolden("volumetrics_point_fog_box", img);
    EXPECT_TRUE(g.matched) << g.message << "\n" << asciiArt(img);
}

TEST_F(VolumetricsTest, CloudsAtNoon) {
    cloudScene(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 80000.0f, glm::vec3(1.0f), 0.5f);
    const Image img = render(camera({0, 2.0f, 0}, {0, 22.0f, -60.0f}, 14.0f, 75.0f, 1000.0f), {.frames = 48});
    EXPECT_TRUE(ranPass("Volumetrics.CloudTrace"));
    GoldenResult g = compareGolden("volumetrics_clouds_noon", img);
    EXPECT_TRUE(g.matched) << g.message << "\n" << asciiArt(img);
}

TEST_F(VolumetricsTest, CloudsAtSunset) {
    cloudScene(glm::normalize(glm::vec3(0.1f, -0.07f, -1.0f)), 15000.0f, {1.0f, 0.55f, 0.3f}, 0.55f);
    const Image img = render(camera({0, 2.0f, 0}, {0, 10.0f, -60.0f}, 10.5f, 75.0f, 1000.0f), {.frames = 48});
    GoldenResult g = compareGolden("volumetrics_clouds_sunset", img);
    EXPECT_TRUE(g.matched) << g.message << "\n" << asciiArt(img);
}

// --- ray traced visibility hook: a producer writing zero visibility removes all lighting from the fog ---

namespace {

struct ZeroVisibilityFeature final : IRenderFeature {
    rhi::PipelineHandle pipeline;
    std::string_view name() const override { return "TestFogVisibility"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    i32 order() const override { return volumetrics::kFogOrder - 10; }
    std::vector<std::string_view> provides() const override { return {volumetrics::kVolumetricFogVisibility}; }
    bool initialize(FeatureInitContext& ctx) override {
        rhi::ComputePipelineDesc d;
        d.name = "test.fogVisibility";
        d.shader = rhi::ShaderStageDesc::glsl(R"(#version 460
#include <render/volumetrics/fog_common.glsl>
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
OX_RENDER_PUSH(uint dst;);
void main() {
    uvec3 c = gl_GlobalInvocationID;
    if (any(greaterThanEqual(vec3(c), VIEW.volumetricFogGrid.xyz))) return;
    // A real producer traces from oxFroxelWorldPosition(pc.view, vec3(c) + 0.5 + jitter, depth) to each light.
    imageStore(oxImages3D_rgba16f[pc.dst], ivec3(c), vec4(0.0, 0.0, 0.0, 1.0));
}
)", rhi::ShaderStage::Compute, "test_fog_visibility.comp");
        pipeline = ctx.device.createComputePipeline(d);
        return true;
    }
    void shutdown(rhi::Device& dev) override { dev.destroy(pipeline); }
    void setup(FeatureContext& ctx) override {
        const volumetrics::FroxelGrid g = volumetrics::froxelGridFromCVars(&ctx.snapshot());
        rhi::TextureDesc td;
        td.type = rhi::TextureType::Tex3D;
        td.format = volumetrics::kVisibilityFormat;
        td.width = g.x;
        td.height = g.y;
        td.depth = g.z;
        td.usage = rhi::TextureUsage::None;
        td.name = "VolumetricFogVisibility";
        const rhi::RGTexture vis = ctx.graph().createTexture(td);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        ctx.graph().addPass("TestFogVisibility", rhi::PassType::Compute)
            .overwrite(vis, rhi::Access::StorageWriteCompute)
            .execute([this, vis, g, view, scene](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 dst;
                } pc{view, scene, p.storageIndex(vis)};
                p.cmd.bindPipeline(pipeline);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((g.x + 3) / 4, (g.y + 3) / 4, (g.z + 3) / 4);
            });
        ctx.resources().setTexture(volumetrics::kVolumetricFogVisibility, vis);
    }
};

} // namespace

TEST_F(VolumetricsTest, VisibilityHookReplacesShadowLookups) {
    windowWallScene();
    fogEnvironment(0.05f, 0.02f, glm::vec3(0.9f), 0.25f, 0.25f);
    const CameraParams cam = camera({0, 2.5f, 8.0f}, {0, 4.5f, -12.0f}, 13.0f, 65.0f, 300.0f);
    const Image lit = render(cam, {.width = 128, .height = 128, .frames = 4});
    renderer->features().emplace<ZeroVisibilityFeature>();
    const Image dark = render(cam, {.width = 128, .height = 128, .frames = 4});
    EXPECT_TRUE(ranPass("TestFogVisibility"));
    // In-scattering is gone (only extinction remains): the hazy foreground gets much darker.
    const f32 a = meanLuminance(lit, 0, 64, 128, 100), b = meanLuminance(dark, 0, 64, 128, 100);
    EXPECT_LT(b, a * 0.7f) << "lit " << a << " dark " << b;
}

TEST_F(VolumetricsTest, TogglesAndQualityLevels) {
    windowWallScene();
    fogEnvironment(0.05f, 0.02f);
    Entity c = world->create("Clouds");
    c.add<CloudLayerComponent>();
    const CameraParams cam = camera({0, 2.5f, 8.0f}, {0, 6.0f, -12.0f}, 13.0f, 65.0f, 300.0f);
    render(cam, {.width = 96, .height = 64, .frames = 3});
    EXPECT_TRUE(ranPass("Volumetrics.FogScatter"));
    EXPECT_TRUE(ranPass("Volumetrics.CloudReconstruct"));
    EXPECT_TRUE(ranPass("Volumetrics.Composite"));
    {
        CVarScope off("r.VolumetricFog", "false");
        render(cam, {.width = 96, .height = 64, .frames = 4});
        EXPECT_FALSE(ranPass("Volumetrics.FogScatter"));
        EXPECT_TRUE(ranPass("Volumetrics.CloudTrace"));
    }
    // Low: no clouds, half the depth slices; every level renders without validation errors.
    for (QualityLevel l : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
        scalability::setGroup(Scalability::Volumetrics, l);
        render(cam, {.width = 96, .height = 64, .frames = 4});
        EXPECT_EQ(ranPass("Volumetrics.CloudTrace"), l != QualityLevel::Low) << int(l);
        EXPECT_TRUE(ranPass("Volumetrics.FogIntegrate")) << int(l);
    }
    scalability::setGroup(Scalability::Volumetrics, QualityLevel::Low);
    EXPECT_EQ(volumetrics::froxelGridFromCVars().z, 32u);
    {
        CVarScope off("r.Feature.Volumetrics", "false");
        render(cam, {.width = 96, .height = 64, .frames = 4}); // stats are from the last retired frame
        EXPECT_FALSE(ranPass("Volumetrics."));
    }
}

// 1080p GPU timings per Volumetrics quality level (fog with height fog + 8 fog volumes + 64 lights, clouds).
TEST_F(VolumetricsTest, PerfReport1080p) {
    Random rng(11);
    const Uuid ground = material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.8f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(160.0f));
    for (int i = 0; i < 120; ++i) {
        const Uuid m = material({rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 1.0f}, 0.0f, rng.range(0.2f, 0.9f));
        mesh(Primitive::Cube, m, {rng.range(-50.0f, 50.0f), 1.5f, rng.range(-60.0f, 20.0f)}, {1.0f, 3.0f, 1.0f});
    }
    for (int i = 0; i < 64; ++i) {
        pointLight({rng.range(-30.0f, 30.0f), 2.0f, rng.range(-40.0f, 10.0f)}, 3000.0f, 8.0f,
                   {rng.range(0.3f, 1.0f), rng.range(0.3f, 1.0f), rng.range(0.3f, 1.0f)}, i < 4);
    }
    for (int i = 0; i < 8; ++i) {
        Entity e = world->create("FogVolume");
        e.setPosition({rng.range(-30.0f, 30.0f), 2.0f, rng.range(-40.0f, 0.0f)});
        auto& fv = e.add<FogVolumeComponent>();
        fv.shape = FogVolumeShape(i % 3);
        fv.extents = glm::vec3(4.0f);
        fv.density = 0.2f;
        fv.noiseIntensity = 0.5f;
    }
    sun(glm::normalize(glm::vec3(-0.5f, -0.6f, -0.3f)), 40000.0f);
    fogEnvironment(0.02f, 0.1f);
    world->create("Clouds").add<CloudLayerComponent>();
    const CameraParams cam = camera({0, 6, 25}, {0, 8, -40}, 13.0f, 60.0f, 400.0f);
    CVarScope cache("r.Shadows.Caching", "false");
    std::printf("Volumetrics 1080p GPU ms (Apple M4 Pro):\n");
    const char* names[] = {"Low", "Medium", "High", "Ultra"};
    for (QualityLevel l : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
        scalability::setGroup(Scalability::Volumetrics, l);
        render(cam, {.width = 1920, .height = 1080, .frames = 8});
        const volumetrics::FroxelGrid g = volumetrics::froxelGridFromCVars();
        std::printf("  %-6s grid %ux%ux%u  inject %.3f  scatter %.3f  integrate %.3f  cloudTrace %.3f  "
                    "cloudReconstruct %.3f  composite %.3f  total %.3f  (frame %.3f)\n",
                    names[int(l)], g.x, g.y, g.z, passMs("Volumetrics.FogInject"), passMs("Volumetrics.FogScatter"),
                    passMs("Volumetrics.FogIntegrate"), passMs("Volumetrics.CloudTrace"),
                    passMs("Volumetrics.CloudReconstruct"), passMs("Volumetrics.Composite"), passMs("Volumetrics."),
                    renderer->stats().gpuFrameMs);
        EXPECT_GT(passMs("Volumetrics."), 0.0);
    }
}
