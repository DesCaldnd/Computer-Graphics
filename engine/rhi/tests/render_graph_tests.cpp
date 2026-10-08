// CPU-only tests of the render graph planner: culling, ordering, barriers, queue batches, aliasing.
#include <oxwald/rhi/render_graph.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace ox;
using namespace ox::rhi;

namespace {

TextureDesc tex(const char* name, u32 w = 256, u32 h = 256, VkFormat f = VK_FORMAT_R16G16B16A16_SFLOAT) {
    TextureDesc d;
    d.name = name;
    d.width = w;
    d.height = h;
    d.format = f;
    d.usage = TextureUsage::None;
    return d;
}

const RGBarrier* findBarrier(const std::vector<RGBarrier>& list, u32 resource) {
    for (const auto& b : list) {
        if (b.resource == resource) return &b;
    }
    return nullptr;
}

bool culled(const RenderGraphPlan& plan, u32 pass) {
    return std::find(plan.culledPasses.begin(), plan.culledPasses.end(), pass) != plan.culledPasses.end();
}

} // namespace

TEST(RenderGraphPlan, CullsPassesThatDoNotContributeToOutputs) {
    RenderGraph g;
    TextureDesc bbDesc = tex("Backbuffer", 256, 256, VK_FORMAT_B8G8R8A8_SRGB);
    RGTexture backbuffer = g.importTexture(TextureHandle{1, 1}, bbDesc, {Access::Undefined, Access::Present});
    RGTexture scene = g.createTexture(tex("Scene"));
    RGTexture unused = g.createTexture(tex("DebugOverlay"));

    g.addPass("Scene", PassType::Graphics).color(scene);                                   // 0
    g.addPass("Debug", PassType::Graphics).color(unused);                                  // 1: result never read
    g.addPass("Tonemap", PassType::Graphics).read(scene, Access::SampledFragment).color(backbuffer); // 2
    g.addPass("Readback", PassType::Transfer).read(scene, Access::TransferRead).sideEffect();        // 3

    const RenderGraphPlan& plan = g.compile();
    EXPECT_TRUE(culled(plan, 1));
    EXPECT_FALSE(culled(plan, 0));
    EXPECT_FALSE(culled(plan, 2));
    EXPECT_FALSE(culled(plan, 3)) << "side-effect passes are never culled";
    ASSERT_EQ(plan.passes.size(), 3u);
    EXPECT_EQ(plan.passes[0].pass, 0u);
    EXPECT_EQ(plan.passes[1].pass, 2u);
    EXPECT_EQ(plan.passes[2].pass, 3u);
    EXPECT_FALSE(plan.resources[unused.id].used);

    RGCompileOptions noCull;
    noCull.cull = false;
    EXPECT_EQ(g.compile(noCull).passes.size(), 4u);
}

