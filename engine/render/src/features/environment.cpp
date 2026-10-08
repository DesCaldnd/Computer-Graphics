// Built-in features "Environment" (IBL generation + sky constants) and "Sky" (background pass).
#include "../renderer_impl.hpp"

#include <oxwald/core/hash.hpp>
#include <oxwald/core/log.hpp>

#include <bit>
#include <cstdlib>
#include <glm/gtc/packing.hpp>
#include <cmath>
#include <cstring>

namespace ox::render {

namespace {

constexpr u32 kCaptureSize = 128;
constexpr u32 kBrdfLutSize = 128;

// Split-sum DFG LUT (x = ∫(1 - Fc)·Gvis, y = ∫Fc·Gvis, height-correlated Smith GGX), u = NdotV, v = perceptual
// roughness, RGBA16F. Computed once per process on the CPU (≈10 ms): deterministic and independent of compute
// storage-image support. Mirrors render/ibl/brdf_lut.comp.
const std::vector<u8>& brdfLutData() {
    static const std::vector<u8> data = [] {
        constexpr u32 n = kBrdfLutSize, samples = 128;
        std::vector<u8> out(usize(n) * n * 8);
        auto radical = [](u32 b) {
            b = (b << 16u) | (b >> 16u);
            b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
            b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
            b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
            b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
            return f32(b) * 2.3283064365386963e-10f;
        };
        for (u32 y = 0; y < n; ++y) {
            const f32 rough = std::max((f32(y) + 0.5f) / f32(n), 0.045f);
            const f32 a = rough * rough, a2 = a * a;
            for (u32 x = 0; x < n; ++x) {
                const f32 NdotV = (f32(x) + 0.5f) / f32(n);
                const glm::vec3 V{std::sqrt(1.0f - NdotV * NdotV), 0.0f, NdotV};
                f32 A = 0.0f, B = 0.0f;
                for (u32 i = 0; i < samples; ++i) {
                    const f32 u1 = f32(i) / f32(samples), u2 = radical(i);
                    const f32 phi = 2.0f * kPi * u1;
                    const f32 cosT = std::sqrt((1.0f - u2) / (1.0f + (a2 - 1.0f) * u2));
                    const f32 sinT = std::sqrt(1.0f - cosT * cosT);
                    const glm::vec3 H{sinT * std::cos(phi), sinT * std::sin(phi), cosT};
                    const f32 VdotH = glm::dot(V, H);
                    const glm::vec3 L = 2.0f * VdotH * H - V;
                    const f32 NdotL = std::clamp(L.z, 0.0f, 1.0f);
                    if (NdotL <= 0.0f) continue;
                    const f32 NdotH = std::max(H.z, 1e-5f);
                    const f32 vh = std::clamp(VdotH, 0.0f, 1.0f);
                    const f32 lv = NdotL * std::sqrt(NdotV * NdotV * (1.0f - a2) + a2);
                    const f32 ll = NdotV * std::sqrt(NdotL * NdotL * (1.0f - a2) + a2);
                    const f32 vis = 0.5f / std::max(lv + ll, 1e-5f) * 4.0f * NdotL * vh / NdotH;
                    const f32 fc = std::pow(1.0f - vh, 5.0f);
                    A += (1.0f - fc) * vis;
                    B += fc * vis;
                }
                const u16 px[4] = {glm::packHalf1x16(A / samples), glm::packHalf1x16(B / samples), 0, glm::packHalf1x16(1.0f)};
                std::memcpy(&out[(usize(y) * n + x) * 8], px, 8);
            }
        }
        return out;
    }();
    return data;
}

// Sets the sky/IBL view constants and (re)generates the environment maps when the environment changed:
// capture (sky or HDRI) → mips → GGX prefiltered cube + SH9 irradiance; BRDF LUT once.
CVar<float> cvLdrSkyLuminance("r.Sky.LdrLuminance", 5000.0f,
                               "cd/m² of a white texel of an 8-bit (LDR) skybox (EnvironmentComponent::ldrSkyLuminance = 0)",
                               0.0f, 100000.0f);

bool isHdrFormat(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return true;
    default: return false;
    }
}

class EnvironmentFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "Environment"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::PreDepth); }
    i32 order() const override { return -1000; }
    std::vector<std::string_view> provides() const override { return {"view.prefilteredCube", "view.irradianceSH"}; }
    std::vector<std::string> cvarNames() const override { return {"r.IBL", "r.IBL.Intensity", "r.IBL.Resolution"}; }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        m_capture = createComputePipeline(dev, "render.ibl.capture", "render/ibl/capture.comp");
        m_prefilter = createComputePipeline(dev, "render.ibl.prefilter", "render/ibl/prefilter.comp");
        m_sh = createComputePipeline(dev, "render.ibl.sh", "render/ibl/sh.comp");
        rhi::TextureDesc lut;
        lut.name = "render.brdfLut";
        lut.format = VK_FORMAT_R16G16B16A16_SFLOAT; // rg16f is not a guaranteed storage format (Metal)
        lut.width = lut.height = kBrdfLutSize;
        lut.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        m_lut = dev.createTexture(lut);
        dev.uploadTexture(m_lut, brdfLutData());
        rhi::TextureDesc cap;
        cap.name = "render.envCapture";
        cap.type = rhi::TextureType::Cube;
        cap.arrayLayers = 6;
        cap.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        cap.width = cap.height = kCaptureSize;
        cap.mipLevels = 0;
        cap.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSrc |
                    rhi::TextureUsage::TransferDst;
        m_envCube = dev.createTexture(cap);
        m_shBuffer = dev.createBuffer({9 * sizeof(glm::vec4), rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly,
                                       "render.irradianceSH"});
        return true;
    }

    void shutdown(rhi::Device& dev) override {
        for (rhi::PipelineHandle p : {m_capture, m_prefilter, m_sh}) dev.destroy(p);
        for (rhi::TextureHandle t : {m_lut, m_envCube, m_prefiltered}) {
            if (t) dev.destroy(t);
        }
        dev.destroy(m_shBuffer);
    }

    void setup(FeatureContext& ctx) override {
        FrameState& fs = frameState(ctx);
        Renderer::Impl& r = rendererImpl(ctx);
        rhi::Device& dev = ctx.device();
        GpuViewConstants& c = ctx.viewConstants();
        const RenderSnapshot& snap = ctx.snapshot();
        const RenderSettings& st = ctx.settings();

        // --- sky constants ---
        c.skyMode = 0;
        c.skyCube = kInvalidIndex;
        u64 skyKey = 0;
        if (snap.environment) {
            const EnvironmentComponent& env = snap.environment->environment;
            if (env.skybox.isValid()) {
                if (const GpuTexture* t = r.cache->texture(env.skybox); t && t->cube) {
                    c.skyMode = 1;
                    c.skyCube = t->sampledIndex;
                    skyKey = t->texture.packed();
                    // 8-bit skyboxes hold display values: scale them to sky radiance (HDR cubes are already in nits).
                    if (!isHdrFormat(dev.desc(t->texture).format)) {
                        c.skyIntensity *= env.ldrSkyLuminance > 0.0f ? env.ldrSkyLuminance : cvLdrSkyLuminance.get();
                    }
                }
            }
            if (c.skyMode == 0 && snap.environment->preetham) {
                c.skyMode = 2;
                for (u32 i = 0; i < 8; ++i) c.preetham[i] = (*snap.environment->preetham)[i];
            }
        }
        // BRDF LUT (CPU generated at init) is needed for multi-scatter compensation even without IBL.
        c.brdfLut = st.multiScatter || st.ibl ? dev.sampledIndex(m_lut) : kInvalidIndex;
        if (!st.multiScatter && !st.ibl) c.brdfLut = kInvalidIndex;

        if (!snap.environment || !st.ibl) return;

        // --- IBL ---
        const u32 res = std::bit_ceil(u32(std::clamp(st.iblResolution, 16, 1024)));
        const u32 mips = std::max(1u, u32(std::log2(f32(res))) - 1); // smallest face 4×4
        if (!m_prefiltered || m_prefilteredSize != res) {
            if (m_prefiltered) dev.destroy(m_prefiltered);
            rhi::TextureDesc d;
            d.name = "render.prefilteredEnv";
            d.type = rhi::TextureType::Cube;
            d.arrayLayers = 6;
            d.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            d.width = d.height = res;
            d.mipLevels = mips;
            d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
            m_prefiltered = dev.createTexture(d);
            m_prefilteredSize = res;
            m_prefilteredMips = mips;
            m_hash = 0;
        }
        // Environment hash: everything the capture depends on.
        u64 h = 0x9e3779b97f4a7c15ull;
        auto mix = [&](const void* p, usize n) { h = hashCombine(h, fnv1a64(std::span(static_cast<const std::byte*>(p), n))); };
        mix(&c.skyIntensity, sizeof(f32));
        mix(&c.skyMode, sizeof(u32));
        mix(&skyKey, sizeof(skyKey));
        if (snap.environment->iblKey) {
            // Throttled by the provider (world sky): only its key, not the per-frame sky constants / sun.
            mix(&*snap.environment->iblKey, sizeof(u64));
        } else {
            if (c.skyMode == 2) mix(c.preetham, sizeof(c.preetham));
            if (c.sunLight >= 0) {
                const GpuLight& sun = fs.lights[usize(c.sunLight)];
                // Quantise the sun so tiny animation steps do not regenerate every frame.
                const glm::ivec3 qd = glm::ivec3(sun.direction * 512.0f);
                const glm::ivec3 qc = glm::ivec3(glm::log2(glm::max(sun.color, glm::vec3(1e-6f))) * 32.0f);
                mix(&qd, sizeof(qd));
                mix(&qc, sizeof(qc));
            }
        }
        mix(&m_prefilteredSize, sizeof(u32));
        const u32 versions[3] = {dev.pipelineVersion(m_capture), dev.pipelineVersion(m_prefilter), dev.pipelineVersion(m_sh)};
        mix(versions, sizeof(versions));
        if (r.environmentDirty || h != m_hash) {
            m_hash = h;
            r.environmentDirty = false;
            const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();
            ctx.graph().addPass("IBL.Generate", rhi::PassType::Compute).sideEffect().execute([this, viewAddr, sceneAddr](rhi::PassContext& p) {
                generate(p, viewAddr, sceneAddr);
            });
        }
        c.prefilteredCube = dev.sampledIndex(m_prefiltered);
        c.prefilteredMips = m_prefilteredMips;
        c.irradianceSH = dev.address(m_shBuffer);
    }

