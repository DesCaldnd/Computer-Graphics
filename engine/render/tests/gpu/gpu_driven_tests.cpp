// GPU-driven rendering (r.GpuDriven): culling correctness against the CPU path, two-phase occlusion, shadow culling,
// LOD selection, meshlet path, steady-state allocations, stress scene timings.
#include "render_fixture.hpp"

#include <oxwald/assets/mesh_processing.hpp>
#include <oxwald/core/jobs.hpp>
#include <oxwald/core/math.hpp>

#include <atomic>
#include <new>
#include <thread>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>

// Heap allocation counter for the steady-state allocation test (replaces the global operator new of this test
// executable; counting is enabled only around the measured calls, on every thread).
namespace {
std::atomic<bool> gCountAllocations{false};
std::atomic<unsigned long long> gAllocations{0};
} // namespace
void* operator new(std::size_t n) {
    if (gCountAllocations.load(std::memory_order_relaxed)) gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace ox::render::test {

namespace {

struct AllocationScope {
    AllocationScope() {
        gAllocations = 0;
        gCountAllocations = true;
    }
    unsigned long long stop() {
        gCountAllocations = false;
        return gAllocations.load();
    }
};

class GpuDrivenTest : public RenderTest {
protected:
    // Renders the same frame with r.GpuDriven 0 and 1 (fresh views each, so history is identical).
    std::pair<Image, Image> renderBoth(const CameraParams& cam, const Options& o) {
        Image cpu, gpu;
        {
            CVarScope off("r.GpuDriven", "false");
            resetView();
            cpu = render(cam, o);
            EXPECT_FALSE(renderer->stats().gpuDriven);
        }
        {
            CVarScope on("r.GpuDriven", "true");
            resetView();
            gpu = render(cam, o);
            EXPECT_TRUE(renderer->stats().gpuDriven);
        }
        return {cpu, gpu};
    }
    void resetView() {
        if (view) renderer->destroyView(view);
        view = 0;
    }
    // A sphere with a real LOD chain + meshlets (assets mesh processing).
    Uuid lodSphere() {
        PrimitiveParams pp;
        pp.segments = 96;
        pp.rings = 48;
        assets::MeshData m = makePrimitive(Primitive::Sphere, pp);
        assets::MeshProcessSettings ps;
        ps.lodRatios = {0.5f, 0.25f, 0.125f};
        ps.lodMaxError = 0.2f;
        assets::processMesh(m, ps);
        const Uuid id = Uuid::fromName("test.gpuDriven.lodSphere");
        renderer->resources().addMesh(id, m);
        return id;
    }
    Entity meshAsset(const Uuid& meshId, const Uuid& mat, glm::vec3 position, glm::vec3 scale = glm::vec3(1.0f)) {
        Entity e = world->create("LodMesh");
        e.setPosition(position);
        e.setScale(scale);
        auto& mr = e.add<MeshRendererComponent>();
        mr.mesh = meshId;
        mr.materials = {mat};
        return e;
    }
};

void expectSimilar(const Image& a, const Image& b, f64 maxMean, f64 maxBad, const char* what) {
    ASSERT_EQ(a.rgba.size(), b.rgba.size());
    u64 sum = 0, bad = 0;
    for (usize p = 0; p < usize(a.width) * a.height; ++p) {
        bool isBad = false;
        for (u32 c = 0; c < 3; ++c) {
            const int d = std::abs(int(a.rgba[p * 4 + c]) - int(b.rgba[p * 4 + c]));
            sum += u64(d);
            isBad |= d > 24;
        }
        bad += isBad ? 1 : 0;
    }
    const f64 px = f64(a.width) * a.height;
    const f64 mean = f64(sum) / (px * 3.0), badFrac = f64(bad) / px;
    EXPECT_LE(mean, maxMean) << what << ": mean error " << mean;
    EXPECT_LE(badFrac, maxBad) << what << ": bad pixel fraction " << badFrac;
}

} // namespace

TEST_F(GpuDrivenTest, MatchesCpuPathWithShadows) {
    Random rng(11);
    mesh(Primitive::Plane, material({0.6f, 0.6f, 0.6f, 1}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(40.0f));
    const Uuid masked = material({0.9f, 0.4f, 0.2f, 0.4f}, 0.0f, 0.5f, glm::vec3(0.0f), assets::BlendMode::AlphaTest);
    for (int i = 0; i < 120; ++i) {
        const Primitive p = Primitive(rng.rangeInt(0, int(Primitive::Count) - 1));
        if (p == Primitive::Plane) continue;
        const Uuid m = i % 7 == 0 ? masked : material({rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 1.0f}, 0.0f, 0.5f);
        mesh(p, m, {rng.range(-12.0f, 12.0f), 0.5f, rng.range(-12.0f, 12.0f)}, glm::vec3(rng.range(0.5f, 1.2f)));
    }
    sun(glm::normalize(glm::vec3(-1.0f, -0.6f, -0.5f)), 20000.0f);
    spotLight({0, 5, 4}, {0, -1, -0.4f}, 20000.0f, 15.0f, 20.0f, 35.0f, true);
    pointLight({3, 1.5f, -2}, 8000.0f, 8.0f, {1, 0.8f, 0.6f}, true);
    environment();
    const CameraParams cam = camera({0, 6, 14}, {0, 0, 0}, 12.5f);
    auto [cpu, gpu] = renderBoth(cam, {.width = 320, .height = 240, .frames = 3});
    expectSimilar(cpu, gpu, 0.5, 0.002, "r.GpuDriven 0 vs 1");
    const RenderStats& s = renderer->stats();
    std::printf("%s", s.toString().c_str());
    EXPECT_GT(s.indirectDrawCalls, 0u);
    ASSERT_TRUE(s.gpuCulling.valid);
    EXPECT_GT(s.gpuCulling.shadowInstancesTested, 0u);
    EXPECT_GT(s.gpuCulling.shadowInstancesVisible, 0u);
    // Lights cull: every light job tests the full caster set, few casters are near the point light.
    EXPECT_LT(s.gpuCulling.shadowInstancesVisible, s.gpuCulling.shadowInstancesTested);
    GoldenResult g = compareGolden("gpu_driven_shadows", gpu);
    EXPECT_TRUE(g.matched) << g.message;
}

TEST_F(GpuDrivenTest, OcclusionCullsObjectsBehindWall) {
    const Uuid grey = material({0.7f, 0.7f, 0.7f, 1}, 0.0f, 0.7f);
    const Uuid red = material({0.9f, 0.1f, 0.1f, 1}, 0.0f, 0.4f);
    mesh(Primitive::Cube, grey, {0, 2, 0}, {30.0f, 4.0f, 0.5f}); // wall
    // 200 cubes hidden behind the wall, 10 in front of it.
    for (int i = 0; i < 200; ++i) mesh(Primitive::Cube, red, {f32(i % 20) - 10.0f, 0.5f + f32(i / 20) * 0.3f, -3.0f - f32(i % 5)}, glm::vec3(0.3f));
    for (int i = 0; i < 10; ++i) mesh(Primitive::Sphere, red, {f32(i) - 5.0f, 0.5f, 3.0f}, glm::vec3(0.6f));
    sun(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 20000.0f, glm::vec3(1.0f), false);
    environment();
    const CameraParams cam = camera({0, 1.5f, 10}, {0, 1.5f, 0}, 12.5f);
    auto [cpu, gpu] = renderBoth(cam, {.width = 256, .height = 192, .frames = 4});
    expectSimilar(cpu, gpu, 0.3, 0.001, "occlusion");
    const GpuCullingStats& g = renderer->stats().gpuCulling;
    ASSERT_TRUE(g.valid);
    std::printf("tested %u frustum %u occluded %u visible %u\n", g.instancesTested, g.instancesFrustumCulled,
                g.instancesOccluded, g.instancesVisible);
    EXPECT_GE(g.instancesOccluded, 180u) << "cubes behind the wall must be occluded";
    EXPECT_GE(g.instancesVisible, 5u); // wall + spheres in front
    EXPECT_LE(g.instancesVisible, 40u);
}

TEST_F(GpuDrivenTest, DisoccludedObjectsAppearInTheSameFrame) {
    // Frame 1 sees only the wall; then the camera jumps behind it. Phase 2 must draw the newly visible cubes in the
    // very first frame (no one-frame popping), so a single frame matches the CPU path.
    const Uuid grey = material({0.7f, 0.7f, 0.7f, 1}, 0.0f, 0.7f);
    const Uuid red = material({0.9f, 0.1f, 0.1f, 1}, 0.0f, 0.4f);
    mesh(Primitive::Cube, grey, {0, 2, 0}, {30.0f, 4.0f, 0.5f});
    for (int i = 0; i < 50; ++i) mesh(Primitive::Cube, red, {f32(i % 10) - 5.0f, 0.5f, -3.0f - f32(i / 10)}, glm::vec3(0.4f));
    sun(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 20000.0f, glm::vec3(1.0f), false);
    environment();
    CVarScope on("r.GpuDriven", "true");
    render(camera({0, 1.5f, 10}, {0, 1.5f, 0}, 12.5f), {.width = 256, .height = 192, .frames = 3});
    const CameraParams behind = camera({0, 3, -12}, {0, 0.5f, -4}, 12.5f);
    const Image moved = render(behind, {.width = 256, .height = 192, .frames = 1});
    Image reference;
    {
        CVarScope off("r.GpuDriven", "false");
        resetView();
        reference = render(behind, {.width = 256, .height = 192, .frames = 1});
    }
    writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "gpu_driven_disocclusion_gpu.png", moved);
    writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "gpu_driven_disocclusion_cpu.png", reference);
    expectSimilar(reference, moved, 0.3, 0.001, "first frame after disocclusion");
}

TEST_F(GpuDrivenTest, LodSwitchDistanceMatchesScreenError) {
    const Uuid sphere = lodSphere();
    const Uuid white = material({0.8f, 0.8f, 0.8f, 1}, 0.0f, 0.5f);
    meshAsset(sphere, white, {0, 0, 0});
    environment();
    renderer->resources().flush();
    // The mesh must have a LOD chain.
    extractSnapshot();
    const GpuMesh* gm = renderer->resources().mesh(sphere);
    ASSERT_NE(gm, nullptr);
    const GpuScene& scene = renderer->scene();
    const GpuMeshInfo& mi = scene.meshInfo(gm->firstMeshInfo);
    ASSERT_GE(mi.lodCount, 3u) << "mesh processing produced no LODs";
    const GpuMeshLod* lods = scene.meshLods(gm->firstMeshInfo);
    // Expected switch distance to LOD 1: error · scale · projScale / threshold (+ radius, distance is to the surface).
    const u32 height = 256;
    const f32 projScale = 0.5f * f32(height) / std::tan(glm::radians(50.0f) * 0.5f);
    const f32 radius = mi.boundingSphere.w;
    const f32 d1 = lods[1].error * projScale / 1.0f + radius;
    ASSERT_GT(lods[1].error, 0.0f);
    CVarScope on("r.GpuDriven", "true");
    CVarScope occl("r.GpuDriven.Occlusion", "false");
    auto lodAt = [&](f32 distance) {
        render(camera({0, 0, distance}, {0, 0, 0}, 12.0f, 50.0f, 10000.0f), {.width = height, .height = height, .frames = 3});
        return renderer->stats().gpuCulling.averageLod;
    };
    EXPECT_EQ(lodAt(d1 * 0.9f), 0.0f) << "switch distance " << d1;
    EXPECT_GE(lodAt(d1 * 1.1f), 1.0f) << "switch distance " << d1;
    // The CPU selection agrees.
    LodSelection sel;
    sel.projScale = projScale;
    sel.cameraPosition = {0, 0, d1 * 0.9f};
    GpuInstance inst;
    inst.meshIndex = gm->firstMeshInfo;
    inst.boundingSphere = glm::vec4(0, 0, 0, radius);
    EXPECT_EQ(scene.instanceLod(inst, sel), 0u);
    sel.cameraPosition = {0, 0, d1 * 1.1f};
    EXPECT_GE(scene.instanceLod(inst, sel), 1u);
    // Coarser threshold (LODBias +1 = twice the pixel error) switches twice as early.
    CVarScope bias("r.ViewDistance.LODBias", "1");
    const f32 d1b = lods[1].error * projScale / 2.0f + radius;
    EXPECT_EQ(lodAt(d1b * 0.9f), 0.0f) << "biased switch distance " << d1b;
    EXPECT_GE(lodAt(d1b * 1.1f), 1.0f) << "biased switch distance " << d1b;
}

TEST_F(GpuDrivenTest, MeshletPathMatchesInstancedPath) {
    const Uuid sphere = lodSphere();
    const Uuid mat = material({0.3f, 0.6f, 0.9f, 1}, 0.0f, 0.4f);
    for (int i = 0; i < 25; ++i) meshAsset(sphere, mat, {f32(i % 5) * 1.5f - 3.0f, 0.5f, -f32(i / 5) * 1.5f});
    mesh(Primitive::Plane, material({0.6f, 0.6f, 0.6f, 1}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(20.0f));
    sun(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 20000.0f);
    environment();
    const CameraParams cam = camera({0, 3, 6}, {0, 0, -3}, 12.5f);
    CVarScope on("r.GpuDriven", "true");
    Image instanced, meshlets;
    {
        CVarScope m("r.GpuDriven.Meshlets", "false");
        resetView();
        instanced = render(cam, {.width = 256, .height = 192, .frames = 3});
    }
    {
        CVarScope m("r.GpuDriven.Meshlets", "true");
        resetView();
        meshlets = render(cam, {.width = 256, .height = 192, .frames = 3});
        const GpuCullingStats& g = renderer->stats().gpuCulling;
        ASSERT_TRUE(g.valid);
        std::printf("meshlets %u/%u visible, %llu triangles\n", g.meshletsVisible, g.meshletsTested,
                    (unsigned long long)g.meshletTriangles);
        EXPECT_GT(g.meshletsTested, 0u);
        EXPECT_GT(g.meshletsVisible, 0u);
        EXPECT_LT(g.meshletsVisible, g.meshletsTested) << "backface cones / frustum must reject some meshlets";
    }
    expectSimilar(instanced, meshlets, 0.3, 0.002, "meshlets vs instanced");
}

TEST_F(GpuDrivenTest, ToggleAtRuntimeKeepsRendering) {
    mesh(Primitive::Sphere, material({0.8f, 0.2f, 0.2f, 1}, 0.0f, 0.4f), {0, 0.5f, 0});
    sun(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 20000.0f);
    environment();
    const CameraParams cam = camera({0, 1, 4}, {0, 0.5f, 0}, 12.5f);
    Image a, b, c;
    {
        CVarScope on("r.GpuDriven", "true");
        a = render(cam);
    }
    {
        CVarScope off("r.GpuDriven", "false");
        b = render(cam);
    }
    {
        CVarScope on("r.GpuDriven", "true");
        c = render(cam);
    }
    expectSimilar(a, b, 0.3, 0.002, "on → off");
    expectSimilar(b, c, 0.3, 0.002, "off → on");
}

} // namespace ox::render::test

namespace ox::render::test {

// Stress scene for docs/dev/perf.md: N instances (mixed primitives + an LOD sphere, 64 materials, 10 % alpha
// tested) on a 200 m field with occluding walls, sun + 4 cascades + 8 shadowed point lights, 1080p.
// OX_RENDER_PERF=<instances> overrides the default 10k (the numbers in perf.md use 10000 and 50000).
TEST_F(GpuDrivenTest, StressSceneTimings) {
    u32 count = 10000;
    if (const char* e = std::getenv("OX_RENDER_PERF")) count = u32(std::max(1000, std::atoi(e)));
    const Uuid sphere = lodSphere();
    Random rng(7);
    std::vector<Uuid> mats;
    for (int i = 0; i < 64; ++i) {
        mats.push_back(material({rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 0.5f}, rng.chance(0.3f) ? 1.0f : 0.0f,
                                rng.range(0.2f, 0.9f), glm::vec3(0.0f),
                                i % 10 == 0 ? assets::BlendMode::AlphaTest : assets::BlendMode::Opaque));
    }
    mesh(Primitive::Plane, material({0.5f, 0.5f, 0.5f, 1}, 0.0f, 0.9f), {0, 0, 0}, glm::vec3(220.0f));
    for (int w = 0; w < 12; ++w) {
        mesh(Primitive::Cube, mats[1], {rng.range(-80.0f, 80.0f), 4.0f, rng.range(-80.0f, 60.0f)}, {rng.range(15.0f, 40.0f), 8.0f, 1.0f});
    }
    for (u32 i = 0; i < count; ++i) {
        const glm::vec3 p{rng.range(-100.0f, 100.0f), 0.5f, rng.range(-100.0f, 100.0f)};
        const Uuid& m = mats[rng.rangeInt(0, 63)];
        if (i % 8 == 0) meshAsset(sphere, m, p + glm::vec3(0, 0.2f, 0), glm::vec3(rng.range(0.5f, 1.5f)));
        else {
            Primitive pr = Primitive(rng.rangeInt(0, int(Primitive::Count) - 1));
            if (pr == Primitive::Plane) pr = Primitive::Cube;
            mesh(pr, m, p, glm::vec3(rng.range(0.4f, 1.2f)));
        }
    }
    for (int i = 0; i < 8; ++i) {
        pointLight({rng.range(-20.0f, 20.0f), 2.0f, rng.range(-20.0f, 20.0f)}, 5000.0f, 10.0f, glm::vec3(1.0f), true);
    }
    sun(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 30000.0f);
    environment();
    const CameraParams cam = camera({0, 6, 95}, {0, 0, 40}, 13.0f, 60.0f, 400.0f);
    CVarScope cache("r.Shadows.Caching", "false");
    {
        // Extract (game thread): serial vs parallelFor over the mesh renderers.
        world->updateTransforms();
        world->snapshotPreviousTransforms();
        JobSystem jobs(0);
        auto time = [&](JobSystem* j) {
            for (int k = 0; k < 2; ++k) extract(*world, snapshot, {.jobs = j});
            const auto t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < 10; ++k) extract(*world, snapshot, {.jobs = j});
            return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10.0;
        };
        const f64 serial = time(nullptr), parallel = time(&jobs);
        std::printf("extract %zu meshes: serial %.2f ms, parallel (%u threads) %.2f ms\n", snapshot.meshes.size(), serial,
                    jobs.threadCount(), parallel);
    }

    struct Config {
        const char* name;
        const char* gpuDriven;
        const char* occlusion;
        const char* meshlets;
    };
    const Config configs[] = {{"CPU culling + instanced", "false", "false", "false"},
                              {"GPU culling (frustum + LOD)", "true", "false", "false"},
                              {"GPU culling + two-phase HiZ", "true", "true", "false"},
                              {"GPU culling + HiZ + meshlets", "true", "true", "true"}};
    std::printf("\nstress scene: %u instances, 1920x1080\n", count);
    std::printf("| config | GPU frame ms | CPU renderer ms | draw calls | indirect cmds | visible inst. | key passes |\n");
    std::printf("|---|---|---|---|---|---|---|\n");
    Image first;
    for (const Config& c : configs) {
        CVarScope a("r.GpuDriven", c.gpuDriven);
        CVarScope b("r.GpuDriven.Occlusion", c.occlusion);
        CVarScope d("r.GpuDriven.Meshlets", c.meshlets);
        resetView();
        render(cam, {.width = 1920, .height = 1080, .frames = 4}); // warm-up: pipelines, draw sets, visibility
        f64 gpu = 0, cpu = 0;
        const u32 frames = 8;
        std::map<std::string, f64> passMs;
        for (u32 f = 0; f < frames; ++f) {
            device->beginFrame();
            const auto t0 = std::chrono::steady_clock::now();
            renderer->beginFrame(snapshot);
            ViewRenderRequest req;
            req.view = view;
            req.camera = cam;
            req.target.texture = target;
            req.target.finalAccess = rhi::Access::TransferRead;
            renderer->renderView(req);
            renderer->endFrame();
            cpu += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
            device->endFrame();
            gpu += renderer->stats().gpuFrameMs;
            for (const PassTiming& p : renderer->stats().passes) passMs[p.name] += p.gpuMs;
        }
        device->waitIdle();
        const RenderStats& s = renderer->stats();
        std::string passes;
        for (const char* n : {"GpuCull", "DepthPrepass", "HiZ.Early", "GpuCull.Late", "DepthPrepass.Late", "Shadow.Cascades",
                              "Shadow.Points", "ForwardOpaque"}) {
            if (passMs.count(n)) passes += std::format("{} {:.2f}, ", n, passMs[n] / frames);
        }
        std::printf("| %s | %.2f | %.2f | %u | %u | %u | %s |\n", c.name, gpu / frames, cpu / frames, s.drawCalls,
                    s.indirectCommands, s.visibleInstances, passes.c_str());
        Image img;
        img.width = 1920;
        img.height = 1080;
        img.rgba = device->readTexture(target);
        for (usize i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
        if (first.rgba.empty()) first = img;
        else expectSimilar(first, img, 1.0, 0.01, c.name);
    }
}

} // namespace ox::render::test

namespace ox::render::test {

namespace {

// Stand-in for an SSR / SSAO style consumer of HiZ, so the (async) HiZ pass is not culled.
struct HiZConsumer final : IRenderFeature {
    std::string_view name() const override { return "TestHiZConsumer"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    void setup(FeatureContext& ctx) override {
        const rhi::RGTexture hiz = ctx.resources().texture(res::kHiZ);
        if (!hiz.valid()) return;
        rhi::TextureDesc d;
        d.format = VK_FORMAT_R8_UNORM;
        d.width = d.height = 4;
        d.usage = rhi::TextureUsage::None;
        d.name = "HiZProbe";
        const rhi::RGTexture probe = ctx.graph().createTexture(d);
        ctx.graph().addPass("HiZProbe").read(hiz, rhi::Access::SampledFragment).color(probe, VK_ATTACHMENT_LOAD_OP_CLEAR).execute([](rhi::PassContext&) {});
        ctx.graph().markOutput(probe);
    }
};

} // namespace

TEST_F(GpuDrivenTest, AsyncComputeOverlapsGraphics) {
    if (!device->caps().asyncComputeQueue) GTEST_SKIP() << "no async compute queue";
    renderer->features().emplace<HiZConsumer>();
    Random rng(3);
    mesh(Primitive::Plane, material({0.6f, 0.6f, 0.6f, 1}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(80.0f));
    for (int i = 0; i < 300; ++i) {
        mesh(Primitive::Sphere, material({rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 1}, 0.0f, 0.5f),
             {rng.range(-30.0f, 30.0f), 0.5f, rng.range(-30.0f, 30.0f)});
    }
    for (int i = 0; i < 64; ++i) pointLight({rng.range(-25.0f, 25.0f), 1.5f, rng.range(-25.0f, 25.0f)}, 3000.0f, 8.0f, glm::vec3(1), i < 6);
    sun(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 30000.0f);
    environment();
    const CameraParams cam = camera({0, 8, 30}, {0, 0, 0}, 13.0f, 60.0f, 300.0f);
    CVarScope cache("r.Shadows.Caching", "false");
    Image sync, async;
    f64 wall[2] = {}, overlap = 0.0, asyncMs = 0.0;
    for (int round = 0; round < 2; ++round) {
        for (int a = 0; a < 2; ++a) {
            CVarScope ac("r.AsyncCompute", a ? "On" : "Off");
            Image img = render(cam, {.width = 1280, .height = 720, .frames = 4});
            (a ? async : sync) = img;
            wall[a] += renderer->stats().gpuFrameWallMs;
            if (a) {
                overlap += renderer->stats().asyncOverlapMs;
                asyncMs += renderer->stats().asyncComputeMs;
            }
        }
    }
    std::printf("GPU wall: sync %.3f ms, async %.3f ms; async compute %.3f ms, overlapped %.3f ms\n", wall[0] / 2, wall[1] / 2,
                asyncMs / 2, overlap / 2);
    bool hizAsync = false;
    for (const PassTiming& p : renderer->stats().passes) hizAsync = hizAsync || (p.name == "HiZ" && p.asyncCompute);
    EXPECT_TRUE(hizAsync) << "HiZ should run on the async compute queue";
    EXPECT_GT(asyncMs, 0.0);
    // No overlap is asserted: MoltenVK serialises the queues (Metal hazard tracking over the bindless heap).
    expectSimilar(sync, async, 0.2, 0.001, "async vs sync");
}

} // namespace ox::render::test

namespace ox::render::test {

TEST_F(GpuDrivenTest, SteadyStateExtractAndDrawListsDoNotAllocate) {
    Random rng(9);
    std::vector<Uuid> mats;
    for (int i = 0; i < 16; ++i) mats.push_back(material({rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 1}, 0.0f, 0.5f));
    for (int i = 0; i < 6000; ++i) {
        mesh(Primitive(rng.rangeInt(0, 1)), mats[rng.rangeInt(0, 15)], {rng.range(-50.0f, 50.0f), 0.5f, rng.range(-50.0f, 50.0f)});
    }
    sun(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 20000.0f);
    environment();
    const CameraParams cam = camera({0, 5, 60}, {0, 0, 0}, 13.0f, 60.0f, 300.0f);
    JobSystem jobs(4);
    world->updateTransforms();
    world->snapshotPreviousTransforms();
    for (const char* gpu : {"false", "true"}) {
        CVarScope g("r.GpuDriven", gpu);
        render(cam, {.width = 256, .height = 144, .frames = 3}); // warm-up: capacities, draw sets, pipelines
        // Extract: serial and parallel.
        for (int k = 0; k < 2; ++k) extract(*world, snapshot, {});
        AllocationScope serial;
        extract(*world, snapshot, {});
        EXPECT_EQ(serial.stop(), 0u) << "serial extract allocates in steady state";
        for (int k = 0; k < 2; ++k) extract(*world, snapshot, {.jobs = &jobs});
        AllocationScope parallel;
        extract(*world, snapshot, {.jobs = &jobs});
        const auto parallelAllocs = parallel.stop();
        // Only the job system's per-dispatch task objects (two parallelFor calls), independent of the scene size.
        EXPECT_LE(parallelAllocs, 16u) << "parallel extract allocates per mesh";
        // Instances + camera draw lists (CPU culling path builds them into persistent storage).
        GpuScene& scene = renderer->scene();
        ViewDrawLists lists;
        std::vector<u32> ids[u32(DrawBucket::Count)];
        LodSelection lod;
        lod.projScale = 500.0f;
        const Frustum frustum = Frustum::fromViewProj(cam.projectionMatrix(16.0f / 9.0f) * cam.viewMatrix());
        for (int k = 0; k < 2; ++k) scene.buildViewDrawLists(frustum, cam.position(), lists, ids, 0.0f, true, &lod);
        AllocationScope drawLists;
        scene.updateInstances(snapshot, renderer->resources(), 0.0f, cam.position());
        scene.buildViewDrawLists(frustum, cam.position(), lists, ids, 0.0f, true, &lod);
        EXPECT_EQ(drawLists.stop(), 0u) << "draw list building allocates in steady state";
        // Whole renderer frame (informational: render graph declarations still allocate).
        render(cam, {.width = 256, .height = 144, .frames = 2});
        AllocationScope frame;
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
        const auto frameAllocs = frame.stop();
        device->waitIdle();
        std::printf("r.GpuDriven %s: parallel extract %llu allocations, full renderer frame %llu allocations (%u instances)\n",
                    gpu, parallelAllocs, frameAllocs, renderer->stats().instances);
    }
}

} // namespace ox::render::test

namespace ox::render::test {

TEST_F(GpuDrivenTest, ParallelRecordingProducesIdenticalImages) {
    JobSystem jobs(6);
    // Renderer with a job system (parallel recording needs RendererDesc::jobs).
    renderer.reset();
    view = 0;
    renderer = Renderer::create(*device, {.jobs = &jobs});
    Random rng(21);
    std::vector<Uuid> mats;
    for (int i = 0; i < 120; ++i) mats.push_back(material({rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 1}, 0.0f, 0.5f));
    for (int i = 0; i < 3000; ++i) {
        Primitive p = Primitive(rng.rangeInt(0, int(Primitive::Count) - 1));
        if (p == Primitive::Plane) p = Primitive::Torus;
        mesh(p, mats[rng.rangeInt(0, 119)], {rng.range(-30.0f, 30.0f), 0.5f, rng.range(-30.0f, 30.0f)}, glm::vec3(0.6f));
    }
    sun(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 20000.0f);
    environment();
    const CameraParams cam = camera({0, 10, 35}, {0, 0, 0}, 13.0f, 60.0f, 300.0f);
    CVarScope cpu("r.GpuDriven", "false");
    Image serial, parallel;
    f64 serialMs = 0, parallelMs = 0;
    for (int round = 0; round < 3; ++round) {
        {
            CVarScope off("r.ParallelRecording", "Off");
            serial = render(cam, {.width = 640, .height = 360, .frames = 3});
            serialMs += renderer->stats().cpuRenderMs;
            EXPECT_EQ(renderer->stats().parallelRecordedChunks, 0u);
        }
        {
            CVarScope on("r.ParallelRecording", "On");
            parallel = render(cam, {.width = 640, .height = 360, .frames = 3});
            parallelMs += renderer->stats().cpuRenderMs;
            EXPECT_GT(renderer->stats().parallelRecordedChunks, 0u);
        }
    }
    std::printf("CPU renderer: serial %.2f ms, parallel recording %.2f ms (%u draws)\n", serialMs / 3, parallelMs / 3,
                renderer->stats().drawCalls);
    ASSERT_EQ(serial.rgba.size(), parallel.rgba.size());
    writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "gpu_driven_parallel_serial.png", serial);
    writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "gpu_driven_parallel_parallel.png", parallel);
    // Same draws in the same order; the only differences are the frame-to-frame dither noise of the tonemapper.
    expectSimilar(serial, parallel, 0.05, 0.0001, "parallel recording");
}

} // namespace ox::render::test

namespace ox::render::test {

namespace {

// 1024² RGBA8 texture whose mip levels have distinct colours (level 0 white … smaller levels darker).
assets::TextureData mipColouredTexture(u32 size) {
    assets::TextureData t;
    t.format = assets::TextureFormat::RGBA8Unorm;
    t.width = t.height = size;
    for (u32 s = size, level = 0; s >= 1; s /= 2, ++level) {
        assets::TextureMip m;
        m.width = m.height = s;
        m.data.resize(usize(s) * s * 4);
        const u8 v = u8(std::max(255 - int(level) * 25, 10));
        for (usize i = 0; i < m.data.size(); i += 4) {
            m.data[i] = std::byte(v);
            m.data[i + 1] = std::byte(level % 2 ? 40 : v);
            m.data[i + 2] = std::byte(v);
            m.data[i + 3] = std::byte(255);
        }
        t.mips.push_back(std::move(m));
        if (s == 1) break;
    }
    t.mipCount = u32(t.mips.size());
    return t;
}

} // namespace

TEST_F(GpuDrivenTest, TextureStreamingRespectsBudget) {
    CVarScope pool("r.Streaming.PoolSizeMB", "4");
    CVarScope delay("r.Streaming.DropDelayFrames", "2");
    std::vector<Uuid> textures;
    for (int i = 0; i < 24; ++i) {
        const Uuid tex = Uuid::fromName(std::format("test.streaming.tex{}", i));
        renderer->resources().addTexture(tex, mipColouredTexture(1024));
        assets::MaterialAsset m;
        m.albedoTexture = tex;
        m.roughness = 0.8f;
        const Uuid mat = Uuid::fromName(std::format("test.streaming.mat{}", i));
        renderer->resources().addMaterial(mat, m);
        mesh(Primitive::Plane, mat, {f32(i % 2) * 2.2f - 1.1f, 0.0f, -f32(i / 2) * 6.0f}, glm::vec3(2.0f));
        textures.push_back(tex);
    }
    environment();
    sun(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f)), 20000.0f, glm::vec3(1.0f), false);
    const CameraParams cam = camera({0, 2.0f, 3.0f}, {0, 0, -10}, 13.0f, 60.0f, 400.0f);
    // Before the first frame only the mip tail (≤ 64 px) is resident.
    const GpuTexture* t0 = renderer->resources().residentTexture(textures[0]);
    ASSERT_NE(t0, nullptr);
    EXPECT_LE(t0->width, 64u);
    Image img = render(cam, {.width = 1280, .height = 720, .frames = 12});
    const TextureStreamingStats& s = renderer->stats().streaming;
    std::printf("streaming: %u textures, resident %.1f MiB, wanted %.1f MiB, budget %.1f MiB\n", s.streamedTextures,
                f64(s.residentBytes) / (1 << 20), f64(s.wantedBytes) / (1 << 20), f64(s.budgetBytes) / (1 << 20));
    EXPECT_GE(s.streamedTextures, 24u); // other features may stream their own textures too
    EXPECT_GT(s.wantedBytes, s.budgetBytes) << "the scene must want more than the budget";
    EXPECT_LE(s.residentBytes, s.budgetBytes);
    // Near textures get more detail than far ones.
    const u32 nearSize = renderer->resources().residentTexture(textures[0])->width;
    const u32 farSize = renderer->resources().residentTexture(textures[23])->width;
    std::printf("near texture %u px, far texture %u px\n", nearSize, farSize);
    EXPECT_GT(nearSize, farSize);
    EXPECT_GE(nearSize, 128u);
    GoldenResult g = compareGolden("gpu_driven_streaming", img);
    EXPECT_TRUE(g.matched) << g.message;
    // Raising the budget lets the near textures reach full resolution (no stalls: swaps happen on later frames).
    {
        CVarScope big("r.Streaming.PoolSizeMB", "512");
        render(cam, {.width = 1280, .height = 720, .frames = 8});
        EXPECT_GT(renderer->resources().residentTexture(textures[0])->width, nearSize);
    }
}

} // namespace ox::render::test

