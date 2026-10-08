// Renderer behaviour tests: picking, feature injection, graph rebuilds, hot reload, caching, benchmark, perf.
#include "render_fixture.hpp"

#include <oxwald/render/quality.hpp>

#include <chrono>
#include <fstream>
#include <thread>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

namespace fs = std::filesystem;

using RendererTest = RenderTest;

namespace {

glm::ivec2 project(const CameraParams& cam, glm::vec3 p, u32 w, u32 h) {
    const glm::vec4 c = cam.projectionMatrix(f32(w) / f32(h)) * cam.viewMatrix() * glm::vec4(p, 1.0f);
    const glm::vec2 uv = glm::vec2(c) / c.w * 0.5f + 0.5f;
    return {int(uv.x * f32(w)), int(uv.y * f32(h))};
}

// Draws a magenta quad over the centre of SceneColorHDR at AfterOpaque (the extension API end to end).
struct MarkerFeature final : IRenderFeature {
    rhi::PipelineHandle pipeline;
    u32 setups = 0;
    std::string_view name() const override { return "TestMarker"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::AfterOpaque); }
    bool initialize(FeatureInitContext& ctx) override {
        rhi::GraphicsPipelineDesc d;
        d.name = "test.marker";
        d.vertex = fullscreenVertexShader();
        d.fragment = rhi::ShaderStageDesc::glsl(R"(#version 460
#include <render/common/view.glsl>
OX_PUSH_CONSTANTS({ ViewBuffer view; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() {
    if (any(lessThan(uv, vec2(0.4))) || any(greaterThan(uv, vec2(0.6)))) discard;
    // Radiance that maps to ~1.0 after exposure: magenta.
    o = vec4(vec3(1.0, 0.0, 1.0) / pc.view.v.exposure * pc.view.v.preExposure * 4.0, 1.0);
}
)", rhi::ShaderStage::Fragment, "test_marker.frag");
        d.colorFormats = {formats::kSceneColor};
        pipeline = ctx.device.createGraphicsPipeline(d);
        return ctx.device.vkPipeline(pipeline) != VK_NULL_HANDLE;
    }
    void shutdown(rhi::Device& dev) override { dev.destroy(pipeline); }
    void setup(FeatureContext& ctx) override {
        ++setups;
        const rhi::RGTexture hdr = ctx.resources().texture(res::kSceneColorHDR);
        const VkDeviceAddress view = ctx.viewAddress();
        ctx.graph().addPass("TestMarker").color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD).execute([this, view](rhi::PassContext& p) {
            drawFullscreen(p.cmd, pipeline, &view, sizeof(view));
        });
    }
};

// Compute feature writing the optional AO input (the pattern SSAO will use): AO = 0 on the left half.
struct HalfAoFeature final : IRenderFeature {
    rhi::PipelineHandle pipeline;
    std::string_view name() const override { return "TestHalfAO"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    std::vector<std::string_view> provides() const override { return {res::kAO}; }
    bool initialize(FeatureInitContext& ctx) override {
        rhi::ComputePipelineDesc d;
        d.name = "test.halfAo";
        d.shader = rhi::ShaderStageDesc::glsl(R"(#version 460
#include <render/common/view.glsl>
layout(local_size_x = 8, local_size_y = 8) in;
OX_PUSH_CONSTANTS({ uint dst; uint width; uint height; });
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= pc.width || p.y >= pc.height) return;
    OX_IMAGE_STORE_2D(r8, pc.dst, ivec2(p), vec4(p.x < pc.width / 2u ? 0.0 : 1.0));
}
)", rhi::ShaderStage::Compute, "test_half_ao.comp");
        pipeline = ctx.device.createComputePipeline(d);
        return true;
    }
    void shutdown(rhi::Device& dev) override { dev.destroy(pipeline); }
    void setup(FeatureContext& ctx) override {
        const Extent2D e = ctx.renderExtent();
        rhi::TextureDesc td;
        td.format = formats::kAO;
        td.width = e.width;
        td.height = e.height;
        td.usage = rhi::TextureUsage::None;
        td.name = "AO";
        const rhi::RGTexture ao = ctx.graph().createTexture(td);
        ctx.graph().addPass("TestHalfAO", rhi::PassType::Compute)
            .overwrite(ao, rhi::Access::StorageWriteCompute)
            .execute([this, ao, e](rhi::PassContext& p) {
                const u32 pc[3] = {p.storageIndex(ao), e.width, e.height};
                p.cmd.bindPipeline(pipeline);
                p.cmd.pushConstants(pc, sizeof(pc));
                p.cmd.dispatch((e.width + 7) / 8, (e.height + 7) / 8);
            });
        ctx.resources().setTexture(res::kAO, ao);
    }
};

} // namespace