private:
    void generate(rhi::PassContext& p, VkDeviceAddress viewAddr, VkDeviceAddress sceneAddr) {
        rhi::CommandList& cmd = p.cmd;
        rhi::Device& dev = p.device;
        // 1. Capture mip 0.
        cmd.transition(m_envCube, rhi::Access::StorageWriteCompute, true);
        cmd.bindPipeline(m_capture);
        struct {
            u64 view, scene;
            u32 dst, size;
        } cap{viewAddr, sceneAddr, dev.storageIndex(m_envCube, 0), kCaptureSize};
        cmd.pushConstants(cap);
        cmd.dispatch(kCaptureSize / 8, kCaptureSize / 8, 6);
        // 2. Mip chain for filtered importance sampling.
        cmd.generateMipmaps(m_envCube, rhi::Access::SampledCompute);
        // 3. Prefilter each roughness level.
        cmd.transition(m_prefiltered, rhi::Access::StorageWriteCompute, true);
        cmd.bindPipeline(m_prefilter);
        const u32 srcMips = dev.desc(m_envCube).mipLevels;
        for (u32 mip = 0; mip < m_prefilteredMips; ++mip) {
            const u32 size = std::max(m_prefilteredSize >> mip, 1u);
            struct {
                u32 src, dst, size, srcSize;
                f32 roughness;
                u32 samples;
                f32 srcMips;
            } pc{dev.sampledIndex(m_envCube), dev.storageIndex(m_prefiltered, mip), size, kCaptureSize,
                 m_prefilteredMips > 1 ? f32(mip) / f32(m_prefilteredMips - 1) : 0.0f, mip == 0 ? 1u : 128u, f32(srcMips)};
            cmd.pushConstants(pc);
            cmd.dispatch((size + 7) / 8, (size + 7) / 8, 6);
        }
        cmd.transition(m_prefiltered, rhi::Access::SampledGraphics);
        // 4. SH9 irradiance from the 16×16 mip.
        cmd.bindPipeline(m_sh);
        struct {
            u64 dst;
            u32 src, size;
            f32 lod;
        } sh{dev.address(m_shBuffer), dev.sampledIndex(m_envCube), 16, std::log2(f32(kCaptureSize) / 16.0f)};
        cmd.pushConstants(sh);
        cmd.dispatch(1);
        cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::General);
    }

    rhi::PipelineHandle m_capture, m_prefilter, m_sh;
    rhi::TextureHandle m_lut, m_envCube, m_prefiltered;
    rhi::BufferHandle m_shBuffer;
    u32 m_prefilteredSize = 0, m_prefilteredMips = 1;
    u64 m_hash = 0;
};

