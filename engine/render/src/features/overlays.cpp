// Built-in features: "DebugLines" (core DebugDraw), "EditorOverlays" (grid + selection outline) and "DebugViews"
// (buffer visualisations, overdraw, wireframe).
#include "../renderer_impl.hpp"

#include <cstring>

namespace ox::render {

namespace {

rhi::TextureDesc texDesc(VkFormat f, Extent2D e, const char* name) {
    rhi::TextureDesc d;
    d.format = f;
    d.width = e.width;
    d.height = e.height;
    d.usage = rhi::TextureUsage::None;
    d.name = name;
    return d;
}

class DebugLinesFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "DebugLines"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Overlay); }
    i32 order() const override { return 100; }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::GraphicsPipelineDesc d;
        d.name = "render.debugLines";
        d.vertex = rhi::ShaderStageDesc::file("render/debug/lines.vert");
        d.fragment = rhi::ShaderStageDesc::file("render/debug/lines.frag");
        d.colorFormats = {formats::kSceneColorLDR};
        d.blend = {rhi::BlendState::alpha()};
        d.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        m_pipeline = ctx.device.createGraphicsPipeline(d);
        return true;
    }
    void shutdown(rhi::Device& dev) override { dev.destroy(m_pipeline); }

    void setup(FeatureContext& ctx) override {
        const RenderSnapshot& snap = ctx.snapshot();
        if (!ctx.view().desc().flags.debugDraw) return;
        if (snap.debugLines.empty() && snap.debugLinesOverlay.empty()) return;
        const VkDeviceAddress tested = ctx.upload(std::span<const DebugVertex>(snap.debugLines));
        const VkDeviceAddress overlay = ctx.upload(std::span<const DebugVertex>(snap.debugLinesOverlay));
        const u32 nTested = u32(snap.debugLines.size()), nOverlay = u32(snap.debugLinesOverlay.size());
        FrameResources& R = ctx.resources();
        const rhi::RGTexture ldr = R.texture(res::kSceneColorLDR), depth = R.texture(res::kDepth);
        const VkDeviceAddress view = ctx.viewAddress();
        const glm::vec2 depthScale{f32(ctx.renderExtent().width) / f32(ctx.outputExtent().width),
                                   f32(ctx.renderExtent().height) / f32(ctx.outputExtent().height)};
        FrameState* fs = &frameState(ctx);
        ctx.graph()
            .addPass("DebugLines")
            .read(depth, rhi::Access::SampledFragment)
            .color(ldr, VK_ATTACHMENT_LOAD_OP_LOAD)
            .execute([=, this](rhi::PassContext& p) {
                struct {
                    u64 view, vertices;
                    u32 depth, depthTest;
                    f32 scale[2];
                } pc{view, tested, p.sampledIndex(depth), 1, {depthScale.x, depthScale.y}};
                p.cmd.bindPipeline(m_pipeline);
                if (nTested) {
                    p.cmd.pushConstants(pc);
                    p.cmd.draw(nTested);
                }
                if (nOverlay) {
                    pc.vertices = overlay;
                    pc.depthTest = 0;
                    p.cmd.pushConstants(pc);
                    p.cmd.draw(nOverlay);
                }
                fs->stats->drawCalls += (nTested ? 1 : 0) + (nOverlay ? 1 : 0);
            });
    }

private:
    rhi::PipelineHandle m_pipeline;
};

class EditorOverlaysFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "EditorOverlays"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Overlay); }
    i32 order() const override { return 0; }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        m_grid = createFullscreenPipeline(dev, "render.grid", "render/editor/grid.frag", {formats::kSceneColorLDR},
                                          {rhi::BlendState::alpha()});
        m_mask = createFullscreenPipeline(dev, "render.selectionMask", "render/editor/selection_mask.frag",
                                          {formats::kSelectionMask});
        m_outline = createFullscreenPipeline(dev, "render.outline", "render/editor/outline.frag", {formats::kSceneColorLDR},
                                             {rhi::BlendState::alpha()});
        return true;
    }
    void shutdown(rhi::Device& dev) override {
        for (auto p : {m_grid, m_mask, m_outline}) dev.destroy(p);
    }

    void setup(FeatureContext& ctx) override {
        const ViewFlags& flags = ctx.view().desc().flags;
        if (!flags.editor) return;
        FrameResources& R = ctx.resources();
        rhi::RenderGraph& graph = ctx.graph();
        const rhi::RGTexture ldr = R.texture(res::kSceneColorLDR), depth = R.texture(res::kDepth);
        const VkDeviceAddress view = ctx.viewAddress();
        if (flags.grid) {
            graph.addPass("EditorGrid")
                .read(depth, rhi::Access::SampledFragment)
                .color(ldr, VK_ATTACHMENT_LOAD_OP_LOAD)
                .execute([this, view, depth](rhi::PassContext& p) {
                    struct {
                        u64 view;
                        u32 depth;
                        f32 cell, fade, opacity;
                    } pc{view, p.sampledIndex(depth), 1.0f, 150.0f, 1.0f};
                    drawFullscreen(p.cmd, m_grid, &pc, sizeof(pc));
                });
        }
        const rhi::RGTexture entity = R.texture(res::kEntityId);
        const auto& selection = ctx.snapshot().selection;
        if (flags.selectionOutline && entity.valid() && !selection.empty()) {
            std::vector<u32> sorted(selection.begin(), selection.end());
            std::sort(sorted.begin(), sorted.end());
            const VkDeviceAddress ids = ctx.upload(std::span<const u32>(sorted));
            const u32 count = u32(sorted.size());
            const rhi::RGTexture mask = graph.createTexture(texDesc(formats::kSelectionMask, ctx.renderExtent(), "SelectionMask"));
            graph.addPass("SelectionMask")
                .read(entity, rhi::Access::SampledFragment)
                .color(mask, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
                .execute([this, ids, count, entity](rhi::PassContext& p) {
                    struct {
                        u64 ids;
                        u32 count, tex;
                    } pc{ids, count, p.sampledIndex(entity)};
                    drawFullscreen(p.cmd, m_mask, &pc, sizeof(pc));
                });
            R.setTexture(res::kSelectionMask, mask);
            const Extent2D out = ctx.outputExtent();
            graph.addPass("SelectionOutline")
                .read(mask, rhi::Access::SampledFragment)
                .color(ldr, VK_ATTACHMENT_LOAD_OP_LOAD)
                .execute([this, mask, out](rhi::PassContext& p) {
                    struct {
                        f32 color[4];
                        f32 texel[2];
                        f32 thickness;
                        u32 mask;
                    } pc{{1.0f, 0.55f, 0.1f, 1.0f}, {1.0f / f32(out.width), 1.0f / f32(out.height)}, 2.0f, p.sampledIndex(mask)};
                    drawFullscreen(p.cmd, m_outline, &pc, sizeof(pc));
                });
        }
    }

private:
    rhi::PipelineHandle m_grid, m_mask, m_outline;
};

class DebugViewsFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "DebugViews"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Debug); }
    std::vector<std::string> cvarNames() const override { return {"r.DebugView", "r.Wireframe"}; }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps&) const override {
        const DebugView v = s.debugView;
        return s.wireframe || v == DebugView::Velocity || v == DebugView::Depth || v == DebugView::ShadowMask ||
               v == DebugView::Overdraw || v == DebugView::Wireframe;
    }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        m_view = createFullscreenPipeline(dev, "render.debugView", "render/debug/debug_view.frag", {formats::kSceneColorLDR});
        rhi::GraphicsPipelineDesc d;
        d.name = "render.overdraw";
        d.vertex = rhi::ShaderStageDesc::file("render/passes/mesh.vert");
        d.fragment = rhi::ShaderStageDesc::file("render/debug/overdraw.frag");
        d.colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT};
        d.blend = {rhi::BlendState::additive()};
        m_overdraw = dev.createGraphicsPipeline(d);
        if (dev.caps().fillModeNonSolid) {
            d.name = "render.wireframe";
            d.fragment = rhi::ShaderStageDesc::file("render/debug/wireframe.frag");
            d.colorFormats = {formats::kSceneColorLDR};
            d.blend = {};
            d.raster.polygonMode = VK_POLYGON_MODE_LINE;
            m_wireframe = dev.createGraphicsPipeline(d);
        }
        return true;
    }
    void shutdown(rhi::Device& dev) override {
        for (auto p : {m_view, m_overdraw, m_wireframe})
            if (p) dev.destroy(p);
    }

    void setup(FeatureContext& ctx) override {
        const RenderSettings& st = ctx.settings();
        FrameResources& R = ctx.resources();
        rhi::RenderGraph& graph = ctx.graph();
        const rhi::RGTexture ldr = R.texture(res::kSceneColorLDR);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        FrameState* fs = &frameState(ctx);
        auto visualize = [&](u32 mode, rhi::RGTexture tex, f32 scale) {
            if (!tex.valid()) return;
            graph.addPass("DebugView")
                .read(tex, rhi::Access::SampledFragment)
                .color(ldr, VK_ATTACHMENT_LOAD_OP_LOAD)
                .execute([this, view, mode, tex, scale](rhi::PassContext& p) {
                    struct {
                        u64 view;
                        u32 mode, tex;
                        f32 scale;
                    } pc{view, mode, p.sampledIndex(tex), scale};
                    drawFullscreen(p.cmd, m_view, &pc, sizeof(pc));
                });
        };
        switch (st.debugView) {
        case DebugView::Velocity: visualize(11, R.texture(res::kVelocity), 20.0f); break;
        case DebugView::Depth: visualize(12, R.texture(res::kDepth), 1.0f); break;
        case DebugView::ShadowMask: visualize(13, R.texture(res::kShadowMask), 1.0f); break;
        case DebugView::Overdraw: {
            const rhi::RGTexture od =
                graph.createTexture(texDesc(VK_FORMAT_R16G16B16A16_SFLOAT, ctx.renderExtent(), "Overdraw"));
            graph.addPass("Overdraw").color(od, VK_ATTACHMENT_LOAD_OP_CLEAR).execute([this, fs, view, scene](rhi::PassContext& p) {
                FeatureContext fc(*fs, this, InjectionPoint::Debug);
                const rhi::PipelineHandle pipes[1] = {m_overdraw};
                for (u32 b = 0; b < u32(DrawBucket::Count); ++b) {
                    const DrawList& list = fs->drawLists.buckets[b];
                    struct {
                        u64 view, scene, ids;
                        u32 inputs[6];
                    } pc{view, scene, list.instanceIds, {}};
                    fc.drawBatches(p.cmd, list, pipes, &pc, sizeof(pc));
                }
            });
            visualize(8, od, 1.0f);
            break;
        }
        default: break;
        }
        if ((st.wireframe || st.debugView == DebugView::Wireframe) && m_wireframe) {
            graph.addPass("Wireframe").color(ldr, VK_ATTACHMENT_LOAD_OP_LOAD).execute([this, fs, view, scene](rhi::PassContext& p) {
                FeatureContext fc(*fs, this, InjectionPoint::Debug);
                const rhi::PipelineHandle pipes[1] = {m_wireframe};
                for (u32 b = 0; b < u32(DrawBucket::Count); ++b) {
                    const DrawList& list = fs->drawLists.buckets[b];
                    struct {
                        u64 view, scene, ids;
                        u32 inputs[6];
                    } pc{view, scene, list.instanceIds, {}};
                    fc.drawBatches(p.cmd, list, pipes, &pc, sizeof(pc));
                }
            });
        }
    }

private:
    rhi::PipelineHandle m_view, m_overdraw, m_wireframe;
};

} // namespace

std::unique_ptr<IRenderFeature> makeDebugLinesFeature() { return std::make_unique<DebugLinesFeature>(); }
std::unique_ptr<IRenderFeature> makeEditorOverlaysFeature() { return std::make_unique<EditorOverlaysFeature>(); }
std::unique_ptr<IRenderFeature> makeDebugViewsFeature() { return std::make_unique<DebugViewsFeature>(); }

} // namespace ox::render
