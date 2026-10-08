// "PathTracer" (r.PathTracing 1): progressive reference path tracer replacing SceneColorHDR before post-processing.
// Mode 0 = ray query megakernel (path_trace.comp), mode 1 = RT pipeline with SBT (path_trace.rgen + closest-hit /
// any-hit / miss; falls back to mode 0 without VK_KHR_ray_tracing_pipeline). The accumulation restarts whenever the
// camera, the scene (instances, BLASes, materials, moved objects), the lights, the environment or the settings change.
#include "rt_internal.hpp"

#include <oxwald/core/hash.hpp>

#include <cstring>

namespace ox::render::rt {

namespace {

struct PathPush {
    u64 view, scene, rt;
    u32 accum, sceneColor, width, height, frame, maxBounces, samples, reset;
};

struct PathViewState final : IFeatureViewState {
    rhi::TextureHandle accum;
    AccumulationTracker tracker;
    void release(rhi::Device& d) override {
        if (accum) d.destroy(accum);
        accum = {};
    }
};

u64 hashBytes(u64 seed, const void* data, usize size) {
    return fnv1a64(std::span<const std::byte>(static_cast<const std::byte*>(data), size), seed);
}

class PathTracerFeature final : public RtFeatureBase {
public:
    using RtFeatureBase::RtFeatureBase;

    std::string_view name() const override { return "PathTracer"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::BeforePostProcess); }
    i32 order() const override { return -10000; } // before TAA / post-processing
    std::vector<std::string_view> provides() const override { return {ox::render::res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.PathTracing", "r.PathTracing.Mode", "r.PathTracing.MaxBounces", "r.PathTracing.SamplesPerFrame",
                "r.PathTracing.MaxSamples"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return rtOn(s, caps) && RtSettings::fromCVars().pathTracing;
    }
    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        if (!dev.caps().rayTracingSupported()) return false;
        m_kernel = createComputePipeline(dev, "rt.pathTrace", "render/raytracing/path_trace.comp");
        if (dev.caps().rayTracingPipeline) {
            using rhi::ShaderStageDesc;
            rhi::RayTracingPipelineDesc d;
            d.name = "rt.pathTrace.pipeline";
            d.rayGen = ShaderStageDesc::file("render/raytracing/path_trace.rgen");
            d.miss = {ShaderStageDesc::file("render/raytracing/path_trace.rmiss"),
                      ShaderStageDesc::file("render/raytracing/path_shadow.rmiss")};
            const ShaderStageDesc chit = ShaderStageDesc::file("render/raytracing/path_trace.rchit");
            const ShaderStageDesc ahit = ShaderStageDesc::file("render/raytracing/path_trace.rahit");
            // [hit group][ray type] in RtHitGroup order (Opaque, AlphaTested, Translucent) × (radiance, shadow).
            d.hitGroups = {{chit, {}, {}}, {{}, {}, {}}, {chit, ahit, {}}, {{}, ahit, {}}, {chit, {}, {}}, {{}, {}, {}}};
            d.maxRecursionDepth = 1;
            m_pipeline = dev.createRayTracingPipeline(d);
        }
        return true;
    }
    void shutdown(rhi::Device& d) override {
        if (m_kernel) d.destroy(m_kernel);
        if (m_pipeline) d.destroy(m_pipeline);
    }

    void setup(FeatureContext& ctx) override {
        RtViewInputs in;
        if (!gather(ctx, in)) return;
        rhi::Device& dev = ctx.device();
        const RtSettings& st = rts();
        FrameState& fs = frameState(ctx);
        const rhi::RGTexture hdr = ctx.resources().texture(ox::render::res::kSceneColorHDR);
        if (!hdr.valid()) return;
        PathViewState& vs = ctx.viewState<PathViewState>();
        if (!vs.accum || dev.desc(vs.accum).width != in.render.width || dev.desc(vs.accum).height != in.render.height) {
            if (vs.accum) dev.destroy(vs.accum);
            rhi::TextureDesc td = texDesc(VK_FORMAT_R32G32B32A32_SFLOAT, in.render, "rt.pathAccum");
            td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
            vs.accum = dev.createTexture(td);
            vs.tracker.reset();
        }
        const GpuViewConstants& c = ctx.viewConstants();
        u64 cameraHash = hashMatrix(c.unjitteredViewProj);
        cameraHash = hashCombine(cameraHash, (u64(in.render.width) << 32) | in.render.height);
        u64 sceneHash = m_shared->sceneStructureHash;
        if (ctx.scene().movedInstanceCount() > 0) sceneHash = hashCombine(sceneHash, dev.frameNumber()); // animating
        sceneHash = hashBytes(sceneHash, fs.lights.data(), fs.lights.size() * sizeof(GpuLight));
        const u32 envU[] = {c.skyMode, c.skyCube, c.prefilteredCube, u32(c.sunLight)};
        const f32 envF[] = {c.skyIntensity, c.iblIntensity, c.sunAngularRadius};
        sceneHash = hashBytes(sceneHash, envU, sizeof(envU));
        sceneHash = hashBytes(sceneHash, envF, sizeof(envF));
        sceneHash = hashBytes(sceneHash, c.preetham, sizeof(c.preetham));
        const bool usePipeline = st.pathTracingMode == 1 && m_pipeline && dev.vkPipeline(m_pipeline) != VK_NULL_HANDLE;
        const u64 settingsHash = hashCombine(hashCombine(u64(st.pathMaxBounces), usePipeline ? 1u : 0u), st.shadowColored ? 1u : 0u);
        const bool reset = vs.tracker.update(cameraHash, sceneHash, settingsHash);
        const u32 remaining = u32(std::max(st.pathMaxSamples, 1)) - std::min(vs.tracker.sampleCount(), u32(std::max(st.pathMaxSamples, 1)));
        const u32 samples = std::min(u32(std::max(st.pathSamplesPerFrame, 1)), remaining);
        vs.tracker.addSamples(samples);

        const rhi::RGTexture accum = importPersistent(ctx.graph(), dev, vs.accum);
        const u32 bounces = u32(std::max(st.pathMaxBounces, 1));
        const u32 frame = u32(dev.frameNumber());
        const Extent2D e = in.render;
        const rhi::PipelineHandle pipe = usePipeline ? m_pipeline : m_kernel;
        const rhi::Access storage = usePipeline ? rhi::Access::StorageWriteRayTracing : rhi::Access::StorageWriteCompute;
        ctx.graph()
            .addPass(usePipeline ? "RT.PathTrace.Pipeline" : "RT.PathTrace", usePipeline ? rhi::PassType::RayTracing : rhi::PassType::Compute)
            .read(in.rtScene, rhi::Access::StorageReadCompute)
            .write(accum, storage)
            .write(hdr, storage)
            .execute([=](rhi::PassContext& p) {
                PathPush pc{in.view, in.scene, in.rt, p.storageIndex(accum), p.storageIndex(hdr), e.width, e.height,
                            frame, bounces, samples, reset ? 1u : 0u};
                p.cmd.bindPipeline(pipe);
                p.cmd.pushConstants(pc);
                if (usePipeline) p.cmd.traceRays(e.width, e.height);
                else p.cmd.dispatch(dispatchGroups(e.width), dispatchGroups(e.height));
            });
    }

private:
    rhi::PipelineHandle m_kernel, m_pipeline;
};

} // namespace

std::unique_ptr<IRenderFeature> makePathTracerFeature(std::shared_ptr<RtShared> shared) {
    return std::make_unique<PathTracerFeature>(std::move(shared));
}

} // namespace ox::render::rt