// Background: fullscreen at depth 0 with a read-only GREATER_OR_EQUAL test → only pixels the opaque pass left empty.
class SkyFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "Sky"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::AfterOpaque); }
    i32 order() const override { return -1000; }
    std::string_view exclusiveGroup() const override { return "Sky"; }
    std::vector<std::string> cvarNames() const override { return {"r.Sky"}; }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps&) const override { return s.sky; }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::GraphicsPipelineDesc d;
        d.name = "render.sky";
        d.vertex = fullscreenVertexShader();
        d.fragment = rhi::ShaderStageDesc::file("render/passes/sky.frag");
        d.colorFormats = {formats::kSceneColor};
        d.depthFormat = formats::kDepth;
        d.depth = {true, false, VK_COMPARE_OP_GREATER_OR_EQUAL};
        m_pipeline = ctx.device.createGraphicsPipeline(d);
        return true;
    }
    void shutdown(rhi::Device& dev) override { dev.destroy(m_pipeline); }

    void setup(FeatureContext& ctx) override {
        if (!ctx.snapshot().environment) return;
        FrameResources& R = ctx.resources();
        const rhi::RGTexture hdr = R.texture(res::kSceneColorHDR), depth = R.texture(res::kDepth);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        ctx.graph()
            .addPass("Sky")
            .color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD)
            .depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true)
            .execute([this, view, scene](rhi::PassContext& p) {
                const u64 pc[3] = {view, scene, 0};
                drawFullscreen(p.cmd, m_pipeline, pc, 20);
            });
    }

private:
    rhi::PipelineHandle m_pipeline;
};

} // namespace

std::unique_ptr<IRenderFeature> makeEnvironmentFeature() { return std::make_unique<EnvironmentFeature>(); }
std::unique_ptr<IRenderFeature> makeSkyFeature() { return std::make_unique<SkyFeature>(); }

} // namespace ox::render
