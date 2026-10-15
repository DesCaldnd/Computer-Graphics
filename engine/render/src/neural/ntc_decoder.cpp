#include "neural_internal.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/render/gpu_types.hpp>
#include <oxwald/render/neural/neural.hpp>
#include <oxwald/rhi/command_list.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/pipeline.hpp>

#include <cstddef>
#include <format>

namespace ox::render::neural {

namespace {

struct DecodePush {
    VkDeviceAddress weights;
    u32 w0, b0, w1, b1, w2, b2;
    u32 hidden, width, height, mip, channels, features;
    u32 grid0, grid1, g0Size;
    u32 outAlbedo, outNormal, outOrm, outEmissive;
};
constexpr u32 kDecodePushSize = 84; // scalar layout in ntc_decode.comp
static_assert(offsetof(DecodePush, outEmissive) + 4 == kDecodePushSize);

u32 packChannels(const assets::NtcChannelLayout& l) {
    auto o = [](i32 v) { return v < 0 ? 15u : u32(v); };
    return o(l.albedo) | o(l.alpha) << 4 | o(l.normal) << 8 | o(l.orm) << 12 | o(l.emissive) << 16;
}

} // namespace

NtcGpuDecoder::NtcGpuDecoder(rhi::Device& device) : m_device(&device) {}

NtcGpuDecoder::~NtcGpuDecoder() {
    for (auto& [k, p] : m_pipelines)
        if (p) m_device->destroy(p);
}

rhi::PipelineHandle NtcGpuDecoder::pipeline(NeuralPath path) {
    if (!neuralPathSupported(m_device->caps(), path)) return {};
    if (auto it = m_pipelines.find(u32(path)); it != m_pipelines.end()) return it->second;
    std::vector<rhi::ShaderDefine> defs;
    if (path == NeuralPath::ScalarF16) defs.push_back({"OX_NN_FP16"});
    if (path == NeuralPath::CoopVector) defs.push_back({"OX_NN_COOPVEC"});
    if (path == NeuralPath::CoopMatrix) {
        defs.push_back({"OX_NN_COOPMAT"});
        defs.push_back({"OX_NN_SUBGROUP", std::to_string(m_device->caps().subgroupSize)});
        if (m_device->caps().cooperativeMatrixFp16Accumulate) defs.push_back({"OX_NN_CM_FP16ACC"});
    }
    rhi::ComputePipelineDesc d;
    d.name = std::format("ntc.decode.{}", neuralPathName(path));
    d.shader = rhi::ShaderStageDesc::file("render/neural/ntc_decode.comp", defs);
    const rhi::PipelineHandle p = m_device->createComputePipeline(d);
    m_pipelines[u32(path)] = p;
    return p;
}

NtcGpuDecoder::Result NtcGpuDecoder::decode(const assets::NeuralTextureSet& set, NeuralPath path, bool timed, u32 repeat,
                                            bool readback) {
    Result r;
    const rhi::PipelineHandle pipe = pipeline(path);
    if (!pipe || set.pairs.empty()) return r;
    rhi::Device& dev = *m_device;
    NeuralNetwork net;
    if (!net.upload(dev, set.network, "ntc.decode.weights")) return r;
    std::vector<rhi::TextureHandle> grids;
    for (const assets::NtcLevelPair& p : set.pairs)
        for (const assets::NtcLatentGrid& g : p.grids) grids.push_back(detail::uploadLatentGrid(dev, set, g, false));

    struct MipTargets {
        std::array<rhi::TextureHandle, 4> tex{};
        DecodePush pc{};
        u32 w = 0, h = 0;
    };
    std::vector<MipTargets> mips(set.neuralMips);
    const i32 present[4] = {set.channels.albedo, set.channels.normal, set.channels.orm, set.channels.emissive};
    const auto& l = net.layers();
    for (u32 m = 0; m < set.neuralMips; ++m) {
        MipTargets& t = mips[m];
        t.w = set.mipWidth(m);
        t.h = set.mipHeight(m);
        u32 out[4] = {kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex};
        for (u32 i = 0; i < 4; ++i) {
            if (present[i] < 0) continue;
            rhi::TextureDesc d;
            d.name = "ntc.decode.out";
            d.format = VK_FORMAT_R8G8B8A8_UNORM;
            d.width = t.w;
            d.height = t.h;
            d.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSrc;
            t.tex[i] = dev.createTexture(d);
            out[i] = dev.storageIndex(t.tex[i]);
        }
        const u32 pair = m / 2;
        const assets::NtcLatentGrid& g0 = set.pairs[pair].grids[0];
        t.pc = {net.address(), l[0].weightOffset, l[0].biasOffset, l[1].weightOffset, l[1].biasOffset, l[2].weightOffset,
                l[2].biasOffset, set.hidden(), set.width, set.height, m, packChannels(set.channels),
                set.pairs[pair].grids[0].features | set.pairs[pair].grids[1].features << 8,
                dev.sampledIndex(grids[pair * 2]), dev.sampledIndex(grids[pair * 2 + 1]), g0.width | g0.height << 16,
                out[0], out[1], out[2], out[3]};
    }
    auto record = [&](rhi::CommandList& cmd) {
        for (MipTargets& t : mips)
            for (rhi::TextureHandle tex : t.tex)
                if (tex) cmd.transition(tex, rhi::Access::StorageWriteCompute, true);
        cmd.bindPipeline(pipe);
        for (MipTargets& t : mips) {
            cmd.pushConstants(&t.pc, kDecodePushSize);
            if (path == NeuralPath::CoopMatrix) cmd.dispatch((t.w * t.h + 1023) / 1024);
            else cmd.dispatchThreads(t.w, t.h);
        }
        for (MipTargets& t : mips)
            for (rhi::TextureHandle tex : t.tex)
                if (tex) cmd.transition(tex, rhi::Access::TransferRead);
    };
    if (timed) {
        f64 total = 0.0;
        u32 samples = 0;
        // Warm-up frames (clocks, first-use costs) are not counted.
        constexpr u32 kWarmup = 4;
        const u32 frames = kWarmup + std::max(repeat, 1u) + dev.framesInFlight() + 1;
        for (u32 f = 0; f < frames; ++f) {
            dev.beginFrame();
            for (const rhi::GpuTiming& t : dev.gpuTimings())
                if (t.name == "ntc.decode" && f >= kWarmup + dev.framesInFlight()) total += t.milliseconds, ++samples;
            if (f < kWarmup + std::max(repeat, 1u)) {
                rhi::CommandList& cmd = dev.commandList(rhi::QueueType::Graphics, "ntc.decode");
                cmd.beginTimestamp("ntc.decode");
                record(cmd);
                cmd.endTimestamp();
                dev.submit(cmd);
            }
            dev.endFrame();
        }
        r.gpuMs = samples ? total / samples : 0.0;
    } else {
        dev.immediateSubmit(record);
    }
    dev.waitIdle();
    if (readback) {
        for (MipTargets& t : mips) {
            std::array<assets::Image, 4> imgs;
            for (u32 i = 0; i < 4; ++i) {
                if (!t.tex[i]) continue;
                const std::vector<u8> px = dev.readTexture(t.tex[i]);
                assets::Image img(t.w, t.h);
                for (usize p = 0; p < img.pixels.size() && p * 4 + 3 < px.size(); ++p)
                    img.pixels[p] = glm::vec4(px[p * 4], px[p * 4 + 1], px[p * 4 + 2], px[p * 4 + 3]) / 255.0f;
                imgs[i] = std::move(img);
            }
            r.mips.push_back(std::move(imgs));
        }
    }
    for (MipTargets& t : mips)
        for (rhi::TextureHandle tex : t.tex)
            if (tex) dev.destroy(tex);
    for (rhi::TextureHandle g : grids) dev.destroy(g);
    r.ok = true;
    return r;
}

} // namespace ox::render::neural