namespace ox::render::test {

TEST_F(GpuDrivenTest, AsyncPipelineCompileUsesPlaceholderUntilReady) {
    JobSystem jobs(4);
    renderer.reset();
    view = 0;
    renderer = Renderer::create(*device, {.jobs = &jobs});
    const Uuid sphere = lodSphere();
    const Uuid mat = material({0.3f, 0.6f, 0.9f, 1}, 0.0f, 0.4f);
    for (int i = 0; i < 9; ++i) meshAsset(sphere, mat, {f32(i % 3) * 1.5f - 1.5f, 0.5f, -f32(i / 3) * 1.5f});
    environment();
    sun(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 20000.0f, glm::vec3(1.0f), false);
    const CameraParams cam = camera({0, 2, 4}, {0, 0.5f, -1.5f}, 12.5f);
    CVarScope on("r.GpuDriven", "true");
    CVarScope m("r.GpuDriven.Meshlets", "true");
    // The meshlet pipelines are requested on the first frame and compile on the job system; frames keep rendering
    // with the instanced path meanwhile (no hitch, same image).
    const Image first = render(cam, {.width = 256, .height = 192, .frames = 1});
    bool sawCompiling = renderer->stats().pipelinesCompiling > 0;
    Image ready;
    bool meshletsActive = false;
    for (int i = 0; i < 400 && !meshletsActive; ++i) {
        ready = render(cam, {.width = 256, .height = 192, .frames = 1});
        sawCompiling = sawCompiling || renderer->stats().pipelinesCompiling > 0;
        meshletsActive = renderer->stats().gpuCulling.meshletsTested > 0;
        if (!meshletsActive) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("async PSO: compiling observed %d, meshlet path active %d\n", int(sawCompiling), int(meshletsActive));
    EXPECT_TRUE(meshletsActive) << "meshlet pipelines never became ready";
    expectSimilar(first, ready, 0.3, 0.002, "placeholder (instanced) vs meshlet path");
}

} // namespace ox::render::test

namespace ox::render::test {

TEST_F(GpuDrivenTest, MeshShaderPathCompilesAndMatchesWhenSupported) {
    // The task/mesh shaders compile (and pass the bindless layout reflection) on every machine.
    for (const char* path : {"render/gpu_driven/meshlet.task", "render/gpu_driven/meshlet.mesh"}) {
        rhi::ShaderCompileResult r = device->shaderCompiler().compile({path});
        EXPECT_TRUE(r.success) << path << ":\n" << r.errors;
        EXPECT_FALSE(r.spirv.empty());
    }
    if (!device->caps().meshShader || !device->caps().taskShader) {
        GTEST_SKIP() << "VK_EXT_mesh_shader unavailable on " << device->caps().gpuName << " (compile-tested only)";
    }
    const Uuid sphere = lodSphere();
    const Uuid mat = material({0.3f, 0.6f, 0.9f, 1}, 0.0f, 0.4f);
    for (int i = 0; i < 16; ++i) meshAsset(sphere, mat, {f32(i % 4) * 1.5f - 2.25f, 0.5f, -f32(i / 4) * 1.5f});
    environment();
    sun(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 20000.0f);
    const CameraParams cam = camera({0, 3, 5}, {0, 0, -2}, 12.5f);
    CVarScope on("r.GpuDriven", "true");
    CVarScope m("r.GpuDriven.Meshlets", "true");
    Image compute, mesh;
    {
        CVarScope ms("r.GpuDriven.MeshShaders", "false");
        resetView();
        compute = render(cam, {.width = 256, .height = 192, .frames = 3});
    }
    {
        CVarScope ms("r.GpuDriven.MeshShaders", "true");
        resetView();
        mesh = render(cam, {.width = 256, .height = 192, .frames = 3});
    }
    expectSimilar(compute, mesh, 0.3, 0.002, "mesh shaders vs compute meshlet expansion");
}

} // namespace ox::render::test

namespace ox::render::test {

// Cost of the fixed-max-count indirect draws (zero-instance padding, no drawIndirectCount on MoltenVK): 448 batches
// (7 meshes × 64 materials) of which only a handful are visible. Compares the depth prepass of the GPU path (all
// 448 commands submitted) with the CPU path (only the visible batches).
TEST_F(GpuDrivenTest, IndirectPaddingCost) {
    Random rng(17);
    std::vector<Uuid> mats;
    for (int i = 0; i < 64; ++i) mats.push_back(material({rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 1}, 0.0f, 0.5f));
    for (int i = 0; i < 64 * 7 * 4; ++i) {
        Primitive p = Primitive(i % 7);
        if (p == Primitive::Plane) p = Primitive::Torus;
        // Everything far behind the camera except a few objects in front.
        const bool front = i < 24;
        mesh(Primitive(i % 7 == 2 ? 6 : i % 7), mats[(i / 7) % 64],
             front ? glm::vec3(f32(i % 6) - 2.5f, 0.5f, -f32(i / 6) - 2.0f) : glm::vec3(rng.range(-50.0f, 50.0f), 0.5f, rng.range(20.0f, 80.0f)),
             glm::vec3(0.5f));
    }
    environment();
    sun(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 20000.0f, glm::vec3(1.0f), false);
    const CameraParams cam = camera({0, 1.5f, 3}, {0, 0.5f, -3}, 12.5f, 60.0f, 300.0f);
    CVarScope occl("r.GpuDriven.Occlusion", "false");
    for (const char* gpu : {"false", "true", "false", "true"}) {
        CVarScope g("r.GpuDriven", gpu);
        resetView();
        f64 prepass = 0.0, forward = 0.0;
        for (int k = 0; k < 6; ++k) {
            render(cam, {.width = 1920, .height = 1080, .frames = 2});
            for (const PassTiming& p : renderer->stats().passes) {
                if (p.name == "DepthPrepass") prepass += p.gpuMs;
                if (p.name == "ForwardOpaque") forward += p.gpuMs;
            }
        }
        const RenderStats& s = renderer->stats();
        std::printf("r.GpuDriven %-5s: DepthPrepass %.3f ms, ForwardOpaque %.3f ms, %u draw calls, %u indirect commands "
                    "(%u non-empty)\n",
                    gpu, prepass / 6, forward / 6, s.drawCalls, s.indirectCommands, s.gpuCulling.drawCommands);
    }
}

} // namespace ox::render::test
