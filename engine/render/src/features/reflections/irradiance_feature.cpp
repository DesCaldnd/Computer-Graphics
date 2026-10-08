// "IrradianceVolumes" feature: baked irradiance probe grids → IndirectDiffuse.
// Bake: per probe, 6 scene captures (rgb radiance, a = distance) into a scratch cube → SH L1 projection + octahedral
// depth moments (volume_project.comp), r.GI.IrradianceVolumes.ProbesPerFrame probes per frame. Sampling: DDGI-style
// trilinear interpolation with backface and Chebyshev visibility weights (indirect_diffuse.comp), blended over the
// sky SH9 fallback. The data layout (reflection_gpu_types.hpp, irradiance_volume.glsl) is shared with DDGI.
#include "reflection_internal.hpp"

#include <oxwald/core/log.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace ox::render::reflections {

namespace {

constexpr VkFormat kHdr = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr u32 kEvictFrames = 300;

struct VolumeSlot {
    Uuid uuid;
    glm::ivec3 counts{0};
    u32 firstProbe = 0;
    u32 probeCount = 0;
    rhi::TextureHandle atlas;
    u32 nextProbe = 0;   // bake progress
    bool ready = false;
    bool force = false;
    u64 hash = 0;
    u64 lastSeen = 0;
    u32 captureSize = 32;
    // Grid (updated every frame from the snapshot).
    glm::mat4 gridToWorld{1.0f}, worldToGrid{1.0f}, worldToLocal{1.0f};
    glm::vec3 extents{1.0f};
    f32 spacingMin = 1.0f, spacingMax = 1.0f;
    f32 intensity = 1.0f, normalBias = 0.0f, viewBias = 0.0f, blend = 1.0f;
    i32 priority = 0;
    f32 volume = 0.0f;
};

u64 volumeKey(const SnapshotIrradianceVolume& v) {
    if (v.uuid.isValid()) return std::hash<Uuid>{}(v.uuid) & ~(1ull << 63);
    return (1ull << 63) | v.entityId;
}

glm::ivec3 clampCounts(glm::ivec3 c) {
    c = glm::clamp(c, glm::ivec3(2), glm::ivec3(64));
    while (u64(c.x) * c.y * c.z > 16384) c = glm::max(c - 1, glm::ivec3(2));
    return c;
}

Extent2D atlasExtent(u32 probes) {
    const u32 cols = std::min(probes, kIrradianceAtlasProbesPerRow);
    const u32 rows = (probes + kIrradianceAtlasProbesPerRow - 1) / kIrradianceAtlasProbesPerRow;
    return {cols * kIrradianceMomentTile, rows * kIrradianceMomentTile};
}

} // namespace

class IrradianceVolumesFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return kIrradianceVolumesFeature; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    i32 order() const override { return 100; }
    std::string_view exclusiveGroup() const override { return "IndirectDiffuse"; }
    std::vector<std::string_view> provides() const override { return {render::res::kIndirectDiffuse}; }
    std::vector<std::string> cvarNames() const override { return cv::giNames(); }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cv::giVolumes.get(); }

    bool initialize(FeatureInitContext& ctx) override {
        m_capture.init(ctx.device);
        m_project = createComputePipeline(ctx.device, "render.gi.volumeProject", "render/reflections/volume_project.comp");
        m_sample = createComputePipeline(ctx.device, "render.gi.indirectDiffuse", "render/reflections/indirect_diffuse.comp");
        return true;
    }
    void shutdown(rhi::Device& dev) override {
        m_capture.shutdown(dev);
        for (rhi::PipelineHandle p : {m_project, m_sample}) {
            if (p) dev.destroy(p);
        }
        for (auto& [key, s] : m_slots) {
            if (s.atlas) dev.destroy(s.atlas);
        }
        m_slots.clear();
        for (auto& [size, t] : m_scratch) dev.destroy(t);
        m_scratch.clear();
        if (m_probes) dev.destroy(m_probes);
        m_probes = {};
    }

    void setup(FeatureContext& ctx) override {
        const ReflectionSnapshot* ext = ctx.snapshot().findExtension<ReflectionSnapshot>();
        if (!ext || ext->volumes.empty()) {
            m_pending = 0;
            m_bakeRequested = false; // nothing to bake
            return;
        }
        rhi::RenderGraph& g = ctx.graph();
        ImportCache imports;
        rhi::RGBuffer probeBuffer;
        const bool firstOfFrame = m_lastFrame != frameCounter(ctx);
        if (firstOfFrame) {
            m_lastFrame = frameCounter(ctx);
            updateSlots(ctx, *ext);
            if (m_probes) probeBuffer = importProbes(g);
            bake(ctx, *ext, imports, probeBuffer);
        }
        if (!m_probes) return;

        // Visible, baked volumes in blending order.
        const Frustum frustum = ctx.view().frustum();
        std::vector<const VolumeSlot*> visible;
        for (const SnapshotIrradianceVolume& sv : ext->volumes) {
            auto it = m_slots.find(volumeKey(sv));
            if (it == m_slots.end() || !it->second.ready) continue;
            const VolumeSlot& s = it->second;
            const glm::vec3 center(glm::inverse(s.worldToLocal)[3]);
            if (!frustum.intersects(Sphere{center, glm::length(s.extents) + s.blend})) continue;
            visible.push_back(&s);
        }
        if (visible.empty()) return;
        std::stable_sort(visible.begin(), visible.end(), [](const VolumeSlot* a, const VolumeSlot* b) {
            return a->priority != b->priority ? a->priority > b->priority : a->volume < b->volume;
        });
        if (visible.size() > kMaxIrradianceVolumes) visible.resize(kMaxIrradianceVolumes);

        rhi::Device& dev = ctx.device();
        GpuIrradianceVolumes gpu;
        gpu.count = u32(visible.size());
        gpu.probes = dev.address(m_probes);
        for (usize i = 0; i < visible.size(); ++i) {
            const VolumeSlot& s = *visible[i];
            GpuIrradianceVolume& v = gpu.volumes[i];
            v.worldToGrid = s.worldToGrid;
            v.gridToWorld = s.gridToWorld;
            v.probeCount = glm::uvec4(glm::uvec3(s.counts), s.firstProbe);
            v.params = {s.intensity, s.normalBias, s.viewBias, std::max(s.blend, 1e-3f)};
            v.params2 = {s.spacingMax * 2.0f, s.spacingMin, 0.0f, 0.0f};
            v.boxHalfExtents = s.extents;
            v.momentAtlas = dev.sampledIndex(s.atlas);
            v.worldToLocal = s.worldToLocal;
        }
        const VkDeviceAddress volAddr = ctx.upload(std::span<const GpuIrradianceVolumes>(&gpu, 1));
        if (!probeBuffer.valid()) probeBuffer = importProbes(g);

        FrameResources& R = ctx.resources();
        const Extent2D re = ctx.renderExtent();
        const rhi::RGTexture depth = R.texture(render::res::kDepth), normals = R.texture(render::res::kNormals);
        const rhi::RGTexture out = g.createTexture(textureDesc(formats::kIndirectDiffuse, re, "IndirectDiffuse"));
        rhi::PassBuilder pb = g.addPass("GI.IndirectDiffuse", rhi::PassType::Compute);
        pb.read(depth, rhi::Access::SampledCompute).read(normals, rhi::Access::SampledCompute);
        pb.read(probeBuffer, rhi::Access::StorageReadCompute);
        for (const VolumeSlot* s : visible) pb.read(imports.get(ctx, s->atlas), rhi::Access::SampledCompute);
        pb.overwrite(out, rhi::Access::StorageWriteCompute);
        const VkDeviceAddress viewAddr = ctx.viewAddress(), sceneAddr = ctx.sceneAddress();
        const rhi::PipelineHandle pipe = m_sample;
        pb.execute([=](rhi::PassContext& p) {
            struct {
                u64 view, scene;
                u32 depth, normals, out, pad;
                u64 volumes;
            } pc{viewAddr, sceneAddr, p.sampledIndex(depth), p.sampledIndex(normals), p.storageIndex(out), 0, volAddr};
            p.cmd.bindPipeline(pipe);
            p.cmd.pushConstants(pc);
            p.cmd.dispatchThreads(re.width, re.height);
        });
        R.setTexture(render::res::kIndirectDiffuse, out);
    }

    // --- bake API ---
    void requestBake() { m_bakeRequested = true; }
    [[nodiscard]] bool bakeInProgress() const { return m_bakeRequested || m_pending > 0; }
    void setBaked(const Uuid& id, BakedIrradianceVolume data) {
        m_baked[id] = std::move(data);
        for (auto& [key, s] : m_slots) {
            if (s.uuid == id) s.hash = 0; // re-layout next frame → installs the data
        }
    }
    std::vector<std::pair<Uuid, BakedIrradianceVolume>> readBaked(rhi::Device& dev) {
        std::vector<std::pair<Uuid, BakedIrradianceVolume>> out;
        if (!m_probes) return out;
        dev.waitIdle();
        for (auto& [key, s] : m_slots) {
            if (!s.ready || !s.uuid.isValid()) continue;
            BakedIrradianceVolume v;
            v.probeCount = s.counts;
            std::vector<u8> bytes = dev.readBuffer(m_probes, u64(s.firstProbe) * sizeof(GpuIrradianceProbe),
                                                   u64(s.probeCount) * sizeof(GpuIrradianceProbe));
            v.probes.resize(s.probeCount);
            std::memcpy(v.probes.data(), bytes.data(), bytes.size());
            v.moments = dev.readTexture(s.atlas);
            out.emplace_back(s.uuid, std::move(v));
        }
        return out;
    }

