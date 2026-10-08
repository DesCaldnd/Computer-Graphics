#include "translucency_internal.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/paths.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace ox::render::translucency {

namespace {

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

rhi::TextureDesc textureDesc(VkFormat format, Extent2D e, const char* name, u32 mips) {
    rhi::TextureDesc d;
    d.format = format;
    d.width = std::max(e.width, 1u);
    d.height = std::max(e.height, 1u);
    d.mipLevels = mips;
    d.usage = rhi::TextureUsage::None;
    d.name = name;
    return d;
}

rhi::PipelineHandle createSourcePipeline(rhi::Device& device) {
    return createComputePipeline(device, "translucency.refractionSource", "render/translucency/refraction_source.comp");
}

RefractionSources ensureSources(FeatureContext& ctx, rhi::PipelineHandle pipeline) {
    FrameResources& R = ctx.resources();
    RefractionSources out;
    const Extent2D e = ctx.renderExtent();
    const u32 mips = std::clamp<u32>(u32(std::max(refractionMipCount(), 1)), 1u, rhi::fullMipCount(e.width, e.height));
    if (R.hasTexture(res::kSceneColorRefraction) && R.hasTexture(res::kSceneDepthCopy)) {
        out.color = R.texture(res::kSceneColorRefraction);
        out.depth = R.texture(res::kSceneDepthCopy);
        out.mips = mips;
        return out;
    }
    rhi::RenderGraph& g = ctx.graph();
    const rhi::RGTexture hdr = R.texture(res::kSceneColorHDR), depth = R.texture(res::kDepth);
    const rhi::RGTexture color = g.createTexture(textureDesc(formats::kSceneColor, e, "SceneColorRefraction", mips));
    const rhi::RGTexture depthCopy = g.createTexture(textureDesc(VK_FORMAT_R32_SFLOAT, e, "SceneDepthCopy"));
    g.addPass("Translucency.DepthCopy", rhi::PassType::Compute)
        .read(depth, rhi::Access::SampledCompute)
        .overwrite(depthCopy, rhi::Access::StorageWriteCompute)
        .execute([=](rhi::PassContext& p) {
            SourcePush pc;
            pc.mode = 0;
            pc.src = kInvalidIndex;
            pc.depthSrc = p.sampledIndex(depth);
            pc.depthDst = p.storageIndex(depthCopy);
            pc.dstWidth = e.width, pc.dstHeight = e.height;
            p.cmd.bindPipeline(pipeline);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch((e.width + 7) / 8, (e.height + 7) / 8);
        });
    g.addPass("Translucency.RefractionSource", rhi::PassType::Compute)
        .read(hdr, rhi::Access::SampledCompute)
        .overwrite(color, rhi::Access::StorageWriteCompute)
        .execute([=](rhi::PassContext& p) {
            p.cmd.bindPipeline(pipeline);
            SourcePush pc;
            pc.mode = 2; // mip 0 copy + mip 1 box in one pass
            pc.src = p.sampledIndex(hdr);
            pc.dst = p.storageIndex(color, 0);
            pc.depthDst = mips > 1 ? p.storageIndex(color, 1) : kInvalidIndex;
            pc.dstWidth = e.width, pc.dstHeight = e.height;
            p.cmd.pushConstants(pc);
            const u32 hw = std::max(e.width / 2, 1u), hh = std::max(e.height / 2, 1u);
            p.cmd.dispatch((hw + 7) / 8, (hh + 7) / 8);
            u32 w = std::max(e.width >> 1, 1u), h = std::max(e.height >> 1, 1u);
            for (u32 mip = 2; mip < mips; ++mip) {
                p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                const u32 dw = std::max(w >> 1, 1u), dh = std::max(h >> 1, 1u);
                pc.mode = 1;
                pc.src = p.storageIndex(color, mip - 1);
                pc.dst = p.storageIndex(color, mip);
                pc.depthDst = kInvalidIndex;
                pc.srcWidth = w, pc.srcHeight = h, pc.dstWidth = dw, pc.dstHeight = dh;
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((dw + 7) / 8, (dh + 7) / 8);
                w = dw, h = dh;
            }
        });
    R.setTexture(res::kSceneColorRefraction, color);
    R.setTexture(res::kSceneDepthCopy, depthCopy);
    out.color = color;
    out.depth = depthCopy;
    out.mips = mips;
    return out;
}

namespace {

struct Contracts {
    bool fog = false;
    bool planar = false;
};

const Contracts& contracts() {
    static Contracts c;
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<std::filesystem::path> roots;
#ifdef OX_RENDER_SHADER_DIR
        roots.emplace_back(OX_RENDER_SHADER_DIR);
#endif
        if (const char* env = std::getenv("OXWALD_SHADER_DIR")) roots.emplace_back(env);
        roots.push_back(paths::executableDir() / "shaders");
        auto has = [&](const char* file, const char* symbol) {
            for (const auto& r : roots) {
                const std::string src = readFile(r / file);
                if (!src.empty()) return src.find(symbol) != std::string::npos;
            }
            return false;
        };
        c.fog = has("render/volumetrics/fog_sample.glsl", "oxEvaluateVolumetricFog");
        c.planar = has("render/reflections/planar.glsl", "oxSamplePlanarReflection");
        OX_LOG_INFO("render", "translucency contracts: volumetric fog {}, planar reflections {}", c.fog ? "yes" : "no",
                    c.planar ? "yes" : "no");
    });
    return c;
}

} // namespace

