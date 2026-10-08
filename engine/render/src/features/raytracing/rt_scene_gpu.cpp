// "RayTracingScene" feature: BLAS per unique (submesh, LOD) through the BlasScheduler (rhi backend: synchronous
// build + compaction for static meshes, update-capable BLASes refitted from deformed geometry), TLAS rebuilt or
// updated once per frame from the GpuScene instances, RtInstance lookup table + RtSceneHeader in frame memory.
// Publishes the "RtScene" buffer (ordering token for ray traced passes) at InjectionPoint::AfterDepth.
#include "rt_internal.hpp"

#include <oxwald/core/hash.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <unordered_map>

namespace ox::render::rt {

namespace {

// rhi backend: Device::createBlas builds (and compacts) synchronously, so every build completes immediately.
class RhiBlasBackend final : public IBlasBackend {
public:
    RhiBlasBackend(rhi::Device& device, GpuScene& scene) : m_device(&device), m_scene(&scene) {}

    DeformedGeometryProvider provider;

    Handle build(const BlasRequest& r, bool allowCompaction) override {
        rhi::BlasDesc d;
        d.name = std::format("rt.blas.{}", r.meshInfo);
        d.geometries.push_back(geometry(r));
        d.allowUpdate = r.deformable;
        d.allowCompaction = allowCompaction && !r.deformable;
        d.preferFastBuild = r.deformable;
        const rhi::AccelStructHandle h = m_device->createBlas(d);
        if (!h) return 0;
        Rec rec;
        rec.handle = h;
        rec.bytes = estimateBytes(r.indexCount / 3, d.allowCompaction);
        m_recs[h.packed()] = rec;
        return h.packed();
    }
    bool isComplete(Handle) override { return true; }
    std::optional<u64> compactedSize(Handle) override { return std::nullopt; }
    Handle compact(Handle h, u64) override { return h; }
    void refit(Handle h, const BlasRequest& r) override { m_refits.push_back({unpack(h), r}); }
    void destroy(Handle h) override {
        auto it = m_recs.find(h);
        if (it == m_recs.end()) return;
        m_device->destroy(it->second.handle);
        m_recs.erase(it);
    }
    u64 memorySize(Handle h) override {
        auto it = m_recs.find(h);
        return it == m_recs.end() ? 0 : it->second.bytes;
    }
    bool compactsInternally() const override { return true; }

    // Records the refits queued during setup (inside the AS pass).
    void recordRefits(rhi::CommandList& cmd) {
        for (const auto& [h, r] : m_refits) {
            rhi::BlasDesc d;
            d.geometries.push_back(geometry(r));
            cmd.refitBlas(h, d);
        }
        m_refits.clear();
    }
    [[nodiscard]] bool hasRefits() const { return !m_refits.empty(); }
    static rhi::AccelStructHandle unpack(Handle h) { return {u32(h & 0xFFFFFFFFu), u32(h >> 32)}; }

private:
    struct Rec {
        rhi::AccelStructHandle handle;
        u64 bytes = 0;
    };
    rhi::BlasTriangles geometry(const BlasRequest& r) const {
        rhi::BlasTriangles t;
        t.indexBuffer = m_scene->indexBuffer();
        t.indexOffset = u64(r.firstIndex) * 4;
        t.indexCount = r.indexCount;
        t.opaque = true; // alpha-tested instances use FORCE_NO_OPAQUE
        std::optional<DeformedGeometry> dg;
        if (r.deformable && provider && r.gpuInstance != ~0u) dg = provider(r.gpuInstance);
        if (dg && dg->positions) {
            t.vertexBuffer = dg->positions;
            t.vertexOffset = dg->offset;
            t.vertexCount = dg->vertexCount;
            t.vertexStride = dg->stride;
        } else {
            t.vertexBuffer = m_scene->positionBuffer();
            t.vertexOffset = u64(u32(r.vertexOffset)) * sizeof(glm::vec3);
            t.vertexCount = r.vertexCount;
            t.vertexStride = sizeof(glm::vec3);
        }
        return t;
    }
    static u64 estimateBytes(u64 triangles, bool compacted) {
        // Typical driver BVH sizes: ~64 B/triangle built for fast trace, ~half after compaction.
        return triangles * (compacted ? 36u : 72u) + 1024;
    }
    rhi::Device* m_device;
    GpuScene* m_scene;
    std::unordered_map<Handle, Rec> m_recs;
    std::vector<std::pair<rhi::AccelStructHandle, BlasRequest>> m_refits;
};

struct MarkerState final : IFeatureViewState {
    rhi::BufferHandle marker;
    void release(rhi::Device& d) override {
        if (marker) d.destroy(marker);
    }
};

class RayTracingSceneFeature final : public IRenderFeature, public RayTracingSceneApi {
public:
    explicit RayTracingSceneFeature(std::shared_ptr<RtShared> shared) : m_shared(std::move(shared)) {}

