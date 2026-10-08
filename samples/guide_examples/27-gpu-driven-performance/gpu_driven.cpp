// Глава 27: GPU-driven отрисовка, двухфазный Hi-Z, статистика RenderStats, стриминг мипов, параллельная запись
// (docs/guide/27-gpu-driven-performance.md). Нужен Vulkan; без него тесты пропускаются.
#include "guide_render_scene.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace ox;
using namespace ox::render;

namespace {

class GpuDriven : public guide::RenderScene {
protected:
    // Стена и 200 кубиков за ней + 10 сфер перед ней: Hi-Z должен отбросить почти всё, что за стеной.
    void wallScene() {
        const Uuid grey = material({0.7f, 0.7f, 0.7f, 1});
        const Uuid red = material({0.9f, 0.1f, 0.1f, 1});
        mesh(Primitive::Cube, grey, {0, 2, 0}, {30.0f, 4.0f, 0.5f});
        for (int i = 0; i < 200; ++i)
            mesh(Primitive::Cube, red, {f32(i % 20) - 10.0f, 0.5f + f32(i / 20) * 0.3f, -3.0f - f32(i % 5)}, glm::vec3(0.3f));
        for (int i = 0; i < 10; ++i) mesh(Primitive::Sphere, red, {f32(i) - 5.0f, 0.5f, 3.0f}, glm::vec3(0.6f));
        sunAndSky(false);
    }
};

void printTopPasses(const RenderStats& stats, usize count) {
    std::vector<PassTiming> passes = stats.passes;
    std::sort(passes.begin(), passes.end(), [](const PassTiming& a, const PassTiming& b) { return a.gpuMs > b.gpuMs; });
    for (usize i = 0; i < std::min(count, passes.size()); ++i)
        std::printf("  %-32s %.3f ms%s\n", passes[i].name.c_str(), passes[i].gpuMs, passes[i].asyncCompute ? " (async)" : "");
}

} // namespace

TEST_F(GpuDriven, OcclusionCullingAndStats) {
    wallScene();
    const CameraParams cam = camera({0, 1.5f, 10}, {0, 1.5f, 0});

    // CPU-путь (запасной): отсечение по фрустуму на CPU, по draw call на батч.
    u32 cpuDraws = 0;
    {
        guide::CVarOverride off("r.GpuDriven", "false");
        render(cam, 256, 192, 4);
        EXPECT_FALSE(renderer->stats().gpuDriven);
        cpuDraws = renderer->stats().drawCalls;
    }
    // GPU-driven (по умолчанию): отсечение и выбор LOD в compute, indirect-отрисовка.
    render(cam, 256, 192, 4);
    const RenderStats& s = renderer->stats();
    EXPECT_TRUE(s.gpuDriven);
    EXPECT_GT(s.indirectDrawCalls, 0u);
    const GpuCullingStats& g = s.gpuCulling; // читается с опозданием на latencyFrames кадров
    ASSERT_TRUE(g.valid);
    std::printf("tested %u, frustum %u, occluded %u, visible %u, draw commands %u (latency %u frames)\n",
                g.instancesTested, g.instancesFrustumCulled, g.instancesOccluded, g.instancesVisible, g.drawCommands,
                g.latencyFrames);
    EXPECT_GE(g.instancesOccluded, 150u) << "кубики за стеной отсечены Hi-Z";
    EXPECT_LE(g.instancesVisible, 40u);
    std::printf("draw calls: CPU path %u, GPU-driven %u\n", cpuDraws, s.drawCalls);

    // Всё, что показывает оверлей статистики, — в RenderStats.
    std::printf("%s", s.toString().c_str());
    printTopPasses(s, 6);
    EXPECT_GT(s.gpuFrameMs, 0.0);
    EXPECT_GT(s.renderGraphPasses, 0u);
    bool hasGpuCull = false;
    for (const PassTiming& p : s.passes) hasGpuCull = hasGpuCull || p.name.find("GpuCull") != std::string::npos;
    EXPECT_TRUE(hasGpuCull);
    u64 vram = 0;
    for (const VramCategory& c : s.vram) vram += c.bytes;
    EXPECT_GT(vram, 0u);
}