TEST_F(RendererTest, PickingReturnsEntityIds) {
    const Uuid m = material({0.7f, 0.7f, 0.7f, 1.0f}, 0.0f, 0.5f);
    Entity a = mesh(Primitive::Cube, m, {-1.5f, 0, 0});
    Entity b = mesh(Primitive::Sphere, m, {0, 0, 0});
    Entity c = mesh(Primitive::Cylinder, m, {1.5f, 0, 0});
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    const CameraParams cam = camera({0, 0, 6}, {0, 0, 0}, 12.0f, 50.0f);
    Options o;
    o.editor = true;
    render(cam, o);
    struct Probe {
        Entity e;
        glm::vec3 p;
        PickRequestId id;
    } probes[] = {{a, {-1.5f, 0, 0}, 0}, {b, {0, 0, 0}, 0}, {c, {1.5f, 0, 0}, 0}, {Entity{}, {0, 3, 0}, 0}};
    for (Probe& p : probes) {
        const glm::ivec2 px = project(cam, p.p, 256, 256);
        p.id = renderer->requestPick(view, u32(px.x), u32(px.y));
        ASSERT_NE(p.id, 0u);
    }
    const PickRequestId rect = renderer->requestPick(view, 0, 0, 256, 256);
    render(cam, o); // records the picks
    render(cam, o); // retires them
    for (Probe& p : probes) {
        PickResult r = renderer->takePickResult(p.id);
        ASSERT_TRUE(r.ready);
        ASSERT_EQ(r.ids.size(), 1u);
        const u32 expected = p.e ? encodeEntityId(u32(entt::to_integral(p.e.handle()))) : kNoEntity;
        EXPECT_EQ(r.ids[0], expected);
        if (expected) EXPECT_EQ(decodeEntityId(r.ids[0]), u32(entt::to_integral(p.e.handle())));
    }
    PickResult all = renderer->takePickResult(rect);
    ASSERT_TRUE(all.ready);
    EXPECT_EQ(all.ids.size(), 256u * 256u);
    EXPECT_EQ(all.unique().size(), 3u);
    EXPECT_FALSE(renderer->takePickResult(rect).ready) << "results are taken once";
}

TEST_F(RendererTest, PickingAtHalfRenderScale) {
    CVarScope sp("r.ScreenPercentage", "50");
    Entity a = mesh(Primitive::Cube, material({1, 1, 1, 1}, 0, 0.5f), {0, 0, 0}, glm::vec3(2.0f));
    const CameraParams cam = camera({0, 0, 5}, {0, 0, 0}, 12.0f, 50.0f);
    Options o;
    o.editor = true;
    render(cam, o);
    const PickRequestId id = renderer->requestPick(view, 128, 128);
    render(cam, o);
    render(cam, o);
    PickResult r = renderer->takePickResult(id);
    ASSERT_TRUE(r.ready);
    EXPECT_EQ(r.x, 64u) << "output pixel mapped to render resolution";
    EXPECT_EQ(r.dominant(), encodeEntityId(u32(entt::to_integral(a.handle()))));
}

