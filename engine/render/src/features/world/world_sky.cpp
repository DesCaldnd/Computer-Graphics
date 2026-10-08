// "WorldSky" (exclusive group "Sky", priority above the core analytic sky): physically scaled Preetham sky with
// twilight / night, sun disc with limb darkening, moon with phase, rotating star field; radiance cube of the dome for
// the IBL (refreshed when the sun / moon moved by r.Sky.IBLUpdateDegrees, which also keys the core IBL regeneration
// through SnapshotEnvironment::iblKey) and aerial perspective. Without a WorldSnapshot sky it draws the core sky.
#include "world_internal.hpp"

#if OX_RENDER_HAS_WORLD

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

#include <bit>
#include <cmath>
#include <cstring>

namespace ox::render::worldfx {

namespace {

constexpr u32 kSkySunDisc = 1u, kSkyMoon = 2u, kSkyStars = 4u;

struct WorldSkyGpu {
    glm::vec4 preetham[8];
    glm::vec4 sunDir;
    glm::vec4 sunColor;
    glm::vec4 moonDir;
    glm::vec4 moonColor;
    glm::vec4 starsRot;
    glm::vec4 params;
    glm::vec4 night;
    glm::vec4 aerial;
    u32 flags, skyCube, pad0, pad1;
};
static_assert(sizeof(WorldSkyGpu) == 16 * 16 + 16);

f32 elevationDeg(glm::vec3 d) { return glm::degrees(std::asin(std::clamp(glm::normalize(d).y, -1.0f, 1.0f))); }

WorldSkyGpu buildSky(const WorldSkySnapshot& s, f64 time, const WorldCVars& cv, u32 cube) {
    WorldSkyGpu g{};
    std::memcpy(g.preetham, &s.preetham, sizeof(g.preetham));
    const glm::vec3 sun = glm::normalize(s.sunDirection);
    const f32 sunElev = elevationDeg(sun);
    g.sunDir = glm::vec4(sun, glm::radians(std::max(s.sunAngularDiameterDeg, 0.05f)) * 0.5f);
    g.sunColor = glm::vec4(s.sunLight.color * s.sunLight.illuminance * s.sunDiscIntensity, sunElev);
    g.moonDir = glm::vec4(glm::normalize(s.moonDirection), glm::radians(std::max(s.moonAngularDiameterDeg, 0.05f)) * 0.5f);
    g.moonColor = glm::vec4(s.moonLight.color * s.moonLight.illuminance * s.moonIntensity, s.moonPhase.illuminatedFraction);
    g.starsRot = {s.starsRotation.x, s.starsRotation.y, s.starsRotation.z, s.starsRotation.w};
    // Preetham is only valid for a sun above the horizon (the model clamps it to ~1°): below, the twilight sky loses
    // about one decade of luminance per 2.5° of solar depression.
    const f32 day = sunElev >= 1.0f ? 1.0f : std::pow(10.0f, (sunElev - 1.0f) / 2.5f);
    g.params = {s.skyIntensity, s.stars ? s.starsIntensity : 0.0f, f32(std::fmod(time, 3600.0)), day < 1e-6f ? 0.0f : day};
    g.night = {0.40f * 0.004f, 0.50f * 0.004f, 0.80f * 0.004f, 0.06f};
    g.aerial = {cv.aerialDensity, 1.0f / 8000.0f, 2.0e-6f * cv.aerialDensity, 1.0f};
    g.flags = (s.sunDisc ? kSkySunDisc : 0u) | (s.moon ? kSkyMoon : 0u) | (s.stars ? kSkyStars : 0u);
    g.skyCube = cube;
    return g;
}

struct SkyViewState final : IFeatureViewState {
    VkDeviceAddress sky = 0;
    u64 frame = ~0ull;
};

class WorldSkyFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "WorldSky"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::PreDepth, InjectionPoint::AfterOpaque); }
    // PreDepth: before the core Environment (-1000) so the IBL capture sees this frame's cube; AfterOpaque: after the
    // world geometry, where the core sky would run.
    i32 order() const override { return -1100; }
    std::string_view exclusiveGroup() const override { return "Sky"; }
    i32 priority() const override { return 100; }
    std::vector<std::string_view> provides() const override { return {res::kSceneColorHDR}; }
    std::vector<std::string> cvarNames() const override { return worldSkyCVarNames(); }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps&) const override { return s.sky; }

    bool initialize(FeatureInitContext& ctx) override {
        m_coreSky = skyPipeline(ctx.device, true); // world pipelines are created when a world sky first appears
        return true;
    }

    void shutdown(rhi::Device& dev) override {
        for (rhi::PipelineHandle p : {m_sky, m_coreSky, m_cubePipe, m_aerial}) {
            if (p) dev.destroy(p);
        }
        if (m_cube) dev.destroy(m_cube);
    }

    void setup(FeatureContext& ctx) override {
        const WorldSnapshot* world = ctx.snapshot().findExtension<WorldSnapshot>();
        const bool active = world && world->sky.valid;
        SkyViewState& vs = ctx.viewState<SkyViewState>();
        const WorldCVars cv = WorldCVars::read();
        if (ctx.point() == InjectionPoint::PreDepth) {
            vs.sky = 0;
            if (!active) return;
            rhi::Device& dev = ctx.device();
            if (!m_sky) {
                m_sky = skyPipeline(dev, false);
                m_cubePipe = createComputePipeline(dev, "world.sky.cube", "render/world/sky_cube.comp");
                m_aerial = createComputePipeline(dev, "world.sky.aerial", "render/world/aerial.comp");
            }
            ensureCube(dev, u32(std::clamp(cv.skyCubeSize, 16, 1024)));
            const WorldSkyGpu g = buildSky(world->sky, world->time, cv, dev.sampledIndex(m_cube));
            vs.sky = ctx.upload(std::span<const WorldSkyGpu>(&g, 1));
            vs.frame = ctx.stats().frame;
            const u64 key = hashCombine(worldSkyIblKey(world->sky, cv.iblUpdateDegrees), m_cubeSize);
            if (key != m_cubeKey || m_cubeVersion != dev.pipelineVersion(m_cubePipe)) {
                m_cubeKey = key;
                m_cubeVersion = dev.pipelineVersion(m_cubePipe);
                const VkDeviceAddress sky = vs.sky;
                const rhi::TextureHandle cube = m_cube;
                const u32 size = m_cubeSize;
                const rhi::PipelineHandle pipe = m_cubePipe;
                ctx.graph().addPass("WorldSky.Cube", rhi::PassType::Compute).sideEffect().execute([sky, cube, size, pipe](rhi::PassContext& p) {
                    p.cmd.memoryBarrier(rhi::Access::SampledGraphics, rhi::Access::StorageWriteCompute);
                    p.cmd.transition(cube, rhi::Access::StorageWriteCompute, true);
                    p.cmd.bindPipeline(pipe);
                    struct {
                        u64 sky;
                        u32 dst, size;
                    } pc{sky, p.device.storageIndex(cube, 0), size};
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatch((size + 7) / 8, (size + 7) / 8, 6);
                    p.cmd.transition(cube, rhi::Access::SampledCompute);
                    p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::SampledGraphics);
                });
            }
            return;
        }

        // --- AfterOpaque ---
        FrameResources& R = ctx.resources();
        const rhi::RGTexture hdr = R.texture(res::kSceneColorHDR), depth = R.texture(res::kDepth);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        if (!active || vs.frame != ctx.stats().frame || !vs.sky) {
            // No world sky: behave like the core "Sky" feature.
            if (!ctx.snapshot().environment) return;
            const rhi::PipelineHandle pipe = m_coreSky;
            ctx.graph()
                .addPass("Sky")
                .color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD)
                .depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true)
                .execute([pipe, view, scene](rhi::PassContext& p) {
                    const u64 pc[3] = {view, scene, 0};
                    drawFullscreen(p.cmd, pipe, pc, 20);
                });
            return;
        }
        // Make the world sky the environment of every sky consumer (IBL capture, translucency, volumetrics).
        GpuViewConstants& c = ctx.viewConstants();
        c.skyMode = 1;
        c.skyCube = ctx.device().sampledIndex(m_cube);

        const VkDeviceAddress sky = vs.sky;
        const f32 pixelAngle = ctx.view().camera().verticalFov / f32(std::max(ctx.renderExtent().height, 1u));
        const rhi::PipelineHandle skyPipe = m_sky;
        ctx.graph()
            .addPass("WorldSky")
            .color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD)
            .depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, true)
            .execute([skyPipe, view, scene, sky, pixelAngle](rhi::PassContext& p) {
                struct {
                    u64 view, scene, sky;
                    f32 pixelAngle;
                    u32 unused;
                } pc{view, scene, sky, pixelAngle, 0};
                drawFullscreen(p.cmd, skyPipe, &pc, sizeof(pc));
            });

        // Aerial perspective, unless froxel volumetric fog already integrates the atmosphere.
        const bool volumetric = R.texture(res::kVolumetricFog).valid() || c.volumetricFogGrid.w > 0.0f;
        if (cv.aerialPerspective && !volumetric) {
            const Extent2D e = ctx.renderExtent();
            const rhi::PipelineHandle aerial = m_aerial;
            ctx.graph()
                .addPass("WorldSky.Aerial", rhi::PassType::Compute)
                .read(depth, rhi::Access::SampledCompute)
                .write(hdr, rhi::Access::StorageWriteCompute)
                .execute([aerial, view, scene, sky, depth, hdr, e](rhi::PassContext& p) {
                    struct {
                        u64 view, scene, sky;
                        u32 depth, color;
                    } pc{view, scene, sky, p.sampledIndex(depth), p.storageIndex(hdr)};
                    p.cmd.bindPipeline(aerial);
                    p.cmd.pushConstants(pc);
                    p.cmd.dispatchThreads(e.width, e.height);
                });
        }
    }

