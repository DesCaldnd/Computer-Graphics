// "GlobalIlluminationRT" (group "IndirectDiffuse"): DDGI probe volume around the camera updated with ray queries every
// frame (first view), applied per view into IndirectDiffuse. DdgiVolumeRenderer (ddgi.hpp) owns the plain-compute
// parts (probe blending, borders, apply) so they are testable without ray tracing hardware.
#include "rt_internal.hpp"

#include <oxwald/core/hash.hpp>

#include <cstddef>

namespace ox::render::rt {

namespace {

struct UpdatePush {
    u64 volume, state;
    u32 rayTexture, atlas, probeCount, pad;
};
struct BorderPush {
    u64 volume, state;
    u32 irradiance, depth, probeCount, pad;
};
struct ApplyPush {
    u64 view, scene, volume;
    u32 depth, normals, outDiffuse, width, height, scale;
};
struct TracePush {
    u64 view, scene, rt;
    u32 rayTexture, probeCount, frame;
};

glm::vec4 randomRotation(u32 frame) {
    // Uniform random quaternion (Shoemake) from a hashed frame number.
    Random rng(hashCombine(0xDD61ull, frame));
    const f32 u1 = rng.nextFloat(), u2 = rng.nextFloat() * kTwoPi, u3 = rng.nextFloat() * kTwoPi;
    const f32 a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
    return {a * std::sin(u2), a * std::cos(u2), b * std::sin(u3), b * std::cos(u3)};
}

} // namespace

// --- DdgiVolumeRenderer ---

bool DdgiVolumeRenderer::initialize(rhi::Device& device) {
    if (m_updateIrradiance) return true;
    m_updateIrradiance = createComputePipeline(device, "rt.ddgi.updateIrradiance", "render/raytracing/ddgi_update.comp");
    m_updateDepth = createComputePipeline(device, "rt.ddgi.updateDepth", "render/raytracing/ddgi_update.comp", {{"OX_DDGI_DEPTH"}});
    m_border = createComputePipeline(device, "rt.ddgi.border", "render/raytracing/ddgi_border.comp");
    m_apply = createComputePipeline(device, "rt.ddgi.apply", "render/raytracing/ddgi_apply.comp");
    return device.vkPipeline(m_updateIrradiance) != VK_NULL_HANDLE;
}

void DdgiVolumeRenderer::shutdown(rhi::Device& device) {
    for (rhi::PipelineHandle* p : {&m_updateIrradiance, &m_updateDepth, &m_border, &m_apply}) {
        if (*p) device.destroy(*p);
        *p = {};
    }
    if (m_irradiance) device.destroy(m_irradiance);
    if (m_depth) device.destroy(m_depth);
    if (m_state) device.destroy(m_state);
    m_irradiance = m_depth = {};
    m_state = {};
    m_desc = DdgiVolumeDesc{glm::ivec3(0), 0.0f, 0};
}

void DdgiVolumeRenderer::configure(rhi::Device& device, const DdgiVolumeDesc& desc) {
    if (m_irradiance && desc.counts == m_desc.counts && desc.raysPerProbe == m_desc.raysPerProbe &&
        desc.spacing == m_desc.spacing) {
        return;
    }
    if (m_irradiance) device.destroy(m_irradiance);
    if (m_depth) device.destroy(m_depth);
    if (m_state) device.destroy(m_state);
    m_desc = desc;
    auto make = [&](VkFormat f, u32 texels, const char* name) {
        const glm::uvec2 size = ddgiAtlasSize(desc.counts, texels);
        rhi::TextureDesc td;
        td.format = f;
        td.width = size.x;
        td.height = size.y;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::TransferDst;
        td.name = name;
        return device.createTexture(td);
    };
    m_irradiance = make(VK_FORMAT_R16G16B16A16_SFLOAT, kDdgiIrradianceTexels, "rt.ddgi.irradiance");
    m_depth = make(VK_FORMAT_R16G16_SFLOAT, kDdgiDepthTexels, "rt.ddgi.depth");
    const u64 stateBytes = u64(probeCount()) * sizeof(glm::ivec4);
    m_state = device.createBuffer({stateBytes, rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst,
                                   rhi::MemoryUsage::GpuOnly, "rt.ddgi.probeState"});
    // Every probe starts invalid (w = 0) → first update writes without hysteresis.
    device.immediateSubmit([&](rhi::CommandList& cmd) {
        cmd.fillBuffer(m_state, 0);
        for (rhi::TextureHandle t : {m_irradiance, m_depth}) {
            cmd.transition(t, rhi::Access::TransferWrite, true);
            cmd.clearTexture(t, rhi::ClearColor{});
            cmd.transition(t, rhi::Access::SampledCompute);
        }
        cmd.bufferBarrier(m_state, rhi::Access::TransferWrite, rhi::Access::StorageReadCompute);
    });
}

DdgiVolumeGpu DdgiVolumeRenderer::frameVolume(rhi::Device& device, const glm::vec3& camera, u32 frame) const {
    DdgiVolumeGpu v;
    v.counts = m_desc.counts;
    v.spacing = m_desc.spacing;
    v.minCoord = ddgiMinCoord(m_desc.counts, m_desc.spacing, camera);
    v.raysPerProbe = m_desc.raysPerProbe;
    v.irradianceTexture = m_irradiance ? device.sampledIndex(m_irradiance) : ~0u;
    v.depthTexture = m_depth ? device.sampledIndex(m_depth) : ~0u;
    v.enabled = m_irradiance ? 1u : 0u;
    v.frame = frame;
    v.normalBias = 0.2f * m_desc.spacing;
    v.viewBias = 0.1f * m_desc.spacing;
    v.rayRotation = randomRotation(frame);
    v.probeState = m_state ? device.address(m_state) : 0;
    return v;
}

rhi::RGTexture DdgiVolumeRenderer::createRayTexture(FeatureContext& ctx) const {
    rhi::TextureDesc td;
    td.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    td.width = std::max(m_desc.raysPerProbe, 1u);
    td.height = std::max(probeCount(), 1u);
    td.usage = rhi::TextureUsage::None;
    td.name = "RtDdgiRays";
    return ctx.graph().createTexture(td);
}

void DdgiVolumeRenderer::import(FeatureContext& ctx) {
    rhi::Device& dev = ctx.device();
    m_irradianceRG = importPersistent(ctx.graph(), dev, m_irradiance);
    m_depthRG = importPersistent(ctx.graph(), dev, m_depth);
    m_stateRG = ctx.graph().importBuffer(m_state, dev.desc(m_state), {rhi::Access::General, rhi::Access::Undefined});
    ctx.resources().setTexture("RtDdgiIrradiance", m_irradianceRG);
    ctx.resources().setTexture("RtDdgiDepth", m_depthRG);
}

void DdgiVolumeRenderer::update(FeatureContext& ctx, rhi::RGTexture rays, VkDeviceAddress volume) {
    rhi::RenderGraph& graph = ctx.graph();
    const u32 probes = probeCount();
    const rhi::RGTexture irr = m_irradianceRG, dep = m_depthRG;
    const rhi::RGBuffer state = m_stateRG;
    graph.addPass("RT.DDGI.UpdateIrradiance", rhi::PassType::Compute)
        .read(rays, rhi::Access::SampledCompute)
        .read(state, rhi::Access::StorageReadCompute)
        .write(irr, rhi::Access::StorageWriteCompute)
        .execute([=, this](rhi::PassContext& p) {
            UpdatePush pc{volume, p.address(state), p.sampledIndex(rays), p.storageIndex(irr), probes, 0};
            p.cmd.bindPipeline(m_updateIrradiance);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch(probes);
        });
    graph.addPass("RT.DDGI.UpdateDepth", rhi::PassType::Compute)
        .read(rays, rhi::Access::SampledCompute)
        .read(state, rhi::Access::StorageReadCompute)
        .write(dep, rhi::Access::StorageWriteCompute)
        .execute([=, this](rhi::PassContext& p) {
            UpdatePush pc{volume, p.address(state), p.sampledIndex(rays), p.storageIndex(dep), probes, 0};
            p.cmd.bindPipeline(m_updateDepth);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch(probes);
        });
    graph.addPass("RT.DDGI.Border", rhi::PassType::Compute)
        .write(irr, rhi::Access::StorageWriteCompute)
        .write(dep, rhi::Access::StorageWriteCompute)
        .write(state, rhi::Access::StorageWriteCompute)
        .execute([=, this](rhi::PassContext& p) {
            BorderPush pc{volume, p.address(state), p.storageIndex(irr), p.storageIndex(dep), probes, 0};
            p.cmd.bindPipeline(m_border);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch(probes);
        });
}

rhi::RGTexture DdgiVolumeRenderer::apply(FeatureContext& ctx, VkDeviceAddress volume, rhi::RGTexture depth,
                                         rhi::RGTexture normals, Extent2D extent, u32 scale) {
    rhi::TextureDesc td;
    td.format = formats::kIndirectDiffuse;
    td.width = extent.width;
    td.height = extent.height;
    td.usage = rhi::TextureUsage::None;
    td.name = "RtIndirectDiffuse";
    const rhi::RGTexture out = ctx.graph().createTexture(td);
    const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
    const rhi::RGTexture irr = m_irradianceRG, dep = m_depthRG;
    ctx.graph()
        .addPass("RT.DDGI.Apply", rhi::PassType::Compute)
        .read(depth, rhi::Access::SampledCompute)
        .read(normals, rhi::Access::SampledCompute)
        .read(irr, rhi::Access::SampledCompute)
        .read(dep, rhi::Access::SampledCompute)
        .overwrite(out, rhi::Access::StorageWriteCompute)
        .execute([=, this](rhi::PassContext& p) {
            ApplyPush pc{view, scene, volume, p.sampledIndex(depth), p.sampledIndex(normals), p.storageIndex(out),
                         extent.width, extent.height, scale};
            p.cmd.bindPipeline(m_apply);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch(dispatchGroups(extent.width), dispatchGroups(extent.height));
        });
    return out;
}

// --- feature ---

namespace {

class GlobalIlluminationRtFeature final : public RtFeatureBase {
public:
    using RtFeatureBase::RtFeatureBase;

