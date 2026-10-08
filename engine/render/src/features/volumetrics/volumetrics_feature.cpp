// Volumetrics feature: froxel fog (inject → scatter + temporal → integrate), volumetric clouds (trace with
// checkerboard updates → temporal reconstruction) and the composite onto SceneColorHDR. See
// include/oxwald/render/features/volumetrics/volumetrics.hpp and docs/dev/modules/render.md (Volumetrics).

#include "volumetrics_gpu.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/render/features/volumetrics/volumetrics.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/rhi/command_list.hpp>
#include <oxwald/rhi/device.hpp>

#include <glm/gtc/matrix_access.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ox::render::volumetrics {

namespace {

using S = Scalability;

// --- cvars (Volumetrics scalability group: Low / Medium / High / Ultra) ---
CVar<bool> cvFog("r.VolumetricFog", true, "Froxel volumetric fog (height fog, fog volumes, light shafts)");
CVar<int> cvGridX("r.VolumetricFog.GridSizeX", 160, "Froxel grid width", S::Volumetrics, {96, 128, 160, 240});
CVar<int> cvGridY("r.VolumetricFog.GridSizeY", 90, "Froxel grid height", S::Volumetrics, {54, 72, 90, 135});
CVar<int> cvGridZ("r.VolumetricFog.GridSizeZ", 64, "Froxel depth slices", S::Volumetrics, {32, 48, 64, 128});
CVar<float> cvDistance("r.VolumetricFog.Distance", 128.0f, "Froxel grid far distance (m)", S::Volumetrics,
                       {64.0f, 96.0f, 128.0f, 192.0f});
CVar<float> cvDepthScale("r.VolumetricFog.DepthDistributionScale", 32.0f,
                         "Exponential slice distribution (higher = more slices near the camera)", 0.01f, 1000.0f);
CVar<bool> cvTemporal("r.VolumetricFog.TemporalReprojection", true, "Jittered froxel samples + history blending");
CVar<float> cvHistoryWeight("r.VolumetricFog.HistoryWeight", 0.9f, "Weight of the reprojected history",
                            S::Volumetrics, {0.85f, 0.9f, 0.9f, 0.95f});
CVar<float> cvAnisotropy("r.VolumetricFog.Anisotropy", 0.6f, "Henyey-Greenstein g of the height fog", -0.95f, 0.95f);
CVar<bool> cvLocalShadows("r.VolumetricFog.LocalLightShadows", true, "Spot / point light shadows in the fog",
                          S::Volumetrics, {false, true, true, true});
CVar<bool> cvSunShadows("r.VolumetricFog.SunShadows", true, "Cascaded sun shadows in the fog (light shafts)");
CVar<bool> cvCloudShadows("r.VolumetricFog.CloudShadows", true, "Cloud layer shadows on the fog", S::Volumetrics,
                          {false, false, true, true});
CVar<int> cvMaxVolumes("r.VolumetricFog.MaxVolumes", 64, "Maximum fog volumes per view (nearest first)", 0, 1024);
CVar<float> cvSkyDistance("r.VolumetricFog.SkyDistance", 20000.0f,
                          "Distance the sky is fogged to (aerial perspective of the background)", 0.0f, 1e7f);

CVar<bool> cvClouds("r.VolumetricClouds", true, "Ray-marched volumetric clouds (CloudLayerComponent)",
                    S::Volumetrics, {false, true, true, true});
CVar<int> cvCloudDownsample("r.VolumetricClouds.Downsample", 2, "Cloud resolution divisor (1, 2, 4)", S::Volumetrics,
                            {4, 4, 2, 2});
CVar<int> cvCloudChecker("r.VolumetricClouds.Checkerboard", 16,
                         "Temporal update rate: 1 = every pixel per frame, 4 = 1/4, 16 = 1/16", S::Volumetrics,
                         {16, 16, 16, 4});
CVar<int> cvCloudSteps("r.VolumetricClouds.Steps", 64, "Primary ray march steps", S::Volumetrics, {32, 48, 64, 96});
CVar<int> cvCloudLightSteps("r.VolumetricClouds.LightSteps", 6, "Light march steps towards the sun", S::Volumetrics,
                            {4, 5, 6, 8});
CVar<float> cvCloudDistance("r.VolumetricClouds.MaxDistance", 40000.0f, "Max march distance (m)", S::Volumetrics,
                            {25000.0f, 30000.0f, 40000.0f, 50000.0f});
CVar<bool> cvCloudTemporal("r.VolumetricClouds.Temporal", true, "Temporal reconstruction (off = trace every pixel)");

constexpr u32 kBaseNoiseSize = 128;
constexpr u32 kDetailNoiseSize = 32;
constexpr u32 kWeatherSize = 512;
constexpr f32 kPlanetRadius = 6360000.0f;
constexpr f32 kCloudExtinction = 0.02f; // 1/m at density 1

// 4×4 Bayer order of the checkerboard pixel updated each frame (spreads consecutive updates apart).
constexpr u32 kBayer4[16] = {0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12};
constexpr u32 kBayer2[4] = {0, 3, 1, 2};

f32 wrapOffset(f64 v, f64 period) {
    if (period <= 0.0) return f32(v);
    return f32(v - std::floor(v / period) * period);
}

glm::vec3 wrapOffset(glm::dvec3 v, f64 period) {
    return {wrapOffset(v.x, period), wrapOffset(v.y, period), wrapOffset(v.z, period)};
}

struct ViewState final : IFeatureViewState {
    glm::vec3 prevCamera{0.0f};
    glm::vec3 prevForward{0.0f, 0.0f, -1.0f};
    f32 prevPreExposure = 0.0f;
    FroxelGrid prevGrid;
    u64 lastFogFrame = ~0ull;
    f32 cloudPrevPreExposure = 0.0f;
    u64 lastCloudFrame = ~0ull;
};

rhi::TextureDesc volumeDesc(const FroxelGrid& g, const char* name) {
    rhi::TextureDesc d;
    d.type = rhi::TextureType::Tex3D;
    d.format = formats::kVolumetricFog;
    d.width = g.x;
    d.height = g.y;
    d.depth = g.z;
    d.usage = rhi::TextureUsage::None;
    d.name = name;
    return d;
}

rhi::TextureDesc cloudDesc(u32 w, u32 h, const char* name) {
    rhi::TextureDesc d;
    d.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    d.width = std::max(w, 1u);
    d.height = std::max(h, 1u);
    d.usage = rhi::TextureUsage::None;
    d.name = name;
    return d;
}

class VolumetricsFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return kFeatureName; }
    InjectionMask injectionPoints() const override {
        return maskOf(InjectionPoint::Lighting, InjectionPoint::AfterOpaque);
    }
    i32 order() const override { return kFogOrder; }
    std::string_view exclusiveGroup() const override { return "Volumetrics"; }
    std::vector<std::string_view> provides() const override {
        return {res::kVolumetricFog, kVolumetricClouds, kVolumetricCloudsDepth};
    }
    std::vector<std::string> cvarNames() const override {
        return {"r.VolumetricFog", "r.VolumetricFog.GridSizeX", "r.VolumetricFog.GridSizeY", "r.VolumetricFog.GridSizeZ",
                "r.VolumetricFog.Distance", "r.VolumetricFog.DepthDistributionScale",
                "r.VolumetricFog.TemporalReprojection", "r.VolumetricFog.HistoryWeight", "r.VolumetricFog.Anisotropy",
                "r.VolumetricFog.LocalLightShadows", "r.VolumetricFog.SunShadows", "r.VolumetricFog.CloudShadows",
                "r.VolumetricFog.MaxVolumes", "r.VolumetricFog.SkyDistance", "r.VolumetricClouds",
                "r.VolumetricClouds.Downsample", "r.VolumetricClouds.Checkerboard", "r.VolumetricClouds.Steps",
                "r.VolumetricClouds.LightSteps", "r.VolumetricClouds.MaxDistance", "r.VolumetricClouds.Temporal"};
    }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvFog || cvClouds; }

    bool initialize(FeatureInitContext& ctx) override {
        rhi::Device& dev = ctx.device;
        m_inject = createComputePipeline(dev, "render.volumetrics.fogInject", "render/volumetrics/fog_inject.comp");
        m_scatter = createComputePipeline(dev, "render.volumetrics.fogScatter", "render/volumetrics/fog_scatter.comp");
        m_tileDepth = createComputePipeline(dev, "render.volumetrics.fogTileDepth", "render/volumetrics/fog_tile_depth.comp");
        m_integrate = createComputePipeline(dev, "render.volumetrics.fogIntegrate", "render/volumetrics/fog_integrate.comp");
        m_cloudTrace = createComputePipeline(dev, "render.volumetrics.cloudTrace", "render/volumetrics/cloud_trace.comp");
        m_cloudReconstruct =
            createComputePipeline(dev, "render.volumetrics.cloudReconstruct", "render/volumetrics/cloud_reconstruct.comp");
        m_noise = createComputePipeline(dev, "render.volumetrics.noise", "render/volumetrics/noise_gen.comp");
        rhi::BlendState blend;
        blend.enable = true;
        blend.srcColor = VK_BLEND_FACTOR_ONE;
        blend.dstColor = VK_BLEND_FACTOR_SRC_ALPHA; // dst · transmittance + in-scatter
        blend.srcAlpha = VK_BLEND_FACTOR_ZERO;
        blend.dstAlpha = VK_BLEND_FACTOR_ONE;
        m_composite = createFullscreenPipeline(dev, "render.volumetrics.composite", "render/volumetrics/composite.frag",
                                               {formats::kSceneColor}, {blend});
        return true;
    }

    void shutdown(rhi::Device& dev) override {
        for (rhi::PipelineHandle p : {m_inject, m_scatter, m_tileDepth, m_integrate, m_cloudTrace, m_cloudReconstruct, m_noise, m_composite}) {
            dev.destroy(p);
        }
        for (rhi::TextureHandle t : {m_baseNoise, m_detailNoise, m_weather}) {
            if (t) dev.destroy(t);
        }
        m_baseNoise = m_detailNoise = m_weather = {};
    }

    void setup(FeatureContext& ctx) override {
        if (ctx.point() == InjectionPoint::Lighting) setupLighting(ctx);
        else setupComposite(ctx);
    }