std::vector<rhi::ShaderDefine> contractDefines() {
    std::vector<rhi::ShaderDefine> d;
    if (contracts().fog) d.push_back({"OX_HAS_VOLUMETRIC_FOG_SAMPLE"});
    if (contracts().planar) d.push_back({"OX_HAS_PLANAR_REFLECTIONS"});
    return d;
}

rhi::RGTexture declareLightingReads(rhi::PassBuilder& pass, FrameResources& R) {
    if (rhi::RGBuffer c = R.buffer(res::kLightClusters); c.valid()) pass.read(c, rhi::Access::StorageReadGraphics);
    for (std::string_view n : {res::kShadowCascades, res::kShadowAtlas, res::kPointShadows}) {
        if (rhi::RGTexture t = R.texture(n); t.valid()) pass.read(t, rhi::Access::SampledGraphics);
    }
    const rhi::RGTexture fog = R.texture(res::kVolumetricFog);
    if (fog.valid() && contracts().fog) {
        pass.read(fog, rhi::Access::SampledGraphics);
        return fog;
    }
    return {};
}

rhi::TextureHandle createTexture2D(rhi::Device& device, const char* name, u32 width, u32 height,
                                   const std::vector<u8>& rgba8, bool srgb, bool withMips) {
    rhi::TextureDesc d;
    d.name = name;
    d.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    d.width = width;
    d.height = height;
    d.mipLevels = withMips ? rhi::fullMipCount(width, height) : 1;
    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    const rhi::TextureHandle t = device.createTexture(d);
    // CPU box-filtered mip chain (linear average; sRGB textures are only used for small sprites).
    std::vector<u8> all = rgba8;
    std::vector<u8> prev = rgba8;
    u32 w = width, h = height;
    for (u32 mip = 1; mip < d.mipLevels; ++mip) {
        const u32 nw = std::max(w >> 1, 1u), nh = std::max(h >> 1, 1u);
        std::vector<u8> next(usize(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y) {
            for (u32 x = 0; x < nw; ++x) {
                for (u32 c = 0; c < 4; ++c) {
                    u32 sum = 0;
                    for (u32 dy = 0; dy < 2; ++dy) {
                        for (u32 dx = 0; dx < 2; ++dx) {
                            const u32 sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                            sum += prev[(usize(sy) * w + sx) * 4 + c];
                        }
                    }
                    next[(usize(y) * nw + x) * 4 + c] = u8((sum + 2) / 4);
                }
            }
        }
        all.insert(all.end(), next.begin(), next.end());
        prev = std::move(next);
        w = nw, h = nh;
    }
    device.uploadTexture(t, all);
    return t;
}

} // namespace ox::render::translucency