    std::string_view name() const override { return "GlobalIlluminationRT"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    i32 order() const override { return -100; } // before reflections / volumetrics read the volume
    std::string_view exclusiveGroup() const override { return kGroupIndirectDiffuse; }
    i32 priority() const override { return kRtPriority; }
    std::vector<std::string_view> provides() const override { return {ox::render::res::kIndirectDiffuse}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.RayTracing.GI", "r.RayTracing.GI.RaysPerProbe", "r.RayTracing.GI.ProbesXZ", "r.RayTracing.GI.ProbesY",
                "r.RayTracing.GI.ProbeSpacing", "r.RayTracing.GI.Hysteresis", "r.RayTracing.GI.ResolutionScale"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return rtOn(s, caps) && RtSettings::fromCVars().gi;
    }
    bool initialize(FeatureInitContext& ctx) override {
        if (!ctx.device.caps().rayTracingSupported()) return false;
        m_trace = createComputePipeline(ctx.device, "rt.ddgi.trace", "render/raytracing/ddgi_trace.comp");
        return m_volume.initialize(ctx.device) && m_shared->acquireDenoiser(ctx.device);
    }
    void shutdown(rhi::Device& d) override {
        if (m_trace) d.destroy(m_trace);
        m_volume.shutdown(d);
        m_shared->releaseDenoiser(d);
    }

    void setup(FeatureContext& ctx) override {
        RtViewInputs in;
        if (!gather(ctx, in)) return;
        rhi::Device& dev = ctx.device();
        const RtSettings& st = rts();
        DdgiVolumeDesc desc;
        desc.counts = {st.giProbesXZ, st.giProbesY, st.giProbesXZ};
        desc.spacing = st.giProbeSpacing;
        desc.raysPerProbe = u32(st.giRaysPerProbe);
        m_volume.configure(dev, desc);
        m_volume.import(ctx);
        const VkDeviceAddress volumeAddr = m_shared->header + offsetof(RtSceneHeaderGpu, ddgi);
        if (m_lastFrame != dev.frameNumber()) {
            // First view of the frame: move the window, trace, blend. Other RT passes see the volume in the header.
            m_lastFrame = dev.frameNumber();
            DdgiVolumeGpu v = m_volume.frameVolume(dev, ctx.view().camera().position(), u32(dev.frameNumber()));
            v.hysteresis = st.giHysteresis;
            m_shared->headerCpu->ddgi = v;
            const rhi::RGTexture rays = m_volume.createRayTexture(ctx);
            const u32 probes = m_volume.probeCount(), raysPerProbe = desc.raysPerProbe;
            const rhi::RGTexture irr = m_volume.irradiance(), dep = m_volume.depth();
            ctx.graph()
                .addPass("RT.DDGI.Trace", rhi::PassType::Compute)
                .read(in.rtScene, rhi::Access::StorageReadCompute)
                .read(irr, rhi::Access::SampledCompute)
                .read(dep, rhi::Access::SampledCompute)
                .overwrite(rays, rhi::Access::StorageWriteCompute)
                .execute([=, this](rhi::PassContext& p) {
                    TracePush pc{in.view, in.scene, in.rt, p.storageIndex(rays), probes, in.frame};
                    p.cmd.bindPipeline(m_trace);
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatch(dispatchGroups(raysPerProbe, 64), probes);
                });
            m_volume.update(ctx, rays, volumeAddr);
            m_volumeCpu = v;
        } else {
            m_shared->headerCpu->ddgi = m_volumeCpu;
        }
        const Extent2D se = scaled(in.render, st.giResolutionScale);
        const u32 scale = se == in.render ? 1u : 2u;
        rhi::RGTexture diffuse = m_volume.apply(ctx, volumeAddr, in.depth, in.normals, se, scale);
        if (scale > 1) {
            diffuse = m_shared->denoiser.upsample(ctx, "RtIndirectDiffuse", diffuse, se, denoiserInputs(in, diffuse, se),
                                                  DenoiseOutput::RGBA16F);
        }
        ctx.resources().setTexture(ox::render::res::kIndirectDiffuse, diffuse);
    }

private:
    DdgiVolumeRenderer m_volume;
    DdgiVolumeGpu m_volumeCpu;
    rhi::PipelineHandle m_trace;
    u64 m_lastFrame = ~0ull;
};

} // namespace

std::unique_ptr<IRenderFeature> makeGlobalIlluminationRtFeature(std::shared_ptr<RtShared> shared) {
    return std::make_unique<GlobalIlluminationRtFeature>(std::move(shared));
}

} // namespace ox::render::rt