private:
    // --- noise (generated once, on first use) ---
    bool ensureNoise(rhi::Device& dev) {
        if (m_baseNoise) return true;
        auto make = [&](rhi::TextureType type, u32 size, const char* name) {
            rhi::TextureDesc d;
            d.type = type;
            d.format = VK_FORMAT_R8G8B8A8_UNORM;
            d.width = d.height = size;
            d.depth = type == rhi::TextureType::Tex3D ? size : 1;
            d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
            d.name = name;
            return dev.createTexture(d);
        };
        m_baseNoise = make(rhi::TextureType::Tex3D, kBaseNoiseSize, "render.volumetrics.baseNoise");
        m_detailNoise = make(rhi::TextureType::Tex3D, kDetailNoiseSize, "render.volumetrics.detailNoise");
        m_weather = make(rhi::TextureType::Tex2D, kWeatherSize, "render.volumetrics.weather");
        dev.immediateSubmit([&](rhi::CommandList& cmd) {
            cmd.bindPipeline(m_noise);
            struct {
                u32 target, size, mode, seed;
            } pc{};
            const rhi::TextureHandle textures[3] = {m_baseNoise, m_detailNoise, m_weather};
            const u32 sizes[3] = {kBaseNoiseSize, kDetailNoiseSize, kWeatherSize};
            for (u32 mode = 0; mode < 3; ++mode) {
                cmd.transition(textures[mode], rhi::Access::StorageWriteCompute, true);
                pc = {dev.storageIndex(textures[mode]), sizes[mode], mode, 17u + mode};
                cmd.pushConstants(pc);
                const u32 groups = (sizes[mode] + 3) / 4;
                cmd.dispatch(groups, groups, mode == 2 ? 1 : groups);
                cmd.transition(textures[mode], rhi::Access::SampledCompute);
            }
        });
        return true;
    }

    void setupLighting(FeatureContext& ctx) {
        const RenderSnapshot& snap = ctx.snapshot();
        const VolumetricsSnapshot* ext = snap.findExtension<VolumetricsSnapshot>();
        m_fogThisFrame = cvFog && fogActive(snap);
        m_cloudsThisFrame = cvClouds && ext && ext->clouds.has_value() && ext->clouds->coverage > 0.0f;
        if (!m_fogThisFrame && !m_cloudsThisFrame) return;
        rhi::Device& dev = ctx.device();
        if (!ensureNoise(dev)) return;
        ViewState& vs = ctx.viewState<ViewState>();
        if (m_fogThisFrame) setupFog(ctx, ext, vs);
        if (m_cloudsThisFrame) setupClouds(ctx, *ext, vs);
    }

    glm::vec3 worldWind(const VolumetricsSnapshot* ext) const { return ext && ext->hasWind ? ext->wind : glm::vec3(0.0f); }

    void setupFog(FeatureContext& ctx, const VolumetricsSnapshot* ext, ViewState& vs) {
        rhi::Device& dev = ctx.device();
        rhi::RenderGraph& graph = ctx.graph();
        FrameResources& R = ctx.resources();
        const RenderSnapshot& snap = ctx.snapshot();
        const RenderView& view = ctx.view();
        GpuViewConstants& c = ctx.viewConstants();
        const FroxelGrid grid = froxelGridFromCVars(&snap);
        const VolumetricFogComponent fogSettings = ext && ext->fog ? *ext->fog : VolumetricFogComponent{cvAnisotropy.get()};
        const f32 anisotropy = ext && ext->fog ? fogSettings.anisotropy : cvAnisotropy.get();
        const f64 time = snap.time;
        const glm::vec3 wind = worldWind(ext);
        const glm::vec3 camPos = view.camera().position();
        const glm::vec3 camFwd = -glm::normalize(glm::vec3(view.camera().world[2]));

        // Temporal state.
        const bool temporal = cvTemporal;
        HistoryTexture history = ctx.history("Volumetrics.FogScatter", [&] {
            rhi::TextureDesc d = volumeDesc(grid, "Volumetrics.FogScatter");
            d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
            return d;
        }());
        const bool sameGrid = vs.prevGrid.farDistance == grid.farDistance && vs.prevGrid.depthScale == grid.depthScale;
        const bool historyValid = temporal && history.previousValid && sameGrid && vs.lastFogFrame + 1 == view.frameIndex() &&
                                  vs.prevPreExposure > 0.0f;
        glm::vec3 jitter(0.0f);
        if (temporal) {
            const u32 i = u32(view.frameIndex() % 16) + 1;
            jitter = {halton(i, 3) - 0.5f, halton(i, 5) - 0.5f, halton(i, 2) - 0.5f};
        }

        // View constants (fog_common.glsl / fog_sample.glsl).
        c.volumetricFogGrid = {f32(grid.x), f32(grid.y), f32(grid.z), grid.farDistance};
        c.volumetricFogDepth = {grid.depthScale, grid.logRange(), jitter.z, 0.0f};
        c.volumetricFogLighting = {fogSettings.ambientIntensity, fogSettings.directionalIntensity, anisotropy,
                                   std::max(cvSkyDistance.get(), grid.farDistance)};
        c.fogColor.w = 0.0f; // the forward pass must not apply its own height fog on top

        // Fog volumes: nearest first, culled against the grid range.
        std::vector<GpuFogVolume> volumes;
        if (ext) {
            struct Candidate {
                const SnapshotFogVolume* v;
                f32 distance;
            };
            std::vector<Candidate> candidates;
            const f32 reach = grid.farDistance * 2.0f;
            for (const SnapshotFogVolume& sv : ext->volumes) {
                const glm::vec3 e = sv.volume.shape == FogVolumeShape::Sphere ? glm::vec3(sv.volume.extents.x) : sv.volume.extents;
                const f32 radius = glm::length(glm::vec3(sv.world[0])) * e.x + glm::length(glm::vec3(sv.world[1])) * e.y +
                                   glm::length(glm::vec3(sv.world[2])) * e.z;
                const f32 d = glm::length(glm::vec3(sv.world[3]) - camPos) - radius;
                if (d > reach) continue;
                candidates.push_back({&sv, d});
            }
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
            const usize maxVolumes = usize(std::clamp(cvMaxVolumes.get(), 0, 1024));
            if (candidates.size() > maxVolumes) candidates.resize(maxVolumes);
            for (const Candidate& cand : candidates) {
                const FogVolumeComponent& v = cand.v->volume;
                const glm::vec3 e = glm::max(v.shape == FogVolumeShape::Sphere ? glm::vec3(v.extents.x) : v.extents, glm::vec3(1e-4f));
                GpuFogVolume g;
                const glm::mat4 worldToUnit = glm::inverse(cand.v->world * glm::scale(glm::mat4(1.0f), e));
                for (int r = 0; r < 3; ++r) g.unitRows[r] = glm::row(worldToUnit, r);
                g.albedoDensity = {glm::clamp(v.albedo, glm::vec3(0.0f), glm::vec3(1.0f)), v.density};
                g.emissionG = {v.emission, std::clamp(v.anisotropy, -0.95f, 0.95f)};
                const f32 scale = std::max(v.noiseScale, 0.01f);
                g.params = {std::clamp(v.falloff, 0.0f, 1.0f), std::clamp(v.noiseIntensity, 0.0f, 1.0f), 1.0f / scale,
                            f32(u32(v.shape))};
                g.noiseOffset = {wrapOffset(glm::dvec3(v.noiseVelocity + wind * v.windInfluence) * time, f64(scale)), 0.0f};
                volumes.push_back(g);
            }
        }

        // Ray traced visibility hook.
        const rhi::RGTexture visibility = R.texture(kVolumetricFogVisibility);

        GpuFogConstants fc;
        fc.grid = {grid.x, grid.y, grid.z, u32(volumes.size())};
        fc.jitter = {jitter, historyValid ? std::clamp(cvHistoryWeight.get(), 0.0f, 0.99f) : 0.0f};
        fc.prevCamera = {vs.prevCamera, vs.prevPreExposure > 0.0f ? c.preExposure / vs.prevPreExposure : 1.0f};
        fc.prevForward = {vs.prevForward, 0.0f};
        fc.emission = {fogSettings.emission, fogSettings.localLightIntensity};
        fc.wind = {wind, f32(std::fmod(time, 100000.0))};
        fc.volumes = volumes.empty() ? 0 : ctx.upload(std::span<const GpuFogVolume>(volumes));
        fc.noiseTexture = dev.sampledIndex(m_baseNoise);
        fc.weatherTexture = dev.sampledIndex(m_weather);
        fc.flags = (cvSunShadows ? kFogSunShadows : 0u) | (cvLocalShadows ? kFogLocalShadows : 0u) |
                   (temporal ? kFogJitter : 0u) | (visibility.valid() ? kFogVisibility : 0u);
        if (ext && ext->clouds && cvClouds && cvCloudShadows && ext->clouds->shadowStrength > 0.0f) {
            const CloudLayerComponent& cl = *ext->clouds;
            fc.flags |= kFogCloudShadows;
            fc.cloudLayer = {cl.altitude, std::max(cl.thickness, 1.0f), std::clamp(cl.coverage, 0.0f, 1.0f),
                             std::clamp(cl.shadowStrength, 0.0f, 1.0f)};
            const glm::vec2 offset = cloudWeatherOffset(cl, wind, time);
            fc.cloudWeather = {offset, 1.0f / std::max(cl.weatherScale, 1.0f), kCloudExtinction * cl.density};
        }
        const GpuAllocation fogAlloc = ctx.allocate(sizeof(GpuFogConstants));
        std::memcpy(fogAlloc.cpu, &fc, sizeof(fc));
        GpuFogConstants* fogMapped = static_cast<GpuFogConstants*>(fogAlloc.cpu);
        const VkDeviceAddress fogAddr = fogAlloc.address;

        vs.prevCamera = camPos;
        vs.prevForward = camFwd;
        vs.prevPreExposure = c.preExposure;
        vs.prevGrid = grid;
        vs.lastFogFrame = view.frameIndex();

        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();
        const rhi::RGTexture media = graph.createTexture(volumeDesc(grid, "Volumetrics.FogMedia"));
        const rhi::RGTexture emissive = graph.createTexture(volumeDesc(grid, "Volumetrics.FogEmissive"));
        const rhi::RGTexture fog = graph.createTexture(volumeDesc(grid, "VolumetricFog"));

        graph.addPass("Volumetrics.FogInject", rhi::PassType::Compute)
            .overwrite(media, rhi::Access::StorageWriteCompute)
            .overwrite(emissive, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                struct {
                    u64 view, scene, fog;
                    u32 media, emissive;
                } pc{viewAddr, sceneAddr, fogAddr, p.storageIndex(media), p.storageIndex(emissive)};
                p.cmd.bindPipeline(m_inject);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((grid.x + 7) / 8, (grid.y + 7) / 8, grid.z);
            });

        // Closest opaque depth per froxel column (limits lighting of the slice that straddles a surface).
        const rhi::RGTexture sceneDepth = R.texture(res::kDepth);
        rhi::TextureDesc tileDesc;
        tileDesc.format = VK_FORMAT_R32_SFLOAT;
        tileDesc.width = grid.x;
        tileDesc.height = grid.y;
        tileDesc.usage = rhi::TextureUsage::None;
        tileDesc.name = "Volumetrics.FogTileDepth";
        const rhi::RGTexture tileDepth = graph.createTexture(tileDesc);
        graph.addPass("Volumetrics.FogTileDepth", rhi::PassType::Compute)
            .read(sceneDepth, rhi::Access::SampledCompute)
            .overwrite(tileDepth, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 depth, out, gx, gy;
                } pc{viewAddr, sceneAddr, p.sampledIndex(sceneDepth), p.storageIndex(tileDepth), grid.x, grid.y};
                p.cmd.bindPipeline(m_tileDepth);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((grid.x + 7) / 8, (grid.y + 7) / 8);
            });

        rhi::PassBuilder scatter = graph.addPass("Volumetrics.FogScatter", rhi::PassType::Compute);
        scatter.read(media, rhi::Access::StorageReadCompute)
            .read(tileDepth, rhi::Access::SampledCompute)
            .read(emissive, rhi::Access::StorageReadCompute)
            .overwrite(history.current, rhi::Access::StorageWriteCompute);
        if (historyValid) scatter.read(history.previous, rhi::Access::SampledCompute);
        if (visibility.valid()) scatter.read(visibility, rhi::Access::SampledCompute);
        if (const rhi::RGBuffer clusters = R.buffer(res::kLightClusters); clusters.valid()) {
            scatter.read(clusters, rhi::Access::StorageReadCompute);
        }
        for (std::string_view n : {res::kShadowCascades, res::kShadowAtlas, res::kPointShadows}) {
            if (const rhi::RGTexture t = R.texture(n); t.valid()) scatter.read(t, rhi::Access::SampledCompute);
        }
        const rhi::RGTexture prev = history.previous, cur = history.current;
        scatter.execute([=, this](rhi::PassContext& p) {
            // Transient resource: its bindless index is only known at execution.
            if (visibility.valid()) fogMapped->visibilityTexture = p.sampledIndex(visibility);
            struct {
                u64 view, scene, fog;
                u32 media, emissive, history, out, tileDepth;
            } pc{viewAddr, sceneAddr, fogAddr, p.storageIndex(media), p.storageIndex(emissive),
                 historyValid ? p.sampledIndex(prev) : kInvalidIndex, p.storageIndex(cur), p.sampledIndex(tileDepth)};
            p.cmd.bindPipeline(m_scatter);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch((grid.x + 7) / 8, (grid.y + 7) / 8, grid.z);
        });

        graph.addPass("Volumetrics.FogIntegrate", rhi::PassType::Compute)
            .read(cur, rhi::Access::SampledCompute)
            .overwrite(fog, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                struct {
                    u64 view, scene, fog;
                    u32 scatter, out;
                } pc{viewAddr, sceneAddr, fogAddr, p.sampledIndex(cur), p.storageIndex(fog)};
                p.cmd.bindPipeline(m_integrate);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((grid.x + 7) / 8, (grid.y + 7) / 8);
            });
        R.setTexture(res::kVolumetricFog, fog);
    }

    static glm::vec2 cloudWind(const CloudLayerComponent& cl, glm::vec3 worldWind) {
        const glm::vec2 dir = glm::length(cl.windDirection) > 1e-5f ? glm::normalize(cl.windDirection) : glm::vec2(1, 0);
        return dir * cl.windSpeed + glm::vec2(worldWind.x, worldWind.z);
    }

    static glm::vec2 cloudWeatherOffset(const CloudLayerComponent& cl, glm::vec3 worldWind, f64 time) {
        const glm::vec2 w = cloudWind(cl, worldWind);
        const f64 period = std::max(f64(cl.weatherScale), 1.0);
        return {wrapOffset(f64(cl.weatherOffset.x) - f64(w.x) * time, period),
                wrapOffset(f64(cl.weatherOffset.y) - f64(w.y) * time, period)};
    }

    void setupClouds(FeatureContext& ctx, const VolumetricsSnapshot& ext, ViewState& vs) {
        rhi::Device& dev = ctx.device();
        rhi::RenderGraph& graph = ctx.graph();
        FrameResources& R = ctx.resources();
        const RenderSnapshot& snap = ctx.snapshot();
        const RenderView& view = ctx.view();
        const GpuViewConstants& c = ctx.viewConstants();
        const CloudLayerComponent& cl = *ext.clouds;
        const Extent2D re = ctx.renderExtent();
        const glm::vec3 wind = worldWind(&ext);

        const u32 down = u32(std::clamp(cvCloudDownsample.get(), 1, 8));
        const bool temporal = cvCloudTemporal;
        u32 checker = 1;
        if (temporal) checker = cvCloudChecker.get() >= 16 ? 4 : cvCloudChecker.get() >= 4 ? 2 : 1;
        const u32 cw = (re.width + down - 1) / down, ch = (re.height + down - 1) / down;
        const u32 tw = (cw + checker - 1) / checker, th = (ch + checker - 1) / checker;

        HistoryTexture hColor = ctx.history("Volumetrics.CloudColor", [&] {
            rhi::TextureDesc d = cloudDesc(cw, ch, "Volumetrics.CloudColor");
            d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
            return d;
        }());
        HistoryTexture hData = ctx.history("Volumetrics.CloudData", [&] {
            rhi::TextureDesc d = cloudDesc(cw, ch, "Volumetrics.CloudData");
            d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
            return d;
        }());
        const bool historyValid = temporal && hColor.previousValid && hData.previousValid &&
                                  vs.lastCloudFrame + 1 == view.frameIndex() && vs.cloudPrevPreExposure > 0.0f;
        const u32 phase = u32(view.frameIndex() % (checker * checker));
        const u32 cell = checker == 4 ? kBayer4[phase] : checker == 2 ? kBayer2[phase] : 0u;

        GpuCloudConstants cc;
        const f32 thickness = std::max(cl.thickness, 10.0f);
        cc.layer = {cl.altitude, thickness, std::clamp(cl.coverage, 0.0f, 1.0f), std::clamp(cl.cloudType, 0.0f, 1.0f)};
        cc.shape = {kCloudExtinction * std::max(cl.density, 0.0f), 1.0f / std::max(cl.shapeScale, 1.0f),
                    1.0f / std::max(cl.detailScale, 0.1f), std::clamp(cl.detailStrength, 0.0f, 1.0f)};
        const glm::vec2 w2 = cloudWind(cl, wind);
        const glm::vec3 shapeOffset =
            wrapOffset(-glm::dvec3(w2.x, 0.0, w2.y) * 1.3 * snap.time, f64(std::max(cl.shapeScale, 1.0f)));
        cc.wind = {shapeOffset, 1.0f / std::max(cl.weatherScale, 1.0f)};
        cc.weather = {cloudWeatherOffset(cl, wind, snap.time), kPlanetRadius, std::max(cvCloudDistance.get(), 1000.0f)};
        cc.lighting = {cl.ambientIntensity, cl.sunIntensity, std::clamp(cl.forwardScattering, 0.0f, 0.95f), -0.3f};
        cc.albedo = {glm::clamp(cl.albedo, glm::vec3(0.0f), glm::vec3(1.0f)), 0.6f};
        cc.size = {tw, th, cw, ch};
        cc.march = {checker, cell % checker, cell / checker, u32(std::clamp(cvCloudSteps.get(), 8, 512))};
        const f32 exposureScale = vs.cloudPrevPreExposure > 0.0f ? c.preExposure / vs.cloudPrevPreExposure : 1.0f;
        u32 exposureBits = 0;
        std::memcpy(&exposureBits, &exposureScale, 4);
        cc.misc = {u32(std::clamp(cvCloudLightSteps.get(), 1, 32)), historyValid ? 1u : 0u, u32(view.frameIndex()),
                   exposureBits};
        cc.baseNoise = dev.sampledIndex(m_baseNoise);
        cc.detailNoise = dev.sampledIndex(m_detailNoise);
        cc.weatherTexture = dev.sampledIndex(m_weather);
        vs.cloudPrevPreExposure = c.preExposure;
        vs.lastCloudFrame = view.frameIndex();

        const rhi::RGTexture depth = R.texture(res::kDepth);
        const rhi::RGTexture traceColor = graph.createTexture(cloudDesc(tw, th, "Volumetrics.CloudTrace"));
        const rhi::RGTexture traceData = graph.createTexture(cloudDesc(tw, th, "Volumetrics.CloudTraceData"));
        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();
        const GpuAllocation constants = ctx.allocate(sizeof(GpuCloudConstants));
        std::memcpy(constants.cpu, &cc, sizeof(cc));
        GpuCloudConstants* mapped = static_cast<GpuCloudConstants*>(constants.cpu);
        const VkDeviceAddress cloudAddr = constants.address;

        graph.addPass("Volumetrics.CloudTrace", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .overwrite(traceColor, rhi::Access::StorageWriteCompute)
            .overwrite(traceData, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                mapped->depthTexture = p.sampledIndex(depth); // transient: index known at execution
                struct {
                    u64 view, scene, clouds;
                    u32 color, data;
                } pc{viewAddr, sceneAddr, cloudAddr, p.storageIndex(traceColor), p.storageIndex(traceData)};
                p.cmd.bindPipeline(m_cloudTrace);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((tw + 7) / 8, (th + 7) / 8);
            });

        const rhi::RGTexture prevColor = hColor.previous, prevData = hData.previous;
        const rhi::RGTexture outColor = hColor.current, outData = hData.current;
        rhi::PassBuilder rb = graph.addPass("Volumetrics.CloudReconstruct", rhi::PassType::Compute);
        rb.read(traceColor, rhi::Access::SampledCompute)
            .read(traceData, rhi::Access::SampledCompute)
            .overwrite(outColor, rhi::Access::StorageWriteCompute)
            .overwrite(outData, rhi::Access::StorageWriteCompute);
        if (historyValid) rb.read(prevColor, rhi::Access::SampledCompute).read(prevData, rhi::Access::SampledCompute);
        rb.execute([=, this](rhi::PassContext& p) {
            struct {
                u64 view, scene, clouds;
                u32 traceColor, traceData, prevColor, prevData, outColor, outData;
            } pc{viewAddr,
                 sceneAddr,
                 cloudAddr,
                 p.sampledIndex(traceColor),
                 p.sampledIndex(traceData),
                 historyValid ? p.sampledIndex(prevColor) : kInvalidIndex,
                 historyValid ? p.sampledIndex(prevData) : kInvalidIndex,
                 p.storageIndex(outColor),
                 p.storageIndex(outData)};
            p.cmd.bindPipeline(m_cloudReconstruct);
            p.cmd.pushConstants(pc);
            p.cmd.dispatch((cw + 7) / 8, (ch + 7) / 8);
        });
        R.setTexture(kVolumetricClouds, outColor);
        R.setTexture(kVolumetricCloudsDepth, outData);
    }

    void setupComposite(FeatureContext& ctx) {
        FrameResources& R = ctx.resources();
        const rhi::RGTexture fog = m_fogThisFrame ? R.texture(res::kVolumetricFog) : rhi::RGTexture{};
        const rhi::RGTexture clouds = m_cloudsThisFrame ? R.texture(kVolumetricClouds) : rhi::RGTexture{};
        const rhi::RGTexture cloudData = m_cloudsThisFrame ? R.texture(kVolumetricCloudsDepth) : rhi::RGTexture{};
        if (!fog.valid() && !clouds.valid()) return;
        const rhi::RGTexture hdr = R.texture(res::kSceneColorHDR), depth = R.texture(res::kDepth);
        rhi::PassBuilder pb = ctx.graph().addPass("Volumetrics.Composite");
        pb.read(depth, rhi::Access::SampledFragment).color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD);
        if (fog.valid()) pb.read(fog, rhi::Access::SampledFragment);
        if (clouds.valid()) pb.read(clouds, rhi::Access::SampledFragment).read(cloudData, rhi::Access::SampledFragment);
        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();
        const u32 flags = (clouds.valid() ? 1u : 0u) | (fog.valid() ? 2u : 0u);
        pb.execute([=, this](rhi::PassContext& p) {
            struct {
                u64 view, scene;
                u32 depth, fog, cloudColor, cloudData, flags;
            } pc{viewAddr,
                 sceneAddr,
                 p.sampledIndex(depth),
                 fog.valid() ? p.sampledIndex(fog) : kInvalidIndex,
                 clouds.valid() ? p.sampledIndex(clouds) : kInvalidIndex,
                 clouds.valid() ? p.sampledIndex(cloudData) : kInvalidIndex,
                 flags};
            drawFullscreen(p.cmd, m_composite, &pc, sizeof(pc));
        });
    }

    rhi::PipelineHandle m_inject, m_scatter, m_tileDepth, m_integrate, m_cloudTrace, m_cloudReconstruct, m_noise, m_composite;
    rhi::TextureHandle m_baseNoise, m_detailNoise, m_weather;
    bool m_fogThisFrame = false;
    bool m_cloudsThisFrame = false;
};

} // namespace