    std::string_view name() const override { return kRayTracingSceneFeature; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::AfterDepth); }
    i32 order() const override { return -1000; }
    std::vector<std::string_view> provides() const override { return {res::kRtScene}; }
    std::vector<std::string> cvarNames() const override {
        return {"r.RayTracing", "r.RayTracing.BLAS.LOD", "r.RayTracing.BLAS.BuildsPerFrame"};
    }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        if (!rayTracingActive(s, caps)) return false;
        const RtSettings r = RtSettings::fromCVars();
        return r.shadows || r.reflections || r.ao || r.gi || r.translucency || r.volumetrics || r.pathTracing;
    }

    bool initialize(FeatureInitContext& ctx) override {
        if (!ctx.device.caps().rayTracingSupported()) return false;
        m_device = &ctx.device;
        m_backend = std::make_unique<RhiBlasBackend>(ctx.device, ctx.renderer.scene());
        m_backend->provider = m_provider;
        m_scheduler = std::make_unique<BlasScheduler>(*m_backend);
        return true;
    }

    void shutdown(rhi::Device& device) override {
        m_scheduler.reset();
        m_backend.reset();
        if (m_tlas) device.destroy(m_tlas);
        m_tlas = {};
        m_shared->sceneActive = false;
    }

    // --- RayTracingSceneApi ---
    void setDeformedGeometryProvider(DeformedGeometryProvider provider) override {
        m_provider = std::move(provider);
        if (m_backend) m_backend->provider = m_provider;
    }
    bool activeThisFrame() const override { return m_device && m_shared->sceneReady(*m_device); }
    VkDeviceAddress sceneHeaderAddress() const override { return activeThisFrame() ? m_shared->header : 0; }
    u64 tlasAddress() const override { return activeThisFrame() ? m_shared->tlasAddress : 0; }
    const BlasScheduler::Stats& blasStats() const override {
        static const BlasScheduler::Stats empty;
        return m_scheduler ? m_scheduler->stats() : empty;
    }
    u32 tlasInstanceCount() const override { return m_shared->tlasInstances; }

