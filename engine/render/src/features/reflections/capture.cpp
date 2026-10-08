// Scene captures (reflection probe faces, irradiance probe faces, planar reflections).
#include "reflection_internal.hpp"

#include <cstring>

namespace ox::render::reflections {

namespace {

struct CapturePush {
    u64 view = 0;
    u64 scene = 0;
    u64 drawIds = 0;
    glm::vec4 mainForward{0.0f};
    glm::vec4 mainPosition{0.0f};
    u64 lightList = 0;
    u32 lightCount = 0;
    u32 flags = 0;
};
static_assert(sizeof(CapturePush) == 72);

} // namespace

bool SceneCapture::init(rhi::Device& dev) {
    for (u32 variant = 0; variant < kVariantCount; ++variant) {
        rhi::GraphicsPipelineDesc d;
        d.name = std::format("render.reflections.capture.v{}", variant);
        d.vertex = rhi::ShaderStageDesc::file("render/passes/mesh.vert");
        std::vector<rhi::ShaderDefine> defs;
        if (variant & kVariantAlphaTest) defs.push_back({"OX_ALPHA_TEST"});
        d.fragment = rhi::ShaderStageDesc::file("render/reflections/capture.frag", defs);
        d.colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT};
        d.depthFormat = formats::kDepth;
        d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
        d.raster.cullMode = (variant & kVariantDoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        // Cube faces and mirrored planar views have a mirrored basis: front faces wind clockwise.
        d.raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        m_mesh[variant] = dev.createGraphicsPipeline(d);
    }
    rhi::GraphicsPipelineDesc s;
    s.name = "render.reflections.captureSky";
    s.vertex = fullscreenVertexShader();
    s.fragment = rhi::ShaderStageDesc::file("render/reflections/capture_sky.frag");
    s.colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT};
    s.depthFormat = formats::kDepth;
    s.depth = {true, false, VK_COMPARE_OP_GREATER_OR_EQUAL};
    m_sky = dev.createGraphicsPipeline(s);
    return true;
}

void SceneCapture::shutdown(rhi::Device& dev) {
    for (rhi::PipelineHandle& p : m_mesh) {
        if (p) dev.destroy(p);
        p = {};
    }
    if (m_sky) dev.destroy(m_sky);
    m_sky = {};
}

GpuViewConstants SceneCapture::makeConstants(const GpuViewConstants& main, const glm::mat4& view, const glm::mat4& proj,
                                             glm::vec3 position, Extent2D size, f32 preExposure) {
    GpuViewConstants c = main;
    c.view = view;
    c.proj = proj;
    c.viewProj = proj * view;
    c.invView = glm::inverse(view);
    c.invProj = glm::inverse(proj);
    c.invViewProj = glm::inverse(c.viewProj);
    c.unjitteredViewProj = c.viewProj;
    c.prevUnjitteredViewProj = c.viewProj;
    c.prevViewProj = c.viewProj;
    c.cameraPosition = glm::vec4(position, main.cameraPosition.w);
    c.renderSize = {f32(size.width), f32(size.height), 1.0f / f32(std::max(size.width, 1u)),
                    1.0f / f32(std::max(size.height, 1u))};
    c.outputSize = c.renderSize;
    c.jitter = glm::vec4(0.0f);
    c.preExposure = preExposure;
    c.flags = 0;
    c.debugView = 0;
    c.lightClusterCount = 0;
    c.mipBias = 0.0f;
    c.planarReflections = 0;
    // Cheaper shadow filtering in captures: one hardware compare, no PCSS blocker search.
    c.shadowParams.z = 0.0f;
    c.shadowParams.w = 0.0f;
    return c;
}

void SceneCapture::addPass(FeatureContext& ctx, const Request& rq) {
    FrameResources& R = ctx.resources();
    const GpuAllocation constants = ctx.allocate(sizeof(GpuViewConstants), 16);
    std::memcpy(constants.cpu, &rq.constants, sizeof(GpuViewConstants));
    DrawFilter filter = rq.filter;
    filter.bucketMask = (1u << u32(DrawBucket::Opaque)) | (1u << u32(DrawBucket::Masked));
    const DrawList list = ctx.buildDrawList(filter);

    const GpuViewConstants& mainC = ctx.viewConstants();
    CapturePush pc;
    pc.view = constants.address;
    pc.scene = ctx.sceneAddress();
    pc.drawIds = list.instanceIds;
    pc.mainForward = glm::vec4(-glm::vec3(mainC.invView[2]), 0.0f);
    pc.mainPosition = glm::vec4(glm::vec3(mainC.cameraPosition), 0.0f);
    // Local lights touching the capture volume (the capture has no light clusters).
    {
        const FrameState& frame = frameState(ctx);
        const u32 cap = u32(std::max(cv::captureMaxLocalLights.get(), 0));
        std::vector<u32> ids;
        for (u32 i = frame.directionalCount; i < u32(frame.lights.size()) && ids.size() < cap; ++i) {
            const GpuLight& l = frame.lights[i];
            const Sphere s{l.position, l.range};
            if (filter.frustum && !filter.frustum->intersects(s)) continue;
            if (filter.sphere && glm::length(filter.sphere->center - s.center) > filter.sphere->radius + s.radius) continue;
            ids.push_back(i);
        }
        pc.lightList = ctx.upload(std::span<const u32>(ids));
        pc.lightCount = u32(ids.size());
    }
    pc.flags = rq.sunDisk ? 1u : 0u;

    rhi::RenderGraph& g = ctx.graph();
    const rhi::RGTexture depth = g.createTexture(textureDesc(formats::kDepth, rq.size, "Reflections.CaptureDepth"));
    rhi::PassBuilder pb = g.addPass(rq.name);
    pb.color(rq.target, VK_ATTACHMENT_LOAD_OP_CLEAR, rq.clear, 0, rq.layer).depth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, {0.0f, 0});
    for (std::string_view n : {render::res::kShadowCascades, render::res::kShadowAtlas, render::res::kPointShadows}) {
        if (rhi::RGTexture t = R.texture(n); t.valid()) pb.read(t, rhi::Access::SampledFragment);
    }
    FrameState* fs = &frameState(ctx);
    IRenderFeature* feature = ctx.feature();
    const rhi::PipelineHandle sky = m_sky;
    const std::array<rhi::PipelineHandle, kVariantCount> pipes{m_mesh[0], m_mesh[1], m_mesh[2], m_mesh[3]};
    const glm::uvec4 scissor = rq.scissor;
    pb.execute([fs, feature, list, pc, sky, pipes, scissor](rhi::PassContext& p) {
        FeatureContext fc(*fs, feature, InjectionPoint::Lighting);
        if (scissor.z > 0 && scissor.w > 0) p.cmd.setScissor(i32(scissor.x), i32(scissor.y), scissor.z, scissor.w);
        fc.drawBatches(p.cmd, list, pipes, &pc, sizeof(pc));
        struct {
            u64 view, scene;
            u32 flags;
        } spc{pc.view, pc.scene, pc.flags};
        drawFullscreen(p.cmd, sky, &spc, 20);
    });
}

} // namespace ox::render::reflections
