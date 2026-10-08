#include <oxwald/ui/ui_render_feature.hpp>

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/render_graph.hpp>

#include <algorithm>

namespace ox::ui {

namespace {

// Mirrors the push block in engine/shaders/ui/ui_common.glsl (scalar layout).
struct UiPush {
    u64 vertices = 0;
    u64 indices = 0;
    u64 transforms = 0;
    f32 xform[4]{1, 1, 0, 0};
    f32 ndcScale[2]{0, 0};
    u32 firstIndex = 0;
    i32 vertexOffset = 0;
    i32 transform = -1;
    u32 texture = 0;
    u32 flags = 0;
    u32 pad = 0;
};
static_assert(sizeof(UiPush) == 72 && sizeof(UiPush) <= rhi::kMaxPushConstantSize);

struct ResolvedCmd {
    UiPush pc;
    u32 indexCount = 0;
    i32 x = 0, y = 0;
    u32 w = 0, h = 0;
};

RenderGraphInfo captureGraph(const rhi::RenderGraph& graph, const render::RenderStats& stats, const std::string& view,
                             u64 frame) {
    RenderGraphInfo info;
    info.view = view;
    info.frame = frame;
    const rhi::RenderGraphPlan& plan = graph.plan();
    const std::string prefix = view + "/";
    auto timing = [&](const std::string& pass) {
        for (const render::PassTiming& t : stats.passes)
            if (t.name == pass || (t.name.size() == prefix.size() + pass.size() && t.name.starts_with(prefix) && t.name.ends_with(pass)))
                return t.gpuMs;
        return -1.0;
    };
    for (const rhi::RGPlannedPass& p : plan.passes) {
        if (p.pass >= graph.passCount()) continue;
        RenderGraphPassInfo pi;
        pi.name = graph.passName(p.pass);
        pi.queue = rhi::queueTypeName(p.queue);
        pi.batch = p.batch;
        pi.barriers = u32(p.before.size() + p.after.size());
        pi.gpuMs = timing(pi.name);
        info.passes.push_back(std::move(pi));
    }
    for (u32 c : plan.culledPasses) {
        if (c >= graph.passCount()) continue;
        RenderGraphPassInfo pi;
        pi.name = graph.passName(c);
        pi.culled = true;
        info.passes.push_back(std::move(pi));
    }
    const u32 resources = std::min<u32>(u32(plan.resources.size()), graph.resourceCount());
    for (u32 r = 0; r < resources; ++r) {
        const rhi::RGResourcePlan& rp = plan.resources[r];
        RenderGraphResourceInfo ri;
        ri.name = graph.resourceName(r);
        ri.texture = rp.texture;
        ri.imported = rp.imported;
        ri.used = rp.used;
        ri.aliasSlot = rp.aliasSlot;
        ri.size = rp.size;
        ri.firstPass = rp.firstPass;
        ri.lastPass = rp.lastPass;
        info.resources.push_back(std::move(ri));
    }
    info.batches = u32(plan.batches.size());
    info.transientBytesUnaliased = plan.transientBytesUnaliased;
    info.transientBytesAliased = plan.transientBytesAliased;
    info.graphviz = graph.exportGraphviz();
    return info;
}

} // namespace

UiOverlayFeature::UiOverlayFeature(std::shared_ptr<UiRenderBridge> bridge) : m_bridge(std::move(bridge)) {}

bool UiOverlayFeature::initialize(render::FeatureInitContext& ctx) {
    rhi::GraphicsPipelineDesc d;
    d.name = "ui.overlay";
    d.vertex = rhi::ShaderStageDesc::file("ui/ui.vert");
    d.fragment = rhi::ShaderStageDesc::file("ui/ui.frag");
    d.colorFormats = {render::formats::kSceneColorLDR};
    d.blend = {rhi::BlendState::premultiplied()};
    d.pushConstantSize = sizeof(UiPush);
    m_pipeline = ctx.device.createGraphicsPipeline(d);
    if (!m_pipeline) {
        OX_LOG_ERROR("ui", "UI overlay pipeline creation failed: {}", ctx.device.lastPipelineError());
        return false;
    }
    m_syncedVersion = ~0ull;
    return true;
}

void UiOverlayFeature::shutdown(rhi::Device& device) {
    if (m_pipeline) device.destroy(m_pipeline);
    m_pipeline = {};
    for (auto& [id, t] : m_textures) device.destroy(t.handle);
    m_textures.clear();
    m_syncedVersion = ~0ull;
}

void UiOverlayFeature::syncTextures(rhi::Device& device) {
    UiTextureStore& store = m_bridge->textures();
    const u64 version = store.version();
    if (version == m_syncedVersion) return;
    OX_PROFILE_ZONE_N("UI texture sync");
    auto entries = store.snapshot();
    for (auto it = m_textures.begin(); it != m_textures.end();) {
        if (!entries.contains(it->first)) {
            device.destroy(it->second.handle);
            it = m_textures.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [id, e] : entries) {
        GpuTexture& t = m_textures[id];
        if (t.handle && t.version == e.version) continue;
        if (!e.pixels || e.width == 0 || e.height == 0 || e.pixels->size() < usize(e.width) * e.height * 4) continue;
        if (!t.handle || t.width != e.width || t.height != e.height) {
            if (t.handle) device.destroy(t.handle);
            rhi::TextureDesc td;
            td.format = VK_FORMAT_R8G8B8A8_UNORM;
            td.width = e.width;
            td.height = e.height;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
            td.name = e.name.empty() ? std::format("ui.texture{}", id) : "ui." + e.name;
            t.handle = device.createTexture(td);
            t.width = e.width;
            t.height = e.height;
            t.index = device.sampledIndex(t.handle);
        }
        // Synchronous: textures change rarely (font atlas growth, image loads) and must be ready for this frame.
        device.uploadTexture(t.handle, std::span<const u8>(e.pixels->data(), usize(e.width) * e.height * 4),
                             {.finalAccess = rhi::Access::SampledGraphics});
        t.version = e.version;
    }
    m_syncedVersion = version;
}

void UiOverlayFeature::publishInfo(render::FeatureContext& ctx) {
    RenderInfo info;
    info.valid = true;
    info.stats = ctx.renderer().stats();
    info.frame = info.stats.frame;
    info.memory = ctx.device().memoryStats();
    info.caps = ctx.caps();
    info.upscalers = upscalerAvailability(ctx.caps());
    info.outputSize = {ctx.outputExtent().width, ctx.outputExtent().height};
    info.renderSize = {ctx.renderExtent().width, ctx.renderExtent().height};
    m_bridge->setRenderInfo(std::move(info));
}

void UiOverlayFeature::setup(render::FeatureContext& ctx) {
    OX_PROFILE_ZONE();
    const render::ViewFlags& flags = ctx.view().desc().flags;
    if (!flags.overlays || (flags.editor && !m_bridge->drawInEditorViews.load())) return;
    publishInfo(ctx);

    rhi::RenderGraph& graph = ctx.graph();
    const rhi::RGTexture ldr = ctx.resources().texture(render::res::kSceneColorLDR);
    if (!ldr.valid()) return;

    if (m_bridge->captureRenderGraph.load()) {
        // The plan is compiled when the graph executes: capture it from inside a (no-op) pass at the very end.
        UiRenderBridge* bridge = m_bridge.get();
        const std::string view = ctx.view().desc().name;
        const render::RenderStats stats = ctx.renderer().stats();
        rhi::RenderGraph* g = &graph;
        graph.addPass("UI.GraphCapture").sideEffect().execute([bridge, view, stats, g](rhi::PassContext&) {
            bridge->setRenderGraph(captureGraph(*g, stats, view, stats.frame));
        });
    }

    const std::shared_ptr<const UiFrame> frame = m_bridge->latest();
    if (!frame || frame->empty() || frame->size.x == 0 || frame->size.y == 0 || !m_pipeline) return;
    rhi::Device& device = ctx.device();
    syncTextures(device);

    const VkDeviceAddress vertices = ctx.upload(std::span<const UiVertex>(frame->vertices));
    const VkDeviceAddress indices = ctx.upload(std::span<const u32>(frame->indices));
    const VkDeviceAddress transforms =
        frame->transforms.empty() ? 0 : ctx.upload(std::span<const glm::mat4>(frame->transforms));

    const render::Extent2D out = ctx.outputExtent();
    const f32 sx = f32(out.width) / f32(frame->size.x), sy = f32(out.height) / f32(frame->size.y);
    const u32 white = ctx.defaults().whiteIndex;

    std::vector<ResolvedCmd> cmds;
    cmds.reserve(frame->commands.size());
    for (const UiDrawCmd& c : frame->commands) {
        if (c.indexCount == 0) continue;
        i32 x0 = i32(std::floor(f32(c.clip.x) * sx)), y0 = i32(std::floor(f32(c.clip.y) * sy));
        i32 x1 = i32(std::ceil(f32(c.clip.z) * sx)), y1 = i32(std::ceil(f32(c.clip.w) * sy));
        x0 = std::clamp(x0, 0, i32(out.width));
        y0 = std::clamp(y0, 0, i32(out.height));
        x1 = std::clamp(x1, 0, i32(out.width));
        y1 = std::clamp(y1, 0, i32(out.height));
        if (x1 <= x0 || y1 <= y0) continue;
        u32 texture = white;
        if (isBindlessTexture(c.texture)) {
            texture = u32(c.texture & 0xFFFFFFFFu);
        } else if (c.texture != 0) {
            auto it = m_textures.find(u32(c.texture));
            if (it == m_textures.end() || !it->second.handle) continue; // not uploaded (destroyed meanwhile)
            texture = it->second.index;
        }
        ResolvedCmd r;
        r.pc.vertices = vertices;
        r.pc.indices = indices;
        r.pc.transforms = transforms;
        r.pc.xform[0] = c.xform.x;
        r.pc.xform[1] = c.xform.y;
        r.pc.xform[2] = c.xform.z;
        r.pc.xform[3] = c.xform.w;
        r.pc.ndcScale[0] = 2.0f / f32(frame->size.x);
        r.pc.ndcScale[1] = 2.0f / f32(frame->size.y);
        r.pc.firstIndex = c.firstIndex;
        r.pc.vertexOffset = c.vertexOffset;
        r.pc.transform = (c.transform >= 0 && transforms != 0 && usize(c.transform) < frame->transforms.size()) ? c.transform : -1;
        r.pc.texture = texture;
        r.pc.flags = c.flags;
        r.indexCount = c.indexCount;
        r.x = x0;
        r.y = y0;
        r.w = u32(x1 - x0);
        r.h = u32(y1 - y0);
        cmds.push_back(r);
        ctx.countDraw(c.indexCount / 3);
    }
    if (cmds.empty()) return;

    const rhi::PipelineHandle pipeline = m_pipeline;
    graph.addPass("UI")
        .color(ldr, VK_ATTACHMENT_LOAD_OP_LOAD)
        .execute([pipeline, out, frame, cmds = std::move(cmds)](rhi::PassContext& p) {
            p.cmd.bindPipeline(pipeline);
            p.cmd.setViewport(0.0f, 0.0f, f32(out.width), f32(out.height));
            for (const ResolvedCmd& c : cmds) {
                p.cmd.setScissor(c.x, c.y, c.w, c.h);
                p.cmd.pushConstants(c.pc);
                p.cmd.draw(c.indexCount);
            }
            p.cmd.setScissor(0, 0, out.width, out.height);
        });
}

UiOverlayFeature& attachRenderer(render::Renderer& renderer, std::shared_ptr<UiRenderBridge> bridge) {
    if (renderer.features().find(UiOverlayFeature::kName)) renderer.features().remove(UiOverlayFeature::kName, &renderer.device());
    return renderer.features().emplace<UiOverlayFeature>(std::move(bridge));
}

} // namespace ox::ui
