// "Water" (caustics + Gerstner water surface) and "Underwater" (post effect when the camera is below the surface).
#include "translucency_internal.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/profile.hpp>

#include <cmath>
#include <random>

namespace ox::render {

namespace {

using S = Scalability;

CVar<bool> cvWater("r.Water", true, "Render water surfaces");
CVar<int> cvGrid("r.Water.GridResolution", 256, "Water grid cells per side", S::Shading, {96, 160, 256, 384});
CVar<int> cvSsrSteps("r.Water.SSRSteps", 12, "Water screen-space reflection steps (0 = environment only)",
                     S::Reflections, {0, 8, 12, 20});
CVar<bool> cvCaustics("r.Water.Caustics", true, "Projected caustics on underwater geometry", S::Effects,
                      {false, true, true, true});
CVar<bool> cvUnderwater("r.Water.Underwater", true, "Underwater fog / wobble when the camera is below the surface");
CVar<float> cvMaxExtent("r.Water.MaxExtent", 2000.0f, "Half extent (m) of the camera grid of unbounded water", 50.0f,
                        20000.0f);

struct WaterGpu {
    GerstnerParams gerstner;
    glm::vec4 rect{0.0f};
    glm::vec4 absorption{0.0f};
    glm::vec4 scatter{0.0f};
    glm::vec4 detail{0.0f};
    glm::vec4 foam{0.0f};
    glm::vec4 caustics{0.0f};
    glm::vec4 underwater{0.0f};
    glm::vec4 grid{0.0f};
    u32 normalMap = kInvalidIndex;
    u32 causticsMap = kInvalidIndex;
    u32 flags = 0;
    u32 pad = 0;
};
static_assert(sizeof(WaterGpu) == 528 + 8 * 16 + 16);

struct WaterPush {
    u64 view = 0, scene = 0, water = 0;
    u32 refraction = kInvalidIndex;
    u32 refractionMips = 1;
    u32 sceneDepth = kInvalidIndex;
    u32 planar = kInvalidIndex;
    u32 hdr = kInvalidIndex;
    u32 shadowMask = kInvalidIndex;
    u32 fog = kInvalidIndex;
    u32 ssrSteps = 0;
    u32 gridCells = 0;
    u32 flags = 0;
};
static_assert(sizeof(WaterPush) == 64);

// --- procedural textures ----------------------------------------------------------------------------------

f32 hash2(i32 x, i32 y, u32 seed) {
    u32 h = u32(x) * 374761393u + u32(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return f32((h ^ (h >> 16)) & 0xffffffu) / f32(0xffffff);
}

// Tileable value noise with integer period.
f32 valueNoise(f32 x, f32 y, i32 period, u32 seed) {
    const i32 x0 = i32(std::floor(x)), y0 = i32(std::floor(y));
    const f32 fx = x - f32(x0), fy = y - f32(y0);
    auto at = [&](i32 ix, i32 iy) { return hash2(((ix % period) + period) % period, ((iy % period) + period) % period, seed); };
    const f32 sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const f32 a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sx;
    const f32 b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sx;
    return a + (b - a) * sy;
}

// Detail normal map (rgb, tileable sum of integer-frequency waves) + foam noise (alpha, tileable fbm).
std::vector<u8> makeWaterNormalMap(u32 size) {
    struct Wave {
        f32 kx, ky, amp, phase;
    };
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> freq(-9, 9);
    std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
    std::vector<Wave> waves;
    while (waves.size() < 28) {
        const int kx = freq(rng), ky = freq(rng);
        if (kx == 0 && ky == 0) continue;
        const f32 k = std::sqrt(f32(kx * kx + ky * ky));
        waves.push_back({f32(kx), f32(ky), 1.0f / (k * k), unit(rng) * 6.2831853f});
    }
    std::vector<u8> px(usize(size) * size * 4);
    const f32 twoPi = 6.2831853f;
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 u = f32(x) / f32(size), v = f32(y) / f32(size);
            f32 dx = 0.0f, dy = 0.0f;
            for (const Wave& w : waves) {
                const f32 c = std::cos(twoPi * (w.kx * u + w.ky * v) + w.phase);
                dx += w.amp * twoPi * w.kx * c;
                dy += w.amp * twoPi * w.ky * c;
            }
            glm::vec3 n = glm::normalize(glm::vec3(-dx * 0.18f, -dy * 0.18f, 1.0f));
            f32 foam = 0.0f, amp = 0.5f;
            for (i32 o = 0; o < 4; ++o) {
                const i32 period = 8 << o;
                foam += valueNoise(u * f32(period), v * f32(period), period, 7u + u32(o)) * amp;
                amp *= 0.5f;
            }
            const usize i = (usize(y) * size + x) * 4;
            px[i + 0] = u8(std::clamp(n.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[i + 1] = u8(std::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[i + 2] = u8(std::clamp(n.z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[i + 3] = u8(std::clamp(foam / 0.9375f, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return px;
}

// Caustic network: tileable Worley F2 - F1 (bright thin lines on cell borders).
std::vector<u8> makeCausticsMap(u32 size) {
    constexpr i32 kCells = 6;
    std::vector<u8> px(usize(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 u = f32(x) / f32(size) * kCells, v = f32(y) / f32(size) * kCells;
            const i32 cx = i32(std::floor(u)), cy = i32(std::floor(v));
            f32 f1 = 1e9f, f2 = 1e9f;
            for (i32 oy = -1; oy <= 1; ++oy) {
                for (i32 ox = -1; ox <= 1; ++ox) {
                    const i32 nx = cx + ox, ny = cy + oy;
                    const i32 wx = ((nx % kCells) + kCells) % kCells, wy = ((ny % kCells) + kCells) % kCells;
                    const f32 px0 = f32(nx) + 0.15f + 0.7f * hash2(wx, wy, 11u);
                    const f32 py0 = f32(ny) + 0.15f + 0.7f * hash2(wx, wy, 23u);
                    const f32 d = std::sqrt((px0 - u) * (px0 - u) + (py0 - v) * (py0 - v));
                    if (d < f1) {
                        f2 = f1;
                        f1 = d;
                    } else if (d < f2) {
                        f2 = d;
                    }
                }
            }
            const f32 edge = std::clamp(1.0f - (f2 - f1) / 0.4f, 0.0f, 1.0f);
            const u8 c = u8(std::pow(edge, 2.2f) * 255.0f + 0.5f);
            const usize i = (usize(y) * size + x) * 4;
            px[i + 0] = px[i + 1] = px[i + 2] = c;
            px[i + 3] = 255;
        }
    }
    return px;
}

struct WaterTextures {
    rhi::TextureHandle normal, caustics;
    void create(rhi::Device& dev) {
        if (!normal) normal = translucency::createTexture2D(dev, "water.detailNormal", 256, 256, makeWaterNormalMap(256));
        if (!caustics) caustics = translucency::createTexture2D(dev, "water.caustics", 256, 256, makeCausticsMap(256));
    }
    void destroy(rhi::Device& dev) {
        if (normal) dev.destroy(normal);
        if (caustics) dev.destroy(caustics);
        normal = caustics = {};
    }
};

// Uploads one WaterGpu per surface of the snapshot.
std::vector<VkDeviceAddress> uploadWater(FeatureContext& ctx, const TranslucencySnapshot& data, const WaterTextures& tex,
                                         std::vector<f32>* ampSums = nullptr) {
    std::vector<VkDeviceAddress> out;
    rhi::Device& dev = ctx.device();
    const glm::vec3 cam = ctx.view().camera().position();
    const u32 cells = u32(std::max(i32(cvGrid), 8));
    for (const SnapshotWater& w : data.water) {
        WaterGpu g;
        g.gerstner = w.params;
        const bool unbounded = w.size.x <= 0.0f || w.size.y <= 0.0f;
        g.rect = {w.center, unbounded ? glm::vec2(0.0f) : w.size * 0.5f};
        g.absorption = {w.look.absorption, w.look.refractionStrength};
        g.scatter = {w.look.scatterColor, w.look.roughness};
        const f32 ampSum = gerstnerAmplitudeSum(w.params);
        g.detail = {w.look.detailNormalStrength, w.look.detailScale, w.look.detailSpeed, ampSum};
        g.foam = {w.look.shoreFoamDistance, w.look.crestFoam, w.look.foamIntensity,
                  gerstnerHeight(w.params, {cam.x, cam.z}, w.params.info.z, 3)};
        g.caustics = {w.look.causticsIntensity, w.look.causticsScale, w.look.causticsFalloff, 0.0f};
        g.underwater = {w.look.underwaterColor, w.look.underwaterDensity};
        // Grid centred on the camera (clamped into bounded rectangles), cells growing with the distance.
        glm::vec2 centre(cam.x, cam.z);
        f32 extent = cvMaxExtent;
        if (!unbounded) {
            const glm::vec2 h = w.size * 0.5f;
            centre = glm::clamp(centre, w.center - h, w.center + h);
            const glm::vec2 far = glm::max(glm::abs(centre - (w.center - h)), glm::abs(centre - (w.center + h)));
            extent = std::max(far.x, far.y) + 0.01f;
        }
        constexpr f32 kNearCell = 0.1f; // metres
        const f32 alpha = std::clamp(kNearCell * f32(cells) / (2.0f * extent), 0.02f, 1.0f);
        const f32 snap = std::max(kNearCell, 1e-3f);
        g.grid = {std::floor(centre.x / snap) * snap, std::floor(centre.y / snap) * snap, extent, alpha};
        g.normalMap = dev.sampledIndex(tex.normal);
        if (w.look.normalMap.isValid()) {
            if (const GpuTexture* t = ctx.renderer().resources().texture(w.look.normalMap); t && t->texture) {
                g.normalMap = t->sampledIndex;
            }
        }
        g.causticsMap = dev.sampledIndex(tex.caustics);
        g.flags = unbounded ? 1u : 0u;
        out.push_back(ctx.upload(std::span<const WaterGpu>(&g, 1)));
        if (ampSums) ampSums->push_back(ampSum);
    }
    return out;
}

class WaterFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "Water"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::AfterOpaque, InjectionPoint::Translucency); }
    i32 order() const override { return -50; } // caustics before the refraction copy, surface before transparents
    std::vector<std::string> cvarNames() const override {
        return {"r.Water", "r.Water.GridResolution", "r.Water.SSRSteps", "r.Water.Caustics", "r.Water.MaxExtent"};
    }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvWater; }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        rhi::GraphicsPipelineDesc d;
        d.name = "water.surface";
        d.vertex = rhi::ShaderStageDesc::file("render/translucency/water.vert");
        d.fragment = rhi::ShaderStageDesc::file("render/translucency/water.frag", translucency::contractDefines());
        d.colorFormats = {formats::kSceneColor};
        d.depthFormat = formats::kDepth;
        d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
        d.raster.cullMode = VK_CULL_MODE_NONE;
        d.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        m_surface = dev.createGraphicsPipeline(d);
        rhi::BlendState mul;
        mul.enable = true;
        mul.srcColor = VK_BLEND_FACTOR_DST_COLOR;
        mul.dstColor = VK_BLEND_FACTOR_ZERO;
        mul.srcAlpha = VK_BLEND_FACTOR_ZERO;
        mul.dstAlpha = VK_BLEND_FACTOR_ONE;
        m_caustics = createFullscreenPipeline(dev, "water.caustics", "render/translucency/caustics.frag", {formats::kSceneColor},
                                              {mul});
        m_source = translucency::createSourcePipeline(dev);
        m_textures.create(dev);
        return true;
    }
    void shutdown(rhi::Device& dev) override {
        dev.destroy(m_surface);
        dev.destroy(m_caustics);
        dev.destroy(m_source);
        m_textures.destroy(dev);
    }

    void setup(FeatureContext& ctx) override {
        OX_PROFILE_ZONE();
        const TranslucencySnapshot* data = ctx.snapshot().findExtension<TranslucencySnapshot>();
        if (!data || data->water.empty()) return;
        const std::vector<VkDeviceAddress> waters = uploadWater(ctx, *data, m_textures);
        FrameResources& R = ctx.resources();
        rhi::RenderGraph& g = ctx.graph();
        const rhi::RGTexture hdr = R.texture(res::kSceneColorHDR), depth = R.texture(res::kDepth);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        FrameState* fs = &frameState(ctx);

        if (ctx.point() == InjectionPoint::AfterOpaque) {
            if (!cvCaustics || ctx.viewConstants().sunLight < 0) return;
            const rhi::RGTexture mask = R.texture(res::kShadowMask);
            rhi::PassBuilder pb = g.addPass("Water.Caustics");
            pb.read(depth, rhi::Access::SampledFragment).color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD);
            if (mask.valid()) pb.read(mask, rhi::Access::SampledFragment);
            pb.execute([=, this](rhi::PassContext& p) {
                WaterPush pc;
                pc.view = view, pc.scene = scene;
                pc.sceneDepth = p.sampledIndex(depth);
                pc.shadowMask = mask.valid() ? p.sampledIndex(mask) : kInvalidIndex;
                for (VkDeviceAddress w : waters) {
                    pc.water = w;
                    drawFullscreen(p.cmd, m_caustics, &pc, sizeof(pc));
                }
                fs->stats->drawCalls += u32(waters.size());
            });
            return;
        }

        const translucency::RefractionSources src = translucency::ensureSources(ctx, m_source);
        const rhi::RGTexture planar = R.texture(res::kPlanarReflection);
        rhi::PassBuilder pb = g.addPass("Water.Surface");
        pb.color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD).depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD);
        pb.read(src.color, rhi::Access::SampledFragment).read(src.depth, rhi::Access::SampledFragment);
        if (planar.valid()) pb.read(planar, rhi::Access::SampledFragment);
        const rhi::RGTexture fog = translucency::declareLightingReads(pb, R);
        const u32 cells = u32(std::max(i32(cvGrid), 8));
        const u32 steps = u32(std::max(i32(cvSsrSteps), 0));
        const u32 mips = src.mips;
        pb.execute([=, this](rhi::PassContext& p) {
            WaterPush pc;
            pc.view = view, pc.scene = scene;
            pc.refraction = p.sampledIndex(src.color);
            pc.refractionMips = mips;
            pc.sceneDepth = p.sampledIndex(src.depth);
            pc.planar = planar.valid() ? p.sampledIndex(planar) : kInvalidIndex;
            pc.fog = fog.valid() ? p.sampledIndex(fog) : kInvalidIndex;
            pc.ssrSteps = steps;
            pc.gridCells = cells;
            p.cmd.bindPipeline(m_surface);
            for (VkDeviceAddress w : waters) {
                pc.water = w;
                p.cmd.pushConstants(pc);
                p.cmd.draw(cells * cells * 6);
            }
            fs->stats->drawCalls += u32(waters.size());
            fs->stats->triangles += u64(cells) * cells * 2 * waters.size();
        });
    }

private:
    rhi::PipelineHandle m_surface, m_caustics, m_source;
    WaterTextures m_textures;
};

class UnderwaterFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "Underwater"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Translucency); }
    i32 order() const override { return 900; } // after transparents and particles
    std::vector<std::string> cvarNames() const override { return {"r.Water.Underwater"}; }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvWater && cvUnderwater; }

    bool initialize(FeatureInitContext& ctx) override {
        m_pipeline = createFullscreenPipeline(ctx.device, "water.underwater", "render/translucency/underwater.frag",
                                              {formats::kSceneColor}, {}, translucency::contractDefines());
        m_textures.create(ctx.device);
        return true;
    }
    void shutdown(rhi::Device& dev) override {
        dev.destroy(m_pipeline);
        m_textures.destroy(dev);
    }

    void setup(FeatureContext& ctx) override {
        const TranslucencySnapshot* data = ctx.snapshot().findExtension<TranslucencySnapshot>();
        if (!data || data->water.empty()) return;
        // CPU pre-test: only surfaces whose wave crests can be above the camera.
        const glm::vec3 cam = ctx.view().camera().position();
        i32 index = -1;
        for (usize i = 0; i < data->water.size(); ++i) {
            const SnapshotWater& w = data->water[i];
            const bool bounded = w.size.x > 0.0f && w.size.y > 0.0f;
            if (bounded && (std::abs(cam.x - w.center.x) > w.size.x * 0.5f || std::abs(cam.z - w.center.y) > w.size.y * 0.5f)) continue;
            if (cam.y < gerstnerHeight(w.params, {cam.x, cam.z}, w.params.info.z, 3)) {
                index = i32(i);
                break;
            }
        }
        if (index < 0) return;
        TranslucencySnapshot one;
        one.water.push_back(data->water[usize(index)]);
        const VkDeviceAddress water = uploadWater(ctx, one, m_textures).front();
        FrameResources& R = ctx.resources();
        const rhi::RGTexture hdr = R.texture(res::kSceneColorHDR), depth = R.texture(res::kDepth);
        const rhi::RGTexture out = ctx.graph().createTexture(
            translucency::textureDesc(formats::kSceneColor, ctx.renderExtent(), "SceneColorHDR.Underwater"));
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        ctx.graph()
            .addPass("Water.Underwater")
            .read(hdr, rhi::Access::SampledFragment)
            .read(depth, rhi::Access::SampledFragment)
            .color(out, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
            .execute([=, this](rhi::PassContext& p) {
                WaterPush pc;
                pc.view = view, pc.scene = scene, pc.water = water;
                pc.hdr = p.sampledIndex(hdr);
                pc.sceneDepth = p.sampledIndex(depth);
                drawFullscreen(p.cmd, m_pipeline, &pc, sizeof(pc));
            });
        R.setTexture(res::kSceneColorHDR, out);
    }

private:
    rhi::PipelineHandle m_pipeline;
    WaterTextures m_textures;
};

} // namespace

std::unique_ptr<IRenderFeature> makeWaterFeature() { return std::make_unique<WaterFeature>(); }
std::unique_ptr<IRenderFeature> makeUnderwaterFeature() { return std::make_unique<UnderwaterFeature>(); }

} // namespace ox::render