private:
    rhi::RGBuffer importProbes(rhi::RenderGraph& g) {
        return g.importBuffer(m_probes, {u64(m_capacity) * sizeof(GpuIrradianceProbe), rhi::BufferUsage::Storage,
                                         rhi::MemoryUsage::GpuOnly, "render.irradianceProbes"});
    }

    void updateSlots(FeatureContext& ctx, const ReflectionSnapshot& ext) {
        rhi::Device& dev = ctx.device();
        const u64 frame = frameCounter(ctx) + 1;
        const bool bake = m_bakeRequested;
        m_bakeRequested = false;
        bool layoutChanged = false;
        for (const SnapshotIrradianceVolume& sv : ext.volumes) {
            const u64 key = volumeKey(sv);
            VolumeSlot& s = m_slots[key];
            s.lastSeen = frame;
            s.uuid = sv.uuid;
            const IrradianceVolumeComponent& c = sv.volume;
            const glm::ivec3 counts = clampCounts(c.probeCount);
            const u32 captureSize = std::clamp(std::bit_floor(std::max(c.captureResolution, 8u)), 8u, 128u);
            const glm::mat4 rigid = removeScale(sv.world);
            const glm::vec3 extents = glm::max(c.extents, glm::vec3(0.05f));
            const glm::vec3 spacing = 2.0f * extents / glm::vec3(counts - 1);
            s.gridToWorld = rigid * glm::translate(glm::mat4(1.0f), -extents) * glm::scale(glm::mat4(1.0f), spacing);
            s.worldToGrid = glm::inverse(s.gridToWorld);
            s.worldToLocal = glm::inverse(rigid);
            s.extents = extents;
            s.spacingMin = std::min(spacing.x, std::min(spacing.y, spacing.z));
            s.spacingMax = std::max(spacing.x, std::max(spacing.y, spacing.z));
            s.intensity = c.intensity;
            s.normalBias = c.normalBias * s.spacingMin;
            s.viewBias = c.viewBias * s.spacingMin;
            s.blend = c.blendDistance;
            s.priority = c.priority;
            s.volume = extents.x * extents.y * extents.z;
            u64 h = hashBytes(&sv.world, sizeof(sv.world));
            h = hashBytes(&counts, sizeof(counts), h);
            h = hashBytes(&extents, sizeof(extents), h);
            h = hashBytes(&captureSize, sizeof(captureSize), h);
            if (h != s.hash || s.counts != counts) {
                s.hash = h;
                s.ready = false;
                s.nextProbe = 0;
                s.captureSize = captureSize;
                if (s.counts != counts || !s.atlas) {
                    s.counts = counts;
                    s.probeCount = u32(counts.x * counts.y * counts.z);
                    layoutChanged = true;
                    if (s.atlas) dev.destroy(s.atlas);
                    rhi::TextureDesc d = textureDesc(kHdr, atlasExtent(s.probeCount), "render.irradianceMoments");
                    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSrc |
                              rhi::TextureUsage::TransferDst;
                    s.atlas = dev.createTexture(d);
                }
                s.force = true; // moved / resized: re-bake (baked data no longer matches)
                if (auto it = m_baked.find(s.uuid); s.uuid.isValid() && it != m_baked.end() && it->second.probeCount == counts) {
                    s.force = false;
                }
            }
            if (bake) {
                s.ready = false;
                s.nextProbe = 0;
                s.force = true;
            }
        }
        for (auto it = m_slots.begin(); it != m_slots.end();) {
            if (it->second.lastSeen + kEvictFrames < frame) {
                if (it->second.atlas) dev.destroy(it->second.atlas);
                it = m_slots.erase(it);
                layoutChanged = true;
            } else {
                ++it;
            }
        }
        if (!layoutChanged && m_probes) return;
        // Assign probe ranges; grow the shared buffer (contents are re-baked / re-installed).
        u32 total = 0;
        for (auto& [key, s] : m_slots) {
            s.firstProbe = total;
            total += s.probeCount;
        }
        if (!m_probes || total > m_capacity) {
            if (m_probes) dev.destroy(m_probes);
            m_capacity = std::max(std::bit_ceil(std::max(total, 64u)), 64u);
            m_probes = dev.createBuffer({u64(m_capacity) * sizeof(GpuIrradianceProbe),
                                         rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst | rhi::BufferUsage::TransferSrc,
                                         rhi::MemoryUsage::GpuOnly, "render.irradianceProbes"});
            std::vector<GpuIrradianceProbe> zero(m_capacity);
            dev.writeBuffer(m_probes, zero.data(), zero.size() * sizeof(GpuIrradianceProbe));
        }
        for (auto& [key, s] : m_slots) {
            s.ready = false;
            s.nextProbe = 0;
        }
    }

    void bake(FeatureContext& ctx, const ReflectionSnapshot& ext, ImportCache& imports, rhi::RGBuffer probeBuffer) {
        rhi::Device& dev = ctx.device();
        u32 budget = u32(std::max(cv::giProbesPerFrame.get(), 1));
        u32 pending = 0;
        for (const SnapshotIrradianceVolume& sv : ext.volumes) {
            auto it = m_slots.find(volumeKey(sv));
            if (it == m_slots.end()) continue;
            VolumeSlot& s = it->second;
            if (s.ready) continue;
            // Install baked data instead of capturing.
            if (!s.force && s.nextProbe == 0 && s.uuid.isValid()) {
                auto bit = m_baked.find(s.uuid);
                if (bit != m_baked.end() && bit->second.probeCount == s.counts && bit->second.probes.size() == s.probeCount) {
                    dev.waitIdle(); // frames in flight may still read the shared probe buffer
                    dev.writeBuffer(m_probes, bit->second.probes.data(), bit->second.probes.size() * sizeof(GpuIrradianceProbe),
                                    u64(s.firstProbe) * sizeof(GpuIrradianceProbe));
                    const Extent2D ae = atlasExtent(s.probeCount);
                    if (bit->second.moments.size() == usize(ae.width) * ae.height * 8) {
                        rhi::TextureUploadDesc u;
                        u.finalAccess = rhi::Access::SampledCompute;
                        dev.uploadTexture(s.atlas, bit->second.moments, u);
                        s.ready = true;
                        continue;
                    }
                    OX_LOG_WARN("render", "baked irradiance volume {}: moment data size mismatch, re-baking", s.uuid.toString());
                }
            }
            const rhi::TextureHandle scratch = scratchCube(dev, s.captureSize);
            const rhi::RGTexture scratchRG = imports.get(ctx, scratch);
            const rhi::RGTexture atlasRG = imports.get(ctx, s.atlas);
            while (s.nextProbe < s.probeCount && budget > 0) {
                bakeProbe(ctx, s, s.nextProbe, scratchRG, atlasRG, probeBuffer);
                ++s.nextProbe;
                --budget;
            }
            if (s.nextProbe >= s.probeCount) {
                s.ready = true;
                s.force = false;
            } else {
                pending += s.probeCount - s.nextProbe;
            }
        }
        m_pending = pending;
    }

    rhi::TextureHandle scratchCube(rhi::Device& dev, u32 size) {
        auto it = m_scratch.find(size);
        if (it != m_scratch.end()) return it->second;
        rhi::TextureDesc d;
        d.type = rhi::TextureType::Cube;
        d.arrayLayers = 6;
        d.format = kHdr;
        d.width = d.height = size;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorAttachment;
        d.name = "render.irradianceCapture";
        const rhi::TextureHandle t = dev.createTexture(d);
        m_scratch.emplace(size, t);
        return t;
    }

    void bakeProbe(FeatureContext& ctx, const VolumeSlot& s, u32 local, rhi::RGTexture scratch, rhi::RGTexture atlas,
                   rhi::RGBuffer probeBuffer) {
        const glm::ivec3 c = s.counts;
        const glm::vec3 coord{f32(local % u32(c.x)), f32((local / u32(c.x)) % u32(c.y)), f32(local / u32(c.x * c.y))};
        const glm::vec3 pos = glm::vec3(s.gridToWorld * glm::vec4(coord, 1.0f));
        for (u32 face = 0; face < 6; ++face) {
            glm::mat4 view, proj;
            cubeFaceMatrices(face, pos, 0.02f, view, proj);
            SceneCapture::Request rq;
            rq.name = "GI.ProbeCapture";
            rq.target = scratch;
            rq.layer = face;
            rq.size = {s.captureSize, s.captureSize};
            rq.constants = SceneCapture::makeConstants(ctx.viewConstants(), view, proj, pos, rq.size, 1.0f);
            rq.filter.frustum = Frustum::fromViewProj(proj * view, true);
            m_capture.addPass(ctx, rq);
        }
        const rhi::PipelineHandle pipe = m_project;
        const u32 global = s.firstProbe + local, size = s.captureSize;
        const f32 maxDist = s.spacingMax * 2.0f;
        ctx.graph()
            .addPass("GI.ProbeProject", rhi::PassType::Compute)
            .read(scratch, rhi::Access::SampledCompute)
            .write(probeBuffer, rhi::Access::StorageWriteCompute)
            .write(atlas, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& p) {
                struct {
                    u64 probes;
                    u32 probe, tile, cube, size, atlas;
                    f32 maxDistance;
                } pc{p.address(probeBuffer), global, local, p.sampledIndex(scratch), size, p.storageIndex(atlas), maxDist};
                p.cmd.bindPipeline(pipe);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch(1);
            });
    }

    SceneCapture m_capture;
    rhi::PipelineHandle m_project, m_sample;
    std::unordered_map<u64, VolumeSlot> m_slots;
    std::unordered_map<u32, rhi::TextureHandle> m_scratch;
    std::unordered_map<Uuid, BakedIrradianceVolume> m_baked;
    rhi::BufferHandle m_probes;
    u32 m_capacity = 0;
    u64 m_lastFrame = ~0ull;
    bool m_bakeRequested = false;
    u32 m_pending = 0;
};