TEST_F(RendererTest, DummyFeatureInjectedAfterOpaqueRuns) {
    mesh(Primitive::Plane, material({0.2f, 0.2f, 0.2f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(20.0f));
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    const CameraParams cam = camera({0, 3, 6}, {0, 0, 0}, 12.0f, 50.0f);
    const Image before = render(cam);
    auto& marker = renderer->features().emplace<MarkerFeature>();
    const Image after = render(cam);
    EXPECT_GE(marker.setups, 2u);
    const glm::u8vec4 c = after.at(128, 128), c0 = before.at(128, 128);
    EXPECT_GT(c.r, 200);
    EXPECT_GT(c.b, 200);
    EXPECT_GT(int(c.r) - int(c.g), 60) << "magenta (ACES desaturates bright primaries a little)";
    EXPECT_LT(int(c0.r) - int(c0.g), 30) << "marker only exists with the feature";
    // Outside the quad the image is unchanged.
    EXPECT_EQ(after.at(10, 240), before.at(10, 240));
    // Disabling through the auto-registered toggle cvar removes it on the next frame.
    {
        CVarScope off("r.Feature.TestMarker", "false");
        const Image disabled = render(cam);
        EXPECT_EQ(disabled.at(128, 128), c0);
    }
}

TEST_F(RendererTest, ComputeFeatureProvidesLightingInput) {
    mesh(Primitive::Plane, material({0.9f, 0.9f, 0.9f, 1.0f}, 0.0f, 0.9f), {0, 0, 0}, glm::vec3(30.0f));
    environment(1.0f, 1.0f); // ambient only: AO must darken the left half
    const CameraParams cam = camera({0, 4, 0.01f}, {0, 0, 0}, 11.5f, 60.0f);
    const Image base = render(cam);
    renderer->features().emplace<HalfAoFeature>();
    const Image ao = render(cam);
    EXPECT_LT(ao.luminance(40, 128), base.luminance(40, 128) * 0.3f) << "AO=0 removes ambient on the left";
    EXPECT_NEAR(ao.luminance(216, 128), base.luminance(216, 128), 0.03f) << "AO=1 keeps the right half";
}

TEST_F(RendererTest, CVarToggleRebuildsGraphWithoutLeaks) {
    mesh(Primitive::Cube, material({0.7f, 0.7f, 0.7f, 1.0f}, 0.0f, 0.5f), {0, 0.5f, 0});
    mesh(Primitive::Plane, material({0.7f, 0.7f, 0.7f, 1.0f}, 0.0f, 0.5f), {0, 0, 0}, glm::vec3(10.0f));
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    pointLight({1, 1, 1}, 2000.0f, 5.0f, glm::vec3(1.0f), true);
    environment();
    const CameraParams cam = camera({0, 3, 6}, {0, 0, 0}, 12.0f, 50.0f);
    render(cam);
    render(cam);
    const u32 passesWith = renderer->stats().renderGraphPasses;
    rhi::GpuMemoryStats first{};
    for (int i = 0; i < 6; ++i) {
        {
            CVarScope off("r.Shadows", "false");
            render(cam, {.frames = 1});
            EXPECT_LT(renderer->stats().renderGraphPasses, passesWith) << "shadow passes removed";
            if (i == 0) EXPECT_GE(renderer->stats().renderGraphCompiles, 1u) << "graph recompiled after the change";
        }
        render(cam, {.frames = 1});
        EXPECT_EQ(renderer->stats().renderGraphPasses, passesWith);
        {
            CVarScope dv("r.DebugView", "Overdraw");
            render(cam, {.frames = 1});
        }
        device->waitIdle();
        if (i == 1) first = device->memoryStats();
    }
    // Run a few frames so deferred destruction retires everything, then compare object counts.
    render(cam, {.frames = 4});
    device->waitIdle();
    render(cam, {.frames = 4});
    const rhi::GpuMemoryStats last = device->memoryStats();
    EXPECT_LE(last.textureCount, first.textureCount + 2) << "textures leak when toggling cvars";
    EXPECT_LE(last.bufferCount, first.bufferCount + 2) << "buffers leak when toggling cvars";
}

TEST_F(RendererTest, ShadowCachingSkipsStaticLights) {
    const Uuid m = material({0.7f, 0.7f, 0.7f, 1.0f}, 0.0f, 0.5f);
    mesh(Primitive::Plane, m, {0, 0, 0}, glm::vec3(20.0f));
    mesh(Primitive::Cube, m, {0, 0.5f, 0});
    Entity mover = mesh(Primitive::Sphere, m, {6, 0.5f, 0});
    spotLight({0, 5, 2}, {0, -5, -2}, 20000.0f, 12.0f, 20.0f, 35.0f, true);
    pointLight({-6, 1.5f, 0}, 5000.0f, 4.0f, glm::vec3(1.0f), true);
    const CameraParams cam = camera({0, 6, 10}, {0, 0, 0}, 6.0f, 50.0f);
    render(cam, {.frames = 3});
    EXPECT_EQ(renderer->stats().shadowMapsRendered, 0u) << "static scene: everything cached";
    EXPECT_EQ(renderer->stats().shadowMapsCached, 2u);
    // Move the sphere into the spot light's range: only that light re-renders.
    mover.setPosition({1, 0.5f, 0});
    world->updateTransforms(); // previous != current until the next snapshotPreviousTransforms
    extract(*world, snapshot, {});
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
    EXPECT_EQ(renderer->stats().shadowMapsRendered, 1u) << "only the spot intersecting the moved object";
    EXPECT_EQ(renderer->stats().shadowMapsCached, 1u);
}

TEST_F(RendererTest, ShaderHotReloadOfRenderShaders) {
    // Copy the render shaders to a temp root that shadows engine/shaders/render, edit the tonemapper there.
    const fs::path root = fs::temp_directory_path() / std::format("oxwald_render_hot_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root);
    fs::copy(fs::path(OX_RENDER_SHADER_DIR) / "render", root / "render", fs::copy_options::recursive);
    // Recreate the device with the extra include root (searched before engine/shaders).
    renderer.reset();
    if (target) device->destroy(target);
    device->waitIdle();
    device.reset();
    rhi::DeviceDesc desc;
    desc.validation = true;
    desc.shaderOptions.includeRoots = {root};
    desc.shaderOptions.cacheDirectory = fs::temp_directory_path() / "oxwald_render_test_shader_cache";
    desc.hotReloadPollMs = 0;
    device = rhi::Device::create(desc);
    ASSERT_TRUE(device);
    renderer = Renderer::create(*device);
    target = {};
    view = 0;

    mesh(Primitive::Sphere, material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.5f), {0, 0, 0});
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    const CameraParams cam = camera({0, 0, 4}, {0, 0, 0}, 12.0f, 50.0f);
    const Image before = render(cam);
    EXPECT_FALSE(before.at(5, 5).g > 240 && before.at(5, 5).r < 20);

    const fs::path tonemap = root / "render/passes/tonemap.frag";
    std::string src;
    {
        std::ifstream in(tonemap);
        src.assign(std::istreambuf_iterator<char>(in), {});
    }
    const std::string marker = "    outColor = vec4(encoded + noise / 255.0, 1.0);";
    ASSERT_NE(src.find(marker), std::string::npos);
    src.replace(src.find(marker), marker.size(), "    outColor = vec4(0.0, 1.0, 0.0, 1.0); // hot reloaded");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // mtime granularity
    {
        std::ofstream out(tonemap, std::ios::trunc);
        out << src;
    }
    EXPECT_GE(device->reloadChangedShaders(true), 1u);
    const Image after = render(cam);
    const glm::u8vec4 px = after.at(5, 5);
    EXPECT_LT(px.r, 10);
    EXPECT_GT(px.g, 245);
    // A broken edit keeps the previous pipeline running.
    {
        std::ofstream out(tonemap, std::ios::trunc);
        out << "#version 460\nvoid main() { this does not compile }\n";
    }
    device->reloadChangedShaders(true);
    const Image broken = render(cam);
    EXPECT_GT(broken.at(5, 5).g, 245);
    rhi::Device::resetValidationCounters(); // the broken compile is logged as an error on purpose
    fs::remove_all(root);
}

TEST_F(RendererTest, AutoDetectQualityBenchmark) {
    const auto t0 = std::chrono::steady_clock::now();
    const BenchmarkResult r = autoDetectQuality(*device, {.width = 1920, .height = 1080, .iterations = 2});
    const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(r.valid);
    std::printf("%s\n(took %.2f s)\n", r.toString().c_str(), seconds);
    EXPECT_GT(r.fillRateGPixels, 0.5);
    EXPECT_GT(r.aluGFlops, 50.0);
    EXPECT_GT(r.bandwidthGBs, 5.0);
    EXPECT_GT(r.score, 1.0);
    EXPECT_LT(seconds, 5.0);
    EXPECT_EQ(r.levels[usize(Scalability::RayTracing)],
              device->caps().rayTracingSupported() ? r.levels[usize(Scalability::RayTracing)] : QualityLevel::Low);
}

TEST_F(RendererTest, PerfReport1080p) {
    // Test scene: 400 instances, sun + CSM, 64 point lights (8 shadowed), IBL. Prints per-pass GPU time.
    Random rng(5);
    const Uuid ground = material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.8f);
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
    const CameraParams cam = camera({0, 8, 30}, {0, 0, 0}, 13.0f, 60.0f, 300.0f);
    CVarScope cache("r.Shadows.Caching", "false"); // worst case: every shadow map every frame
    render(cam, {.width = 1920, .height = 1080, .frames = 6});
    const RenderStats& s = renderer->stats();
    std::printf("1080p perf (Apple M4 Pro):\n%s", s.toString().c_str());
    EXPECT_GT(s.gpuFrameMs, 0.0);
    EXPECT_GT(s.drawCalls, 10u);
}

TEST_F(RendererTest, UnlitMaterialsEmit) {
    auto unlit = [&](glm::vec3 emissive) {
        assets::MaterialAsset m;
        m.shadingModel = assets::ShadingModel::Unlit;
        m.baseColor = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
        m.emissive = emissive;
        const Uuid id = Uuid::generate();
        renderer->resources().addMaterial(id, m);
        return id;
    };
    mesh(Primitive::Cube, unlit(glm::vec3(0.0f)), {-1.2f, 0, 0});
    mesh(Primitive::Cube, unlit(glm::vec3(0.0f, 0.6f, 0.0f)), {1.2f, 0, 0});
    world->updateTransforms();
    const Image img = render(camera({0, 0, 5}, {0, 0, 0}, 12.0f), {.width = 128, .height = 128, .frames = 2});
    const auto plain = img.at(36, 64);
    const auto glowing = img.at(92, 64);
    EXPECT_GT(int(glowing.g), int(plain.g) + 60) << "emissive adds to unlit base colour";
    EXPECT_GT(int(glowing.g), int(glowing.r) + 40) << "green emission";
}

TEST_F(RendererTest, ClearcoatAddsASharpHighlight) {
    auto mat = [&](f32 clearcoat) {
        assets::MaterialAsset m;
        m.baseColor = glm::vec4(0.6f, 0.1f, 0.1f, 1.0f);
        m.roughness = 0.9f;
        m.clearcoat = clearcoat;
        m.clearcoatRoughness = 0.3f; // wide enough to cover several pixels at this resolution
        const Uuid id = Uuid::generate();
        renderer->resources().addMaterial(id, m);
        return id;
    };
    mesh(Primitive::Sphere, mat(0.0f), {-1.2f, 0, 0});
    mesh(Primitive::Sphere, mat(1.0f), {1.2f, 0, 0});
    sun(glm::normalize(glm::vec3(0.0f, -0.3f, -1.0f)), 20000.0f);
    environment(0.2f, 0.2f);
    world->updateTransforms();
    const Image img = render(camera({0, 0, 5}, {0, 0, 0}, 12.0f), {.width = 160, .height = 120, .frames = 2});
    auto peak = [&](u32 x0, u32 x1) {
        f32 best = 0.0f;
        for (u32 y = 20; y < 100; ++y)
            for (u32 x = x0; x < x1; ++x) best = std::max(best, img.luminance(x, y));
        return best;
    };
    const f32 plain = peak(10, 75), coated = peak(85, 150);
    EXPECT_GT(coated, plain + 0.15f) << "coat highlight on the rough base (plain " << plain << ", coated " << coated << ")";
}

TEST_F(RendererTest, ResourceRegistrationFromAnotherThreadIsDeferred) {
    const Image warmup = render(camera({0, 0, 5}, {0, 0, 0}, 12.0f), {.width = 64, .height = 64}); // owner = this thread
    (void)warmup;
    GpuResourceCache& cache = renderer->resources();
    std::vector<Uuid> ids(64);
    for (Uuid& id : ids) id = Uuid::generate();
    std::thread game([&] {
        for (usize i = 0; i < ids.size(); ++i) {
            assets::MaterialAsset m;
            m.baseColor = glm::vec4(f32(i) / 64.0f, 0.5f, 0.2f, 1.0f);
            cache.addMaterial(ids[i], m);
        }
    });
    game.join();
    EXPECT_EQ(cache.state(ids[0]), ResourceState::Unknown) << "queued until the render thread's next update()";
    cache.update();
    for (const Uuid& id : ids) EXPECT_EQ(cache.state(id), ResourceState::Ready);
}
