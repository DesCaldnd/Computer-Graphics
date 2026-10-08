// Глава 17: render graph — объявление пассов, компиляция плана, барьеры, culling, aliasing, async compute
// (docs/guide/17-rhi-vulkan.md). Только CPU: compile() не трогает GPU, план можно разглядывать в тестах.
#include <oxwald/rhi/render_graph.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace ox;
using namespace ox::rhi;

namespace {

TextureDesc target(const char* name, VkFormat format, u32 w = 1280, u32 h = 720) {
    TextureDesc d;
    d.name = name;
    d.format = format;
    d.width = w;
    d.height = h;
    d.usage = TextureUsage::None; // для транзиентных ресурсов граф сам выведет usage из объявлений
    return d;
}

const RGBarrier* findBarrier(const std::vector<RGBarrier>& list, u32 resource) {
    auto it = std::find_if(list.begin(), list.end(), [&](const RGBarrier& b) { return b.resource == resource; });
    return it == list.end() ? nullptr : &*it;
}

// Типичный кадр: depth prepass -> SSAO (compute) -> освещение -> tonemap в backbuffer.
struct Frame {
    RGTexture backbuffer, depth, ao, hdr, debug;
};

Frame declareFrame(RenderGraph& graph) {
    Frame f;
    // Внешний ресурс (в реальном коде — swapchain->currentTexture()). Здесь handle фиктивный: compile() его не трогает.
    f.backbuffer = graph.importTexture(TextureHandle{1, 1}, target("Backbuffer", VK_FORMAT_B8G8R8A8_SRGB),
                                       {Access::Undefined, Access::Present});
    // Транзиентные ресурсы: живут один кадр, память выделяет и переиспользует сам граф.
    f.depth = graph.createTexture(target("Depth", VK_FORMAT_D32_SFLOAT));
    f.ao = graph.createTexture(target("AO", VK_FORMAT_R8_UNORM));
    f.hdr = graph.createTexture(target("HDR", VK_FORMAT_R16G16B16A16_SFLOAT));
    f.debug = graph.createTexture(target("DebugOverlay", VK_FORMAT_R8G8B8A8_UNORM));

    graph.addPass("DepthPrepass").depth(f.depth); // CLEAR, reversed-Z: очистка в 0
    graph.addPass("SSAO", PassType::Compute)
        .queue(QueueType::Compute) // подсказка: async compute, если включён
        .read(f.depth, Access::SampledCompute)
        .overwrite(f.ao, Access::StorageWriteCompute);
    graph.addPass("Lighting")
        .read(f.ao, Access::SampledFragment)
        .depth(f.depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, /*readOnly*/ true)
        .color(f.hdr);
    graph.addPass("Debug").color(f.debug); // результат никто не читает -> пасс будет отброшен
    graph.addPass("Tonemap").read(f.hdr, Access::SampledFragment).color(f.backbuffer);
    // execute(...)-лямбды здесь не нужны: на CPU мы только смотрим на план.
    return f;
}

} // namespace