TEST_F(GpuDriven, ToggleOcclusionAtRuntime) {
    wallScene();
    const CameraParams cam = camera({0, 1.5f, 10}, {0, 1.5f, 0});
    guide::CVarOverride noOcclusion("r.GpuDriven.Occlusion", "false"); // только фрустум + дальность + LOD
    render(cam, 256, 192, 4);
    const GpuCullingStats& g = renderer->stats().gpuCulling;
    ASSERT_TRUE(g.valid);
    EXPECT_EQ(g.instancesOccluded, 0u);
    EXPECT_GE(g.instancesVisible, 200u); // всё в фрустуме рисуется, хотя закрыто стеной
}

TEST_F(GpuDriven, TextureStreamingBudget) {
    guide::CVarOverride pool("r.Streaming.PoolSizeMB", "2"); // бюджет резидентных мипов (Textures: 256…2048)
    guide::CVarOverride delay("r.Streaming.DropDelayFrames", "2");
    std::vector<Uuid> textures;
    for (int i = 0; i < 12; ++i) {
        assets::TextureData t; // 512² RGBA8 с полной цепочкой мипов
        t.format = assets::TextureFormat::RGBA8Unorm;
        t.width = t.height = 512;
        for (u32 s = 512; s >= 1; s /= 2) {
            assets::TextureMip m;
            m.width = m.height = s;
            m.data.assign(usize(s) * s * 4, std::byte(200));
            t.mips.push_back(std::move(m));
        }
        t.mipCount = u32(t.mips.size());
        const Uuid tex = Uuid::fromName(std::format("guide.streaming.tex{}", i));
        renderer->resources().addTexture(tex, t);
        assets::MaterialAsset m;
        m.albedoTexture = tex;
        const Uuid mat = Uuid::fromName(std::format("guide.streaming.mat{}", i));
        renderer->resources().addMaterial(mat, m);
        mesh(Primitive::Plane, mat, {f32(i % 2) * 2.2f - 1.1f, 0.0f, -f32(i / 2) * 6.0f}, glm::vec3(2.0f));
        textures.push_back(tex);
    }
    sunAndSky(false);
    // До первого кадра резидентен только «хвост» мипов (≤ r.Streaming.TailSize = 64 px).
    EXPECT_LE(renderer->resources().residentTexture(textures[0])->width, 64u);
    render(camera({0, 2.0f, 3.0f}, {0, 0, -10}), 640, 360, 10);
    const TextureStreamingStats& st = renderer->stats().streaming;
    std::printf("streaming: %u textures, resident %.2f MiB, wanted %.2f MiB, budget %.2f MiB\n", st.streamedTextures,
                f64(st.residentBytes) / (1 << 20), f64(st.wantedBytes) / (1 << 20), f64(st.budgetBytes) / (1 << 20));
    EXPECT_TRUE(st.enabled);
    EXPECT_LE(st.residentBytes, st.budgetBytes);
    EXPECT_GE(renderer->resources().residentTexture(textures[0])->width,
              renderer->resources().residentTexture(textures[11])->width) << "ближние текстуры детальнее дальних";
}

TEST_F(GpuDriven, JobsForExtractAndParallelRecording) {
    JobSystem jobs(4);
    createRenderer({.jobs = &jobs}); // + асинхронная загрузка ассетов и компиляция PSO на job system
    for (int i = 0; i < 300; ++i) // 300 материалов → 300 батчей ≥ r.ParallelRecording.MinBatches (256)
        mesh(Primitive::Cube, material({f32(i % 7) / 7.0f, 0.5f, f32(i % 11) / 11.0f, 1}),
             {f32(i % 20) - 10.0f, 0.5f, -f32(i / 20)}, glm::vec3(0.5f));
    sunAndSky(false);
    guide::CVarOverride cpuPath("r.GpuDriven", "false");      // параллельная запись — для CPU-пути
    guide::CVarOverride parallel("r.ParallelRecording", "On"); // Auto = выкл. на MoltenVK
    render(camera({0, 6, 12}, {0, 0, -6}), 320, 180, 3, &jobs); // extract с jobs: parallelFor от 32768 мешей
    std::printf("CPU renderer %.2f ms, secondary command lists %u, draws %u\n", renderer->stats().cpuRenderMs,
                renderer->stats().parallelRecordedChunks, renderer->stats().drawCalls);
    EXPECT_GT(renderer->stats().parallelRecordedChunks, 0u);
}