TEST(RenderGraphPlan, BarriersAndLayoutTransitions) {
    RenderGraph g;
    RGTexture backbuffer = g.importTexture(TextureHandle{1, 1}, tex("Backbuffer"), {Access::Undefined, Access::Present});
    RGTexture color = g.createTexture(tex("Color"));
    RGTexture depth = g.createTexture(tex("Depth", 256, 256, VK_FORMAT_D32_SFLOAT));

    g.addPass("GBuffer").color(color).depth(depth);
    g.addPass("Blur", PassType::Compute).write(color, Access::StorageWriteCompute);
    g.addPass("Compose").read(color, Access::SampledFragment).read(depth, Access::DepthStencilRead).color(backbuffer);

    const RenderGraphPlan& plan = g.compile();
    ASSERT_EQ(plan.passes.size(), 3u);
    EXPECT_TRUE(plan.culledPasses.empty());

    // Pass 0: first use of transients -> UNDEFINED -> attachment layouts.
    const RGBarrier* c0 = findBarrier(plan.passes[0].before, color.id);
    ASSERT_NE(c0, nullptr);
    EXPECT_EQ(c0->oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(c0->newLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const RGBarrier* d0 = findBarrier(plan.passes[0].before, depth.id);
    ASSERT_NE(d0, nullptr);
    EXPECT_EQ(d0->newLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    // Pass 1: color attachment write -> storage write (RAW+WAW, layout change to GENERAL).
    const RGBarrier* c1 = findBarrier(plan.passes[1].before, color.id);
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(c1->oldLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(c1->newLayout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(c1->srcStages, VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT));
    EXPECT_EQ(c1->srcAccessMask, VkAccessFlags2(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
    EXPECT_EQ(c1->dstStages, VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));

    // Pass 2: storage write -> sampled read; depth write -> read-only depth; backbuffer discard.
    const RGBarrier* c2 = findBarrier(plan.passes[2].before, color.id);
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(c2->oldLayout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(c2->newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(c2->srcAccessMask, VkAccessFlags2(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT));
    const RGBarrier* d2 = findBarrier(plan.passes[2].before, depth.id);
    ASSERT_NE(d2, nullptr);
    EXPECT_EQ(d2->newLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    const RGBarrier* bb = findBarrier(plan.passes[2].before, backbuffer.id);
    ASSERT_NE(bb, nullptr);
    EXPECT_EQ(bb->oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);

    // Final transition of the imported backbuffer to PRESENT after its last use.
    const RGBarrier* fin = findBarrier(plan.passes[2].after, backbuffer.id);
    ASSERT_NE(fin, nullptr);
    EXPECT_EQ(fin->kind, RGBarrierKind::Final);
    EXPECT_EQ(fin->newLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    EXPECT_EQ(plan.resources[backbuffer.id].finalAccess, Access::Present);

    // Derived usage of transients.
    EXPECT_TRUE(any(plan.resources[color.id].derivedTextureUsage & TextureUsage::Storage));
    EXPECT_TRUE(any(plan.resources[color.id].derivedTextureUsage & TextureUsage::Sampled));
    EXPECT_TRUE(any(plan.resources[color.id].derivedTextureUsage & TextureUsage::ColorAttachment));
}

TEST(RenderGraphPlan, ReadAfterReadNeedsNoBarrierWhenAlreadyVisible) {
    RenderGraph g;
    RGTexture out = g.importTexture(TextureHandle{1, 1}, tex("Out"), {Access::Undefined, Access::Undefined});
    RGTexture src = g.createTexture(tex("Src"));
    g.addPass("Produce").color(src);
    g.addPass("ReadA").read(src, Access::SampledFragment).color(out, VK_ATTACHMENT_LOAD_OP_CLEAR);
    g.addPass("ReadB").read(src, Access::SampledFragment).color(out, VK_ATTACHMENT_LOAD_OP_LOAD);
    const RenderGraphPlan& plan = g.compile();
    ASSERT_EQ(plan.passes.size(), 3u);
    EXPECT_NE(findBarrier(plan.passes[1].before, src.id), nullptr);
    EXPECT_EQ(findBarrier(plan.passes[2].before, src.id), nullptr) << "second sampled read in the same stage is already visible";
    // `out` written twice by color attachments in the same layout still needs a WAW barrier.
    const RGBarrier* waw = findBarrier(plan.passes[2].before, out.id);
    ASSERT_NE(waw, nullptr);
    EXPECT_EQ(waw->oldLayout, waw->newLayout);
}

TEST(RenderGraphPlan, WriteAfterReadWaitsForReaders) {
    RenderGraph g;
    RGBuffer buf = g.importBuffer(BufferHandle{1, 1}, {1024, BufferUsage::Storage, MemoryUsage::GpuOnly, "Buf"},
                                  {Access::Undefined, Access::Undefined});
    g.addPass("Write1", PassType::Compute).overwrite(buf, Access::StorageWriteCompute);
    g.addPass("Read", PassType::Graphics).read(buf, Access::StorageReadGraphics).sideEffect();
    g.addPass("Write2", PassType::Compute).overwrite(buf, Access::StorageWriteCompute);
    const RenderGraphPlan& plan = g.compile();
    ASSERT_EQ(plan.passes.size(), 3u);
    const RGBarrier* war = findBarrier(plan.passes[2].before, buf.id);
    ASSERT_NE(war, nullptr);
    EXPECT_TRUE(war->srcStages & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) << "WAR waits on the reader's stages";
    EXPECT_TRUE(war->srcStages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) << "WAW waits on the previous writer";
    EXPECT_EQ(war->srcAccessMask & ~VkAccessFlags2(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT), 0u);
    EXPECT_FALSE(war->texture);
}

TEST(RenderGraphPlan, AliasesTransientsWithDisjointLifetimes) {
    RenderGraph g;
    RGTexture out = g.importTexture(TextureHandle{1, 1}, tex("Out"), {Access::Undefined, Access::SampledFragment});
    RGTexture a = g.createTexture(tex("A"));
    RGTexture b = g.createTexture(tex("B"));
    RGTexture c = g.createTexture(tex("C"));
    g.addPass("P0").color(a);                                              // A: [0,1]
    g.addPass("P1").read(a, Access::SampledFragment).color(b);            // B: [1,2]
    g.addPass("P2").read(b, Access::SampledFragment).color(c);            // C: [2,3]
    g.addPass("P3").read(c, Access::SampledFragment).color(out);

    const RenderGraphPlan& plan = g.compile();
    ASSERT_EQ(plan.passes.size(), 4u);
    EXPECT_EQ(plan.resources[a.id].firstPass, 0u);
    EXPECT_EQ(plan.resources[a.id].lastPass, 1u);
    EXPECT_EQ(plan.resources[c.id].firstPass, 2u);
    EXPECT_EQ(plan.resources[a.id].aliasSlot, plan.resources[c.id].aliasSlot) << "A and C never live at the same time";
    EXPECT_NE(plan.resources[a.id].aliasSlot, plan.resources[b.id].aliasSlot);
    EXPECT_EQ(plan.aliasSlots.size(), 2u);
    EXPECT_LT(plan.transientBytesAliased, plan.transientBytesUnaliased);

    // C's first use is an aliasing barrier that waits for A's last reader and discards contents.
    const RGBarrier* alias = findBarrier(plan.passes[2].before, c.id);
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(alias->kind, RGBarrierKind::Aliasing);
    EXPECT_EQ(alias->oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_TRUE(alias->srcStages & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

    RGCompileOptions noAlias;
    noAlias.aliasing = false;
    const RenderGraphPlan& p2 = g.compile(noAlias);
    EXPECT_EQ(p2.aliasSlots.size(), 3u);
    EXPECT_EQ(p2.transientBytesAliased, p2.transientBytesUnaliased);
}

TEST(RenderGraphPlan, AliasingRespectsMemoryTypes) {
    RenderGraph g;
    RGTexture out = g.importTexture(TextureHandle{1, 1}, tex("Out"), {Access::Undefined, Access::Undefined});
    RGTexture a = g.createTexture(tex("A"));
    RGTexture c = g.createTexture(tex("C", 256, 256, VK_FORMAT_D32_SFLOAT));
    g.addPass("P0").color(a);
    g.addPass("P1").read(a, Access::SampledFragment).color(out);
    g.addPass("P2").depth(c).color(out, VK_ATTACHMENT_LOAD_OP_LOAD);
    RGCompileOptions o;
    o.textureRequirements = [](const TextureDesc& d) {
        // Pretend depth formats live in a different memory type.
        return MemoryRequirements{1 << 20, 4096, d.format == VK_FORMAT_D32_SFLOAT ? 0x2u : 0x1u};
    };
    const RenderGraphPlan& plan = g.compile(o);
    EXPECT_NE(plan.resources[a.id].aliasSlot, plan.resources[c.id].aliasSlot);
}

TEST(RenderGraphPlan, AsyncComputeBatchesAndQueueOwnershipTransfer) {
    RenderGraph g;
    RGTexture out = g.importTexture(TextureHandle{1, 1}, tex("Out"), {Access::Undefined, Access::Present});
    RGTexture depth = g.createTexture(tex("Depth", 256, 256, VK_FORMAT_D32_SFLOAT));
    RGTexture ao = g.createTexture(tex("AO", 256, 256, VK_FORMAT_R8_UNORM));
    g.addPass("DepthPrepass").depth(depth);                                                           // graphics
    g.addPass("SSAO", PassType::Compute).queue(QueueType::Compute)
        .read(depth, Access::SampledCompute).overwrite(ao, Access::StorageWriteCompute);              // async compute
    g.addPass("Lighting").read(ao, Access::SampledFragment).depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true)
        .color(out);                                                                                  // graphics

    RGCompileOptions o;
    o.asyncCompute = true;
    o.queueFamilies = {0, 1, 2};
    const RenderGraphPlan& plan = g.compile(o);
    ASSERT_EQ(plan.passes.size(), 3u);
    ASSERT_EQ(plan.batches.size(), 3u);
    EXPECT_EQ(plan.batches[0].queue, QueueType::Graphics);
    EXPECT_EQ(plan.batches[1].queue, QueueType::Compute);
    EXPECT_EQ(plan.batches[2].queue, QueueType::Graphics);
    EXPECT_EQ(plan.batches[1].waitBatches, std::vector<u32>{0});
    EXPECT_EQ(plan.batches[2].waitBatches, std::vector<u32>{1});

    // depth: graphics(family 0) -> compute(family 1): release after DepthPrepass, acquire before SSAO.
    const RGBarrier* rel = findBarrier(plan.passes[0].after, depth.id);
    ASSERT_NE(rel, nullptr);
    EXPECT_EQ(rel->kind, RGBarrierKind::Release);
    EXPECT_EQ(rel->srcFamily, 0u);
    EXPECT_EQ(rel->dstFamily, 1u);
    EXPECT_EQ(rel->newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const RGBarrier* acq = findBarrier(plan.passes[1].before, depth.id);
    ASSERT_NE(acq, nullptr);
    EXPECT_EQ(acq->kind, RGBarrierKind::Acquire);
    EXPECT_EQ(acq->oldLayout, rel->oldLayout);
    EXPECT_EQ(acq->newLayout, rel->newLayout);

    // ao: compute -> graphics; the transient is never aliased because it crosses queues.
    EXPECT_TRUE(plan.resources[ao.id].asyncQueue);
    const RGBarrier* aoRel = findBarrier(plan.passes[1].after, ao.id);
    ASSERT_NE(aoRel, nullptr);
    EXPECT_EQ(aoRel->kind, RGBarrierKind::Release);
    EXPECT_EQ(aoRel->srcFamily, 1u);
    EXPECT_EQ(aoRel->dstFamily, 0u);

    // Without async compute everything collapses into one graphics batch, no ownership transfers.
    const RenderGraphPlan& single = g.compile();
    ASSERT_EQ(single.batches.size(), 1u);
    for (const auto& p : single.passes) {
        for (const auto& b : p.before) EXPECT_EQ(b.srcFamily, VK_QUEUE_FAMILY_IGNORED);
        EXPECT_EQ(p.queue, QueueType::Graphics);
    }
}

// A dedicated compute family (NVIDIA: compute | transfer only) cannot execute graphics stages, so no barrier recorded
// on the compute queue may name them — also not the cross-frame ordering against last frame's graphics readers.
TEST(RenderGraphPlan, AsyncComputeBarriersNameNoGraphicsStages) {
    RenderGraph g;
    RGTexture out = g.importTexture(TextureHandle{1, 1}, tex("Out"), {Access::Undefined, Access::Present});
    RGTexture depth = g.createTexture(tex("Depth", 256, 256, VK_FORMAT_D32_SFLOAT));
    RGTexture hiz = g.createTexture(tex("HiZ", 256, 256, VK_FORMAT_R32_SFLOAT));
    RGBuffer particles = g.createBuffer({4096, BufferUsage::Storage, MemoryUsage::GpuOnly, "Particles"});
    g.addPass("Simulate", PassType::Compute).queue(QueueType::Compute).overwrite(particles, Access::StorageWriteCompute);
    g.addPass("DepthPrepass").depth(depth);
    g.addPass("HiZ", PassType::Compute).queue(QueueType::Compute)
        .read(depth, Access::SampledCompute).overwrite(hiz, Access::StorageWriteCompute);
    g.addPass("Lighting").read(particles, Access::StorageReadGraphics).read(hiz, Access::SampledFragment)
        .depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true).color(out);
    RGCompileOptions o;
    o.asyncCompute = true;
    o.queueFamilies = {0, 1, 2};
    const RenderGraphPlan& plan = g.compile(o);
    constexpr VkPipelineStageFlags2 kGraphicsStages =
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    u32 computeBarriers = 0;
    for (const auto& p : plan.passes) {
        if (p.queue != QueueType::Compute) continue;
        for (const auto* list : {&p.before, &p.after}) {
            for (const RGBarrier& b : *list) {
                ++computeBarriers;
                EXPECT_EQ(b.srcStages & kGraphicsStages, 0u) << g.passName(p.pass) << " / " << g.resourceName(b.resource);
                EXPECT_EQ(b.dstStages & kGraphicsStages, 0u) << g.passName(p.pass) << " / " << g.resourceName(b.resource);
            }
        }
    }
    EXPECT_GE(computeBarriers, 4u);
}

// Graphics passes that do not depend on the async batch stay in their own batch (no wait), so they overlap with it;
// the batch is split before the first pass that touches a resource the compute queue used (even read after read:
// the resource changes its owning queue family).
TEST(RenderGraphPlan, IndependentGraphicsPassesDoNotWaitForAsyncCompute) {
    RenderGraph g;
    RGTexture out = g.importTexture(TextureHandle{1, 1}, tex("Out"), {Access::Undefined, Access::Present});
    RGTexture depth = g.createTexture(tex("Depth", 256, 256, VK_FORMAT_D32_SFLOAT));
    RGTexture hiz = g.createTexture(tex("HiZ", 256, 256, VK_FORMAT_R32_SFLOAT));
    RGTexture shadow = g.createTexture(tex("Shadow", 256, 256, VK_FORMAT_D32_SFLOAT));
    RGTexture mask = g.createTexture(tex("ShadowMask", 256, 256, VK_FORMAT_R8_UNORM));
    g.addPass("DepthPrepass").depth(depth);
    g.addPass("HiZ", PassType::Compute).queue(QueueType::Compute)
        .read(depth, Access::SampledCompute).overwrite(hiz, Access::StorageWriteCompute);
    g.addPass("Shadows").depth(shadow);
    g.addPass("ShadowMask").read(depth, Access::SampledFragment).read(shadow, Access::SampledFragment).color(mask);
    g.addPass("Lighting").read(hiz, Access::SampledFragment).read(mask, Access::SampledFragment).color(out);
    RGCompileOptions o;
    o.asyncCompute = true;
    o.queueFamilies = {0, 1, 2};
    const RenderGraphPlan& plan = g.compile(o);
    ASSERT_EQ(plan.batches.size(), 4u);
    EXPECT_EQ(plan.batches[1].queue, QueueType::Compute);
    ASSERT_EQ(plan.batches[2].passes.size(), 1u) << "only Shadows: ShadowMask reads the depth HiZ took over";
    EXPECT_TRUE(plan.batches[2].waitBatches.empty()) << "Shadows overlaps with the async batch";
    EXPECT_EQ(plan.batches[3].waitBatches, std::vector<u32>{1});
}

TEST(RenderGraphPlan, SameFamilyQueuesUseSemaphoresOnly) {
    RenderGraph g;
    RGBuffer buf = g.importBuffer(BufferHandle{1, 1}, {4096, BufferUsage::Storage, MemoryUsage::GpuOnly, "Particles"},
                                  {Access::Undefined, Access::Undefined});
    g.addPass("Simulate", PassType::Compute).queue(QueueType::Compute).overwrite(buf, Access::StorageWriteCompute);
    g.addPass("Draw").read(buf, Access::StorageReadGraphics).sideEffect();
    RGCompileOptions o;
    o.asyncCompute = true;
    o.queueFamilies = {0, 0, 0}; // e.g. MoltenVK: one family, several queues
    const RenderGraphPlan& plan = g.compile(o);
    ASSERT_EQ(plan.batches.size(), 2u);
    EXPECT_EQ(plan.batches[1].waitBatches, std::vector<u32>{0});
    EXPECT_TRUE(plan.passes[0].after.empty()) << "no release barrier within one queue family";
    for (const auto& b : plan.passes[1].before) {
        EXPECT_NE(b.kind, RGBarrierKind::Acquire);
    }
}

TEST(RenderGraphPlan, CompileIsCachedUntilTopologyChanges) {
    RenderGraph g;
    auto build = [&](u32 width) {
        g.reset();
        RGTexture out = g.importTexture(TextureHandle{1, 1}, tex("Out"), {Access::Undefined, Access::Present});
        RGTexture t = g.createTexture(tex("Scene", width, width));
        g.addPass("Scene").color(t);
        g.addPass("Post").read(t, Access::SampledFragment).color(out);
        return g.compile();
    };
    build(128);
    EXPECT_EQ(g.compileCount(), 1u);
    const u64 h = g.plan().hash;
    build(128);
    EXPECT_EQ(g.compileCount(), 1u) << "identical frame reuses the cached plan";
    build(256);
    EXPECT_EQ(g.compileCount(), 2u) << "resolution change rebuilds";
    EXPECT_NE(g.plan().hash, h);
}

TEST(RenderGraphPlan, WarnsAboutReadingUninitializedTransient) {
    RenderGraph g;
    RGTexture t = g.createTexture(tex("Garbage"));
    g.addPass("Reader").read(t, Access::SampledFragment).sideEffect();
    const RenderGraphPlan& plan = g.compile();
    ASSERT_FALSE(plan.warnings.empty());
}

TEST(RenderGraphPlan, GraphvizAndDump) {
    RenderGraph g;
    RGTexture out = g.importTexture(TextureHandle{1, 1}, tex("Backbuffer"), {Access::Undefined, Access::Present});
    RGTexture t = g.createTexture(tex("Scene"));
    g.addPass("Scene").color(t);
    g.addPass("Unused").color(g.createTexture(tex("Nope")));
    g.addPass("Post").read(t, Access::SampledFragment).color(out);
    g.compile();
    const std::string dot = g.exportGraphviz();
    EXPECT_NE(dot.find("digraph RenderGraph"), std::string::npos);
    EXPECT_NE(dot.find("Scene"), std::string::npos);
    EXPECT_NE(dot.find("dashed"), std::string::npos) << "culled pass is drawn dashed";
    const std::string dump = g.plan().dump(g);
    EXPECT_NE(dump.find("culled: Unused"), std::string::npos);
    EXPECT_NE(dump.find("final"), std::string::npos);
}

namespace {
// Every access bit in a barrier must be legal for its stage mask (VUID-VkImageMemoryBarrier2-srcAccessMask-*).
void expectAccessMatchesStages(const RenderGraphPlan& plan) {
    auto check = [](VkAccessFlags2 access, VkPipelineStageFlags2 stages) {
        constexpr VkPipelineStageFlags2 kShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        if (access & (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT))
            EXPECT_TRUE(stages & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        if (access & (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT))
            EXPECT_TRUE(stages & (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT));
        if (access & (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT))
            EXPECT_TRUE(stages & kShaders);
        if (access & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT))
            EXPECT_TRUE(stages & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    };
    for (const auto& p : plan.passes) {
        for (const auto* list : {&p.before, &p.after}) {
            for (const RGBarrier& b : *list) {
                check(b.srcAccessMask, b.srcStages);
                check(b.dstAccessMask, b.dstStages);
            }
        }
    }
}
} // namespace

TEST(RenderGraphPlan, BarrierAccessMasksMatchStageMasks) {
    RenderGraph g;
    RGTexture out = g.importTexture(TextureHandle{1, 1}, tex("Out"), {Access::SampledFragment, Access::Present});
    RGTexture a = g.createTexture(tex("A"));
    RGTexture b = g.createTexture(tex("B"));
    RGTexture c = g.createTexture(tex("C"));
    RGTexture d = g.createTexture(tex("D", 256, 256, VK_FORMAT_D32_SFLOAT));
    g.addPass("P0").color(a).depth(d);
    g.addPass("P1").read(a, Access::SampledFragment).read(d, Access::SampledFragment).color(b);
    g.addPass("P2", PassType::Compute).read(a, Access::SampledCompute).read(b, Access::SampledCompute)
        .overwrite(c, Access::StorageWriteCompute);
    g.addPass("P3", PassType::Transfer).read(c, Access::TransferRead).overwrite(out, Access::TransferWrite);
    g.addPass("P4").read(c, Access::SampledFragment).color(out, VK_ATTACHMENT_LOAD_OP_LOAD);
    expectAccessMatchesStages(g.compile());
    RGCompileOptions o;
    o.asyncCompute = true;
    o.queueFamilies = {0, 1, 2};
    expectAccessMatchesStages(g.compile(o));
}