std::unique_ptr<IRenderFeature> makeIrradianceVolumesFeature() { return std::make_unique<IrradianceVolumesFeature>(); }

IrradianceVolumesFeature* findIrradiance(Renderer& renderer) {
    return dynamic_cast<IrradianceVolumesFeature*>(renderer.features().find(kIrradianceVolumesFeature));
}

// --- public bake API ---------------------------------------------------------------------------------------------

void requestBake(Renderer& renderer) {
    probesRequestBake(renderer);
    if (IrradianceVolumesFeature* f = findIrradiance(renderer)) f->requestBake();
}

bool bakeInProgress(Renderer& renderer) {
    IrradianceVolumesFeature* f = findIrradiance(renderer);
    return probesBakeInProgress(renderer) || (f && f->bakeInProgress());
}

std::vector<std::pair<Uuid, BakedIrradianceVolume>> readBakedVolumes(Renderer& renderer) {
    IrradianceVolumesFeature* f = findIrradiance(renderer);
    return f ? f->readBaked(renderer.device()) : std::vector<std::pair<Uuid, BakedIrradianceVolume>>{};
}

void setBakedVolume(Renderer& renderer, const Uuid& volume, BakedIrradianceVolume data) {
    if (IrradianceVolumesFeature* f = findIrradiance(renderer)) f->setBaked(volume, std::move(data));
}

} // namespace ox::render::reflections