private:
    static rhi::PipelineHandle skyPipeline(rhi::Device& dev, bool core) {
        rhi::GraphicsPipelineDesc d;
        d.name = core ? "world.sky.core" : "world.sky";
        d.vertex = fullscreenVertexShader();
        d.fragment = rhi::ShaderStageDesc::file(core ? "render/passes/sky.frag" : "render/world/sky.frag");
        d.colorFormats = {formats::kSceneColor};
        d.depthFormat = formats::kDepth;
        d.depth = {true, false, VK_COMPARE_OP_GREATER_OR_EQUAL};
        return dev.createGraphicsPipeline(d);
    }

    void ensureCube(rhi::Device& dev, u32 size) {
        size = std::bit_ceil(size);
        if (m_cube && m_cubeSize == size) return;
        if (m_cube) dev.destroy(m_cube);
        rhi::TextureDesc d;
        d.name = "world.skyCube";
        d.type = rhi::TextureType::Cube;
        d.arrayLayers = 6;
        d.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        d.width = d.height = size;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
        m_cube = dev.createTexture(d);
        m_cubeSize = size;
        m_cubeKey = 0;
    }

    rhi::PipelineHandle m_sky, m_coreSky, m_cubePipe, m_aerial;
    rhi::TextureHandle m_cube;
    u32 m_cubeSize = 0;
    u64 m_cubeKey = 0;
    u32 m_cubeVersion = 0;
};

} // namespace

std::unique_ptr<IRenderFeature> makeWorldSkyFeature() { return std::make_unique<WorldSkyFeature>(); }

} // namespace ox::render::worldfx

#endif // OX_RENDER_HAS_WORLD