    void setup(FeatureContext& ctx) override {
        OX_PROFILE_ZONE_N("RayTracingScene");
        rhi::Device& dev = ctx.device();
        const RtSettings& st = m_shared->refresh(dev);
        MarkerState& ms = ctx.viewState<MarkerState>();
        if (!ms.marker) {
            ms.marker = dev.createBuffer({256, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly, "rt.sceneToken"});
        }
        const rhi::BufferDesc markerDesc{256, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly, "RtScene"};
        const bool first = m_shared->sceneFrame != dev.frameNumber();
        if (!first) {
            // TLAS already built this frame (earlier view): just publish the token.
            const rhi::RGBuffer token = ctx.graph().importBuffer(ms.marker, markerDesc, {rhi::Access::General, rhi::Access::Undefined});
            ctx.graph()
                .addPass("RT.SceneToken", rhi::PassType::Compute)
                .write(token, rhi::Access::StorageWriteCompute)
                .execute([](rhi::PassContext&) {});
            ctx.resources().setBuffer(res::kRtScene, token);
            return;
        }
        m_shared->sceneFrame = dev.frameNumber();
        m_shared->sceneActive = false;

        GpuScene& scene = ctx.scene();
        m_scheduler->config().maxBuildsPerFrame = u32(std::max(st.blasBuildsPerFrame, 1));
        m_scheduler->beginFrame(dev.frameNumber());
        const glm::vec3 cam = ctx.view().camera().position();

        // --- BLAS requests + TLAS sources ---
        struct Pending {
            TlasSource src;
            u32 firstIndex;
            i32 vertexOffset;
        };
        std::vector<Pending> pending;
        const std::vector<GpuInstance>& instances = scene.instances();
        pending.reserve(instances.size());
        u64 structure = hashCombine(scene.structureVersion(), u64(st.blasLod));
        bool newBlas = false;
        for (u32 i = 0; i < instances.size(); ++i) {
            const GpuInstance& inst = instances[i];
            if (!(inst.flags & kInstanceVisible)) continue;
            const GpuMeshInfo& mi = scene.meshInfo(inst.meshIndex);
            const GpuMaterial& mat = scene.material(inst.materialIndex);
            const u32 lod = selectBlasLod(std::max(mi.lodCount, 1u), st.blasLod);
            u32 firstIndex = mi.firstIndex, indexCount = mi.indexCount;
            if (lod > 0) {
                const GpuMeshLod& l = scene.meshLods(inst.meshIndex)[lod];
                if (l.indexCount > 0) {
                    firstIndex = l.firstIndex;
                    indexCount = l.indexCount;
                }
            }
            if (indexCount < 3) continue;
            const bool skinned = (inst.flags & (kInstanceSkinned | kInstanceSkinnedOutput)) != 0;
            const bool deformable = skinned && bool(m_provider);
            BlasRequest r;
            r.meshInfo = inst.meshIndex;
            r.firstIndex = firstIndex;
            r.indexCount = indexCount;
            r.vertexOffset = mi.vertexOffset;
            r.vertexCount = mi.vertexCount;
            r.deformable = deformable;
            r.gpuInstance = deformable ? i : ~0u;
            r.key = blasKey(inst.meshIndex, firstIndex, indexCount, mi.vertexOffset, mi.vertexCount, deformable);
            if (deformable) r.key = hashCombine(r.key, i);
            const f32 dist = std::max(glm::length(glm::vec3(inst.boundingSphere) - cam), 0.1f);
            r.priority = inst.boundingSphere.w / dist;
            if (!m_scheduler->state(r.key)) newBlas = true;
            m_scheduler->request(r);
            if (deformable) m_scheduler->markDeformed(r.key);

            Pending p;
            p.src.gpuInstance = i;
            p.src.meshInfo = inst.meshIndex;
            p.src.material = inst.materialIndex;
            p.src.blend = mat.flags & kMaterialBlendMask;
            p.src.doubleSided = (mat.flags & kMaterialDoubleSided) != 0;
            p.src.castShadows = (inst.flags & kInstanceCastShadows) != 0;
            p.src.skinned = skinned;
            p.src.deformedBlas = deformable && (inst.flags & kInstanceSkinnedOutput) != 0;
            p.src.world = inst.world;
            p.src.blasKey = r.key;
            p.firstIndex = firstIndex;
            p.vertexOffset = mi.vertexOffset;
            pending.push_back(p);
            structure = hashCombine(structure, hashCombine(r.key, (u64(i) << 32) | inst.materialIndex));
        }
        // Material contents (editor tweaks) also restart the path tracer accumulation.
        {
            std::vector<u32> mats;
            mats.reserve(pending.size());
            for (const Pending& p : pending) mats.push_back(p.src.material);
            std::sort(mats.begin(), mats.end());
            mats.erase(std::unique(mats.begin(), mats.end()), mats.end());
            for (u32 m : mats) {
                const GpuMaterial& gm = scene.material(m);
                structure = fnv1a64(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&gm), sizeof(gm)), structure);
            }
        }
        // BLAS inputs (freshly streamed meshes) must be resident before the synchronous builds.
        if (newBlas || m_scheduler->stats().queued > 0) dev.flushUploads();
        m_scheduler->update();
        std::vector<TlasSource> sources;
        sources.reserve(pending.size());
        for (Pending& p : pending) {
            p.src.blas = m_scheduler->ready(p.src.blasKey);
            sources.push_back(p.src);
        }
        const TlasInstanceTable::Result res = m_table.update(sources);
        std::vector<RtInstanceGpu> table = m_table.gpuInstances();
        std::unordered_map<u32, const Pending*> byInstance;
        for (const Pending& p : pending) byInstance[p.src.gpuInstance] = &p;
        for (RtInstanceGpu& g : table) {
            const Pending* p = byInstance[g.gpuInstance];
            g.firstIndex = p->firstIndex;
            g.vertexOffset = p->vertexOffset;
        }

        // --- TLAS (grow when needed) ---
        const u32 count = u32(m_table.entries().size());
        bool rebuild = res.rebuild;
        if (!m_tlas || count > m_tlasCapacity) {
            if (m_tlas) dev.destroy(m_tlas);
            m_tlasCapacity = std::max(1024u, std::bit_ceil(std::max(count, 1u)));
            m_tlas = dev.createTlas({"rt.tlas", m_tlasCapacity, true});
            rebuild = true;
        }
        if (!m_tlas) return;
        auto tlasInstances = std::make_shared<std::vector<rhi::TlasInstance>>();
        tlasInstances->reserve(count);
        for (const TlasEntry& e : m_table.entries()) {
            rhi::TlasInstance t;
            t.transform = e.transform;
            t.customIndex = e.customIndex;
            t.mask = u8(e.mask);
            t.sbtRecordOffset = e.sbtOffset;
            t.flags = e.flags;
            t.blas = RhiBlasBackend::unpack(e.blas);
            tlasInstances->push_back(t);
        }

        // --- header + tables in frame memory ---
        const VkDeviceAddress tableAddr = ctx.upload(std::span<const RtInstanceGpu>(table));
        GpuAllocation h = ctx.allocate(sizeof(RtSceneHeaderGpu), 16);
        auto* hdr = static_cast<RtSceneHeaderGpu*>(h.cpu);
        *hdr = RtSceneHeaderGpu{};
        hdr->tlas = dev.accelStructAddress(m_tlas);
        hdr->instances = tableAddr;
        hdr->indices = dev.address(scene.indexBuffer());
        hdr->instanceCount = count;
        hdr->frame = u32(dev.frameNumber());
        m_shared->header = h.address;
        m_shared->headerCpu = hdr;
        m_shared->tlasAddress = hdr->tlas;
        m_shared->tlasInstances = count;
        m_shared->sceneStructureHash = structure;
        m_shared->sceneActive = true;

        const rhi::RGBuffer token = ctx.graph().importBuffer(ms.marker, markerDesc, {rhi::Access::General, rhi::Access::Undefined});
        const bool build = rebuild || res.changed || m_backend->hasRefits() || !m_built;
        const bool update = !rebuild && m_built;
        rhi::AccelStructHandle tlas = m_tlas;
        RhiBlasBackend* backend = m_backend.get();
        ctx.graph()
            .addPass("RT.BuildAS", rhi::PassType::Compute)
            .write(token, rhi::Access::StorageWriteCompute)
            .sideEffect()
            .execute([tlas, tlasInstances, backend, build, update](rhi::PassContext& p) {
                // WAR against the previous frame's traversals of the same TLAS / refitted BLASes.
                p.cmd.memoryBarrier(rhi::Access::AccelStructRead, rhi::Access::AccelStructBuildWrite);
                backend->recordRefits(p.cmd);
                if (build) p.cmd.buildTlas(tlas, *tlasInstances, update);
            });
        m_built = true;
        ctx.resources().setBuffer(res::kRtScene, token);
    }

private:
    std::shared_ptr<RtShared> m_shared;
    rhi::Device* m_device = nullptr;
    std::unique_ptr<RhiBlasBackend> m_backend;
    std::unique_ptr<BlasScheduler> m_scheduler;
    TlasInstanceTable m_table;
    rhi::AccelStructHandle m_tlas;
    u32 m_tlasCapacity = 0;
    bool m_built = false;
    DeformedGeometryProvider m_provider;
};

} // namespace

std::unique_ptr<IRenderFeature> makeRayTracingSceneFeature(std::shared_ptr<RtShared> shared) {
    return std::make_unique<RayTracingSceneFeature>(std::move(shared));
}

RayTracingSceneApi* RayTracingSceneApi::find(FeatureRegistry& registry) {
    return dynamic_cast<RayTracingSceneApi*>(registry.find(kRayTracingSceneFeature));
}

} // namespace ox::render::rt