TEST(GuideRhiRenderGraph, CullingOrderAndBarriers) {
    RenderGraph graph;
    Frame f = declareFrame(graph);
    const RenderGraphPlan& plan = graph.compile();

    // "Debug" (объявлен 4-м, индекс 3) отброшен: его результат не нужен ни одному выходу.
    ASSERT_EQ(plan.culledPasses, std::vector<u32>{3});
    ASSERT_EQ(plan.passes.size(), 4u);
    EXPECT_EQ(graph.passName(plan.passes[3].pass), "Tonemap");
    EXPECT_FALSE(plan.resources[f.debug.id].used);

    // Перед SSAO: depth из DEPTH_ATTACHMENT в SHADER_READ_ONLY (запись -> чтение).
    const RGBarrier* depthToSampled = findBarrier(plan.passes[1].before, f.depth.id);
    ASSERT_NE(depthToSampled, nullptr);
    EXPECT_EQ(depthToSampled->oldLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(depthToSampled->newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Перед Lighting: AO из GENERAL (storage-запись в compute) в SHADER_READ_ONLY для фрагментного шейдера.
    const RGBarrier* aoToSampled = findBarrier(plan.passes[2].before, f.ao.id);
    ASSERT_NE(aoToSampled, nullptr);
    EXPECT_EQ(aoToSampled->oldLayout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(aoToSampled->srcStages, VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
    EXPECT_EQ(aoToSampled->dstStages, VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT));

    // После последнего использования импортированный backbuffer переводится в PRESENT.
    const RGBarrier* present = findBarrier(plan.passes[3].after, f.backbuffer.id);
    ASSERT_NE(present, nullptr);
    EXPECT_EQ(present->kind, RGBarrierKind::Final);
    EXPECT_EQ(present->newLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // Usage транзиентов выведен из объявлений: AO — storage + sampled.
    EXPECT_TRUE(any(plan.resources[f.ao.id].derivedTextureUsage & TextureUsage::Storage));
    EXPECT_TRUE(any(plan.resources[f.ao.id].derivedTextureUsage & TextureUsage::Sampled));

    // Текстовый дамп и Graphviz — для отладки (dot -Tsvg frame.dot > frame.svg).
    EXPECT_NE(plan.dump(graph).find("culled: Debug"), std::string::npos);
    EXPECT_NE(graph.exportGraphviz().find("digraph RenderGraph"), std::string::npos);
}

TEST(GuideRhiRenderGraph, AliasingOfTransientMemory) {
    RenderGraph graph;
    RGTexture out = graph.importTexture(TextureHandle{1, 1}, target("Out", VK_FORMAT_R8G8B8A8_UNORM),
                                        {Access::Undefined, Access::SampledFragment});
    // Цепочка блюров: A живёт в пассах 0-1, B — 1-2, C — 2-3. A и C не пересекаются по времени.
    RGTexture a = graph.createTexture(target("BlurA", VK_FORMAT_R16G16B16A16_SFLOAT));
    RGTexture b = graph.createTexture(target("BlurB", VK_FORMAT_R16G16B16A16_SFLOAT));
    RGTexture c = graph.createTexture(target("BlurC", VK_FORMAT_R16G16B16A16_SFLOAT));
    graph.addPass("P0").color(a);
    graph.addPass("P1").read(a, Access::SampledFragment).color(b);
    graph.addPass("P2").read(b, Access::SampledFragment).color(c);
    graph.addPass("P3").read(c, Access::SampledFragment).color(out);

    const RenderGraphPlan& plan = graph.compile();
    EXPECT_EQ(plan.resources[a.id].aliasSlot, plan.resources[c.id].aliasSlot) << "C занимает память A";
    EXPECT_EQ(plan.aliasSlots.size(), 2u);
    EXPECT_LT(plan.transientBytesAliased, plan.transientBytesUnaliased);

    // Первое использование C — aliasing-барьер: ждёт последнего читателя A, содержимое не сохраняется.
    const RGBarrier* alias = findBarrier(plan.passes[2].before, c.id);
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(alias->kind, RGBarrierKind::Aliasing);
    EXPECT_EQ(alias->oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);

    // Отключить aliasing (например, чтобы проверить, не в нём ли баг).
    RGCompileOptions noAlias;
    noAlias.aliasing = false;
    EXPECT_EQ(graph.compile(noAlias).aliasSlots.size(), 3u);
}

TEST(GuideRhiRenderGraph, AsyncComputeBatches) {
    RenderGraph graph;
    Frame f = declareFrame(graph);

    // Так граф компилируется на устройстве с отдельной compute-очередью (семейства 0/1/2, как на MoltenVK).
    RGCompileOptions options;
    options.asyncCompute = true;
    options.queueFamilies = {0, 1, 2}; // Graphics, Compute, Transfer
    const RenderGraphPlan& plan = graph.compile(options);

    // Graphics (DepthPrepass) -> Compute (SSAO) -> Graphics (Lighting, Tonemap): три пачки с ожиданиями по timeline.
    ASSERT_EQ(plan.batches.size(), 3u);
    EXPECT_EQ(plan.batches[1].queue, QueueType::Compute);
    EXPECT_EQ(plan.batches[1].waitBatches, std::vector<u32>{0});
    EXPECT_EQ(plan.batches[2].waitBatches, std::vector<u32>{1});

    // Передача владения depth между семействами: release после DepthPrepass, acquire перед SSAO.
    const RGBarrier* release = findBarrier(plan.passes[0].after, f.depth.id);
    ASSERT_NE(release, nullptr);
    EXPECT_EQ(release->kind, RGBarrierKind::Release);
    EXPECT_EQ(release->dstFamily, 1u);
    const RGBarrier* acquire = findBarrier(plan.passes[1].before, f.depth.id);
    ASSERT_NE(acquire, nullptr);
    EXPECT_EQ(acquire->kind, RGBarrierKind::Acquire);

    // Ресурсы, которые ходят между очередями, никогда не алиасятся.
    EXPECT_TRUE(plan.resources[f.ao.id].asyncQueue);
}

TEST(GuideRhiRenderGraph, PlanIsCachedAcrossFrames) {
    RenderGraph graph; // живёт между кадрами
    for (int frame = 0; frame < 3; ++frame) {
        graph.reset(); // забыть объявления, но сохранить кэш плана
        declareFrame(graph);
        graph.compile();
    }
    EXPECT_EQ(graph.compileCount(), 1u) << "одинаковая топология -> план из кэша";
}
