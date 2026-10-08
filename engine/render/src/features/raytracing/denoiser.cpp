// SVGF-style denoiser (see denoiser.hpp). Plain compute: runs on every device, including MoltenVK.
#include <oxwald/render/features/raytracing/denoiser.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/render_graph.hpp>

#include <format>

namespace ox::render::rt {

namespace {

struct TemporalPush {
    u64 view, scene;
    u32 signal, depth, normals, velocity, prevColor, prevMoments, prevGeo, outColor, outMoments, outGeo;
    u32 width, height, scale, flags;
    f32 maxHistory;
    glm::vec4 luma;
};

struct VariancePush {
    u64 view, scene;
    u32 color, moments, geo, outVariance, width, height;
    glm::vec4 luma;
};

struct AtrousPush {
    u64 view, scene;
    u32 color, variance, geo, outColor, outVariance, width, height, step;
    f32 phiColor, phiNormal, phiDepth;
    glm::vec4 luma;
};
static_assert(sizeof(AtrousPush) <= 128);

struct UpsamplePush {
    u64 view, scene;
    u32 source, depth, normals, outColor, srcWidth, srcHeight, dstWidth, dstHeight, scale;
};

rhi::TextureDesc texDesc(VkFormat format, Extent2D e, std::string name, rhi::TextureUsage usage = rhi::TextureUsage::None) {
    rhi::TextureDesc d;
    d.format = format;
    d.width = std::max(e.width, 1u);
    d.height = std::max(e.height, 1u);
    d.usage = usage;
    d.name = std::move(name);
    return d;
}

u32 groups(u32 n) { return (n + 7) / 8; }

} // namespace

VkFormat denoiseOutputFormat(DenoiseOutput o) {
    switch (o) {
    case DenoiseOutput::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case DenoiseOutput::R8: return VK_FORMAT_R8_UNORM;
    case DenoiseOutput::RGBA16F: break;
    }
    return VK_FORMAT_R16G16B16A16_SFLOAT;
}

bool SvgfDenoiser::initialize(rhi::Device& device) {
    if (initialized()) return true;
    m_temporal = createComputePipeline(device, "rt.denoise.temporal", "render/raytracing/denoise_temporal.comp");
    m_variance = createComputePipeline(device, "rt.denoise.variance", "render/raytracing/denoise_variance.comp");
    const char* defs[3] = {nullptr, "OX_OUT_RGBA8", "OX_OUT_R8"};
    const char* names[3] = {"rgba16f", "rgba8", "r8"};
    for (u32 i = 0; i < 3; ++i) {
        std::vector<rhi::ShaderDefine> d;
        if (defs[i]) d.push_back({defs[i]});
        m_atrous[i] = createComputePipeline(device, std::format("rt.denoise.atrous.{}", names[i]),
                                            "render/raytracing/denoise_atrous.comp", d);
        m_upsample[i] = createComputePipeline(device, std::format("rt.denoise.upsample.{}", names[i]),
                                              "render/raytracing/denoise_upsample.comp", d);
    }
    return device.vkPipeline(m_temporal) != VK_NULL_HANDLE;
}

void SvgfDenoiser::shutdown(rhi::Device& device) {
    for (rhi::PipelineHandle* p : {&m_temporal, &m_variance, &m_atrous[0], &m_atrous[1], &m_atrous[2], &m_upsample[0],
                                   &m_upsample[1], &m_upsample[2]}) {
        if (*p) device.destroy(*p);
        *p = {};
    }
}

rhi::RGTexture SvgfDenoiser::upsample(FeatureContext& ctx, std::string_view name, rhi::RGTexture source,
                                      Extent2D sourceExtent, const DenoiserInputs& g, DenoiseOutput output) {
    const Extent2D dst = g.renderExtent;
    const u32 scale = (sourceExtent == dst) ? 1u : std::max(g.scale, 1u);
    const Extent2D outExtent = scale == 1 ? sourceExtent : dst;
    const rhi::RGTexture out =
        ctx.graph().createTexture(texDesc(denoiseOutputFormat(output), outExtent, std::format("{}.Upsampled", name)));
    const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
    const rhi::PipelineHandle pipe = m_upsample[u32(output)];
    ctx.graph()
        .addPass(std::format("{}.Upsample", name), rhi::PassType::Compute)
        .read(source, rhi::Access::SampledCompute)
        .read(g.depth, rhi::Access::SampledCompute)
        .read(g.normals, rhi::Access::SampledCompute)
        .overwrite(out, rhi::Access::StorageWriteCompute)
        .execute([=](rhi::PassContext& p) {
            UpsamplePush pc{view,           scene,       p.sampledIndex(source), p.sampledIndex(g.depth),
                            p.sampledIndex(g.normals), p.storageIndex(out), sourceExtent.width, sourceExtent.height,
                            outExtent.width, outExtent.height, scale};
            p.cmd.bindPipeline(pipe);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch(groups(outExtent.width), groups(outExtent.height));
        });
    return out;
}

DenoiserOutputs SvgfDenoiser::denoise(FeatureContext& ctx, std::string_view name, const DenoiserInputs& in,
                                      const DenoiserSettings& st, std::optional<Extent2D> upsampleTo) {
    DenoiserOutputs result;
    rhi::RenderGraph& graph = ctx.graph();
    const Extent2D e = in.signalExtent;
    const bool needUpsample = upsampleTo.has_value() && !(*upsampleTo == e);
    const DenoiseOutput finalFormat = needUpsample ? DenoiseOutput::RGBA16F : st.output;
    if (!st.enabled) {
        if (needUpsample || st.output != DenoiseOutput::RGBA16F) {
            DenoiserInputs g = in;
            if (upsampleTo) g.renderExtent = *upsampleTo;
            if (!needUpsample) g.renderExtent = e;
            result.result = upsample(ctx, name, in.signal, e, g, st.output);
        } else {
            result.result = in.signal;
        }
        return result;
    }

    const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
    const rhi::TextureUsage hu = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
    const HistoryTexture color = ctx.history(std::format("{}.color", name), texDesc(VK_FORMAT_R16G16B16A16_SFLOAT, e, std::format("{}.HistoryColor", name), hu));
    const HistoryTexture moments = ctx.history(std::format("{}.moments", name), texDesc(VK_FORMAT_R16G16B16A16_SFLOAT, e, std::format("{}.HistoryMoments", name), hu));
    const HistoryTexture geo = ctx.history(std::format("{}.geo", name), texDesc(VK_FORMAT_R16G16B16A16_SFLOAT, e, std::format("{}.HistoryGeo", name), hu));
    const bool historyValid = st.temporal && color.previousValid && moments.previousValid && geo.previousValid;
    const u32 iterations = std::min(st.iterations, 5u);
    const f32 maxHistory = f32(std::max(st.maxHistory, 1u));

    // 1. temporal accumulation (into the colour history directly when there is no À-trous pass).
    const rhi::RGTexture accum =
        iterations > 0 ? graph.createTexture(texDesc(VK_FORMAT_R16G16B16A16_SFLOAT, e, std::format("{}.Accum", name)))
                       : color.current;
    {
        rhi::PassBuilder pb = graph.addPass(std::format("{}.Temporal", name), rhi::PassType::Compute);
        pb.read(in.signal, rhi::Access::SampledCompute)
            .read(in.depth, rhi::Access::SampledCompute)
            .read(in.normals, rhi::Access::SampledCompute)
            .read(in.velocity, rhi::Access::SampledCompute)
            .overwrite(accum, rhi::Access::StorageWriteCompute)
            .overwrite(moments.current, rhi::Access::StorageWriteCompute)
            .overwrite(geo.current, rhi::Access::StorageWriteCompute);
        if (historyValid) {
            pb.read(color.previous, rhi::Access::SampledCompute)
                .read(moments.previous, rhi::Access::SampledCompute)
                .read(geo.previous, rhi::Access::SampledCompute);
        }
        const HistoryTexture c = color, m = moments, g = geo;
        const DenoiserInputs i = in;
        pb.execute([=, this](rhi::PassContext& p) {
            const u32 sig = p.sampledIndex(i.signal);
            TemporalPush pc{};
            pc.view = view;
            pc.scene = scene;
            pc.signal = sig;
            pc.depth = p.sampledIndex(i.depth);
            pc.normals = p.sampledIndex(i.normals);
            pc.velocity = p.sampledIndex(i.velocity);
            pc.prevColor = historyValid ? p.sampledIndex(c.previous) : sig;
            pc.prevMoments = historyValid ? p.sampledIndex(m.previous) : sig;
            pc.prevGeo = historyValid ? p.sampledIndex(g.previous) : sig;
            pc.outColor = p.storageIndex(accum);
            pc.outMoments = p.storageIndex(m.current);
            pc.outGeo = p.storageIndex(g.current);
            pc.width = e.width;
            pc.height = e.height;
            pc.scale = std::max(i.scale, 1u);
            pc.flags = (historyValid ? 1u : 0u) | (st.temporal ? 2u : 0u);
            pc.maxHistory = maxHistory;
            pc.luma = st.lumaWeights;
            p.cmd.bindPipeline(m_temporal);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch(groups(e.width), groups(e.height));
        });
    }

    // 2. variance (geometry from the temporal pass: normal + linear depth at signal resolution).
    const rhi::RGTexture geoCur = geo.current;
    const rhi::RGTexture variance = graph.createTexture(texDesc(VK_FORMAT_R16_SFLOAT, e, std::format("{}.Variance", name)));
    {
        const rhi::RGTexture mom = moments.current;
        graph.addPass(std::format("{}.Variance", name), rhi::PassType::Compute)
            .read(accum, rhi::Access::SampledCompute)
            .read(mom, rhi::Access::SampledCompute)
            .read(geoCur, rhi::Access::SampledCompute)
            .overwrite(variance, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                VariancePush pc{view, scene, p.sampledIndex(accum), p.sampledIndex(mom), p.sampledIndex(geoCur),
                                p.storageIndex(variance), e.width, e.height, st.lumaWeights};
                p.cmd.bindPipeline(m_variance);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch(groups(e.width), groups(e.height));
            });
    }