// --- FroxelGrid ---

f32 FroxelGrid::logRange() const { return std::log2(1.0f + farDistance * depthScale); }
f32 FroxelGrid::sliceToDepth(f32 slice01) const { return (std::exp2(slice01 * logRange()) - 1.0f) / depthScale; }
f32 FroxelGrid::depthToSlice(f32 viewDepth) const {
    return std::log2(1.0f + std::max(viewDepth, 0.0f) * depthScale) / logRange();
}

FroxelGrid froxelGridFromCVars(const RenderSnapshot* snapshot) {
    FroxelGrid g;
    g.x = u32(std::clamp(cvGridX.get(), 8, 512));
    g.y = u32(std::clamp(cvGridY.get(), 8, 512));
    g.z = u32(std::clamp(cvGridZ.get(), 8, 256));
    g.farDistance = std::clamp(cvDistance.get(), 1.0f, 10000.0f);
    g.depthScale = std::clamp(cvDepthScale.get(), 0.01f, 1000.0f);
    if (snapshot) {
        if (const VolumetricsSnapshot* v = snapshot->findExtension<VolumetricsSnapshot>(); v && v->fog && v->fog->distance > 0.0f) {
            g.farDistance = std::clamp(v->fog->distance, 1.0f, 10000.0f);
        }
    }
    return g;
}

void registerVolumetricsFeatures(FeatureRegistry& registry) {
    registerVolumetricsTypes();
    registry.add(std::make_unique<VolumetricsFeature>());
}

} // namespace ox::render::volumetrics