    // 3. À-trous iterations. Iteration 0 feeds the colour history; the last one writes the output format.
    rhi::RGTexture curColor = accum, curVar = variance;
    for (u32 it = 0; it < iterations; ++it) {
        const bool last = it + 1 == iterations;
        const bool toFinal = last && it > 0;
        const DenoiseOutput fmt = toFinal ? finalFormat : DenoiseOutput::RGBA16F;
        rhi::RGTexture outColor;
        if (it == 0) outColor = color.current;
        else outColor = graph.createTexture(texDesc(denoiseOutputFormat(fmt), e, std::format("{}.Atrous{}", name, it)));
        const bool writesVariance = fmt == DenoiseOutput::RGBA16F;
        rhi::RGTexture outVar;
        if (writesVariance) outVar = graph.createTexture(texDesc(VK_FORMAT_R16_SFLOAT, e, std::format("{}.Var{}", name, it)));
        rhi::PassBuilder pb = graph.addPass(std::format("{}.Atrous{}", name, it), rhi::PassType::Compute);
        pb.read(curColor, rhi::Access::SampledCompute)
            .read(curVar, rhi::Access::SampledCompute)
            .read(geoCur, rhi::Access::SampledCompute)
            .overwrite(outColor, rhi::Access::StorageWriteCompute);
        if (writesVariance) pb.overwrite(outVar, rhi::Access::StorageWriteCompute);
        const rhi::RGTexture srcC = curColor, srcV = curVar;
        const rhi::PipelineHandle pipe = m_atrous[u32(fmt)];
        pb.execute([=](rhi::PassContext& p) {
            AtrousPush pc{};
            pc.view = view;
            pc.scene = scene;
            pc.color = p.sampledIndex(srcC);
            pc.variance = p.sampledIndex(srcV);
            pc.geo = p.sampledIndex(geoCur);
            pc.outColor = p.storageIndex(outColor);
            pc.outVariance = writesVariance ? p.storageIndex(outVar) : pc.outColor;
            pc.width = e.width;
            pc.height = e.height;
            pc.step = 1u << it;
            pc.phiColor = st.phiColor;
            pc.phiNormal = st.phiNormal;
            pc.phiDepth = st.phiDepth;
            pc.luma = st.lumaWeights;
            p.cmd.bindPipeline(pipe);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch(groups(e.width), groups(e.height));
        });
        curColor = outColor;
        if (writesVariance) curVar = outVar;
        if (toFinal) result.result = outColor;
    }
    if (iterations == 0) curColor = color.current;
    result.variance = curVar;

    // 4. Format conversion / upsampling of whatever is not final yet.
    if (!result.result.valid()) {
        if (finalFormat == DenoiseOutput::RGBA16F && !needUpsample) {
            result.result = curColor;
        } else {
            DenoiserInputs g = in;
            g.renderExtent = needUpsample ? *upsampleTo : e;
            result.result = upsample(ctx, name, curColor, e, g, needUpsample ? st.output : finalFormat);
        }
    } else if (needUpsample) {
        DenoiserInputs g = in;
        g.renderExtent = *upsampleTo;
        result.result = upsample(ctx, name, result.result, e, g, st.output);
    }
    return result;
}

} // namespace ox::render::rt
