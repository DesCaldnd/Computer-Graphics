// "Skinning": compute skinning of skinned GPU-scene instances. GpuScene calls our resolver while updating instances
// (beginFrame): each skinned entity gets a double-buffered region in the GpuSkinnedVertex arena (this frame / previous
// frame, swapped every frame) and its instances are flagged kInstanceSkinnedOutput, so every core draw (prepass,
// forward, shadows, picking, GPU culling) reads pre-skinned positions through oxFetchVertex and the previous frame's
// output gives skinning motion vectors. The compute pass runs at PreDepth of the first view of the frame.
#include "world_internal.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/render/features/raytracing/rt_api.hpp>

#include <cstring>
#include <unordered_map>

namespace ox::render::worldfx {

namespace {

struct SkinJobGpu {
    u32 vertexBase, vertexCount, skinBase, paletteBase, outBase, mode, pad0, pad1;
};

struct SkinPush {
    u64 scene, jobs, dualQuats, out;
    u32 jobCount, pad;
};

// Dual quaternion of a rigid palette matrix (scale stripped), as (real xyzw, dual xyzw) — ox::anim::DualQuat::fromMatrix.
void toDualQuat(const glm::mat4& m, glm::vec4& real, glm::vec4& dual) {
    glm::vec3 x(m[0]), y(m[1]), z(m[2]);
    const f32 sx = glm::length(x), sy = glm::length(y), sz = glm::length(z);
    glm::mat3 r(x / std::max(sx, 1e-8f), y / std::max(sy, 1e-8f), z / std::max(sz, 1e-8f));
    if (glm::determinant(r) < 0.0f) r[0] = -r[0];
    const glm::quat q = glm::normalize(glm::quat_cast(r));
    const glm::vec3 t(m[3]);
    const glm::quat d = glm::quat(0.0f, t.x, t.y, t.z) * q * 0.5f;
    real = {q.x, q.y, q.z, q.w};
    dual = {d.x, d.y, d.z, d.w};
}

class SkinningFeature final : public IRenderFeature, public ISkinnedOutputs {
public:
    std::string_view name() const override { return "Skinning"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::PreDepth); }
    i32 order() const override { return -2000; } // before culling and anything that reads geometry
    std::vector<std::string_view> provides() const override { return {kSkinnedVerticesResource}; }
    std::vector<std::string> cvarNames() const override { return skinningCVarNames(); }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return WorldCVars::read().computeSkinning; }

    bool initialize(FeatureInitContext& ctx) override {
        m_device = &ctx.device;
        m_scene = &ctx.renderer.scene();
        m_pipeline = createComputePipeline(ctx.device, "world.skinning", "render/world/skinning.comp");
        // BLAS refit input for the ray tracing team when acceleration structures exist (the usage bit needs the extension).
        rhi::BufferUsage usage = rhi::BufferUsage::Storage;
        if (ctx.device.caps().accelerationStructure) usage = usage | rhi::BufferUsage::AccelStructInput;
        m_arena.init(ctx.device, "world.skinnedVertices", sizeof(GpuSkinnedVertex), 64 * 1024, usage);
        m_scene->setSkinnedVertexAddress(m_arena.address());
        m_scene->setSkinOutputResolver([this](const SnapshotMesh& sm, const GpuMesh& gm, u32 submesh, u64 stamp, u32& cur, u32& prev) {
            return resolve(sm, gm, submesh, stamp, cur, prev);
        });
        // Skinned BLASes are refitted from this frame's output (object space, GpuSkinnedVertex stride 40, position at
        // offset 0, first vertex at paletteOffset of a kInstanceSkinnedOutput instance).
        if (auto* rtScene = rt::RayTracingSceneApi::find(ctx.renderer.features())) {
            rtScene->setDeformedGeometryProvider([this](u32 gpuInstance) { return deformedGeometry(gpuInstance); });
            m_rtScene = rtScene;
        }
        return true;
    }

    void shutdown(rhi::Device& dev) override {
        if (m_rtScene) m_rtScene->setDeformedGeometryProvider({});
        if (m_scene) {
            m_scene->setSkinOutputResolver({});
            m_scene->setSkinnedVertexAddress(0);
        }
        m_arena.release(dev);
        dev.destroy(m_pipeline);
    }

    const SkinnedOutputs& skinnedOutputs() const override { return m_outputs; }

    void setup(FeatureContext& ctx) override {
        if (m_jobsStamp == m_setupStamp) return; // once per frame (first view)
        m_setupStamp = m_jobsStamp;
        OX_PROFILE_ZONE_N("Skinning.setup");
        // Free regions of entities that disappeared (after the frames in flight that may still read them).
        for (auto it = m_allocs.begin(); it != m_allocs.end();) {
            if (it->second.stamp + 2 < m_jobsStamp) {
                m_pendingFrees.push_back({m_jobsStamp, it->second.base, u64(it->second.vertexCount) * 2});
                it = m_allocs.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = m_pendingFrees.begin(); it != m_pendingFrees.end();) {
            if (it->stamp + ctx.device().framesInFlight() + 1 <= m_jobsStamp) {
                m_arena.free(it->offset, it->count);
                it = m_pendingFrees.erase(it);
            } else {
                ++it;
            }
        }
        m_outputs.buffer = m_arena.buffer();
        m_outputs.items.clear();
        if (m_jobs.empty()) return;
        const RenderSnapshot& snap = ctx.snapshot();
        const SkinningSnapshot* methods = snap.findExtension<SkinningSnapshot>();
        std::vector<SkinJobGpu> jobs;
        std::vector<glm::vec4> dq;
        u32 maxVertices = 0;
        for (const Job& j : m_jobs) {
            SkinJobGpu g{j.vertexBase, j.vertexCount, j.skinBase, j.paletteBase, j.outBase, 0, 0, 0};
            if (methods) {
                auto it = methods->methods.find(j.entityId);
                if (it != methods->methods.end() && it->second == GpuSkinningMethod::DualQuaternion &&
                    j.paletteBase + j.paletteCount <= snap.palettes.size()) {
                    g.mode = 1;
                    g.paletteBase = u32(dq.size() / 2);
                    for (u32 k = 0; k < j.paletteCount; ++k) {
                        glm::vec4 r, d;
                        toDualQuat(snap.palettes[j.paletteBase + k], r, d);
                        dq.push_back(r);
                        dq.push_back(d);
                    }
                }
            }
            jobs.push_back(g);
            maxVertices = std::max(maxVertices, j.vertexCount);
            if (!j.previousOnly) m_outputs.items.push_back({j.entityId, j.meshInfo, j.vertexCount, u64(j.outBase) * sizeof(GpuSkinnedVertex),
                                       u64(j.prevBase) * sizeof(GpuSkinnedVertex)});
        }
        if (dq.empty()) dq.push_back(glm::vec4(0.0f));
        const VkDeviceAddress jobsAddr = ctx.upload(std::span<const SkinJobGpu>(jobs));
        const VkDeviceAddress dqAddr = ctx.upload(std::span<const glm::vec4>(dq));
        const VkDeviceAddress sceneAddr = ctx.sceneAddress();
        const u32 jobCount = u32(jobs.size());
        const rhi::RGBuffer out = ctx.graph().importBuffer(
            m_arena.buffer(), {m_arena.capacity() * sizeof(GpuSkinnedVertex), rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly,
                               "SkinnedVertices"},
            {rhi::Access::General, rhi::Access::General});
        const VkDeviceAddress outAddr = m_arena.address();
        const rhi::PipelineHandle pipe = m_pipeline;
        ctx.graph()
            .addPass("Skinning", rhi::PassType::Compute)
            .write(out, rhi::Access::StorageWriteCompute)
            .sideEffect()
            .execute([pipe, sceneAddr, jobsAddr, dqAddr, outAddr, jobCount, maxVertices](rhi::PassContext& p) {
                p.cmd.bindPipeline(pipe);
                p.cmd.pushConstants(SkinPush{sceneAddr, jobsAddr, dqAddr, outAddr, jobCount, 0});
                p.cmd.dispatch((maxVertices + 63) / 64, jobCount);
                // Core geometry passes read the output through buffer addresses without declaring it.
                p.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::General);
            });
        ctx.resources().setBuffer(kSkinnedVerticesResource, out);
    }

private:
    std::optional<rt::DeformedGeometry> deformedGeometry(u32 gpuInstance) const {
        const std::vector<GpuInstance>& instances = m_scene->instances();
        if (gpuInstance >= instances.size()) return std::nullopt;
        const GpuInstance& g = instances[gpuInstance];
        if (!(g.flags & kInstanceSkinnedOutput)) return std::nullopt;
        auto it = m_allocs.find(g.entityId);
        if (it == m_allocs.end()) return std::nullopt;
        rt::DeformedGeometry d;
        d.positions = m_arena.buffer();
        d.offset = u64(g.paletteOffset) * sizeof(GpuSkinnedVertex);
        d.vertexCount = it->second.vertexCount;
        d.stride = sizeof(GpuSkinnedVertex);
        d.version = it->second.stamp;
        return d;
    }

    struct Alloc {
        u64 base = 0;
        u32 vertexCount = 0;
        u32 meshInfo = 0;
        u32 parity = 0;
        u64 stamp = 0;
        u32 cur = 0, prev = 0;
    };
    struct Job {
        u32 entityId, meshInfo, vertexBase, vertexCount, skinBase, paletteBase, paletteCount, outBase, prevBase;
        bool previousOnly = false; // fills the previous-frame half (not an output of its own)
    };
    struct PendingFree {
        u64 stamp, offset, count;
    };

    bool resolve(const SnapshotMesh& sm, const GpuMesh& gm, u32 submesh, u64 stamp, u32& cur, u32& prev) {
        (void)submesh;
        if (!WorldCVars::read().computeSkinning || !FeatureRegistry::toggle("Skinning").get()) return false;
        if (stamp != m_jobsStamp) {
            m_jobsStamp = stamp;
            m_jobs.clear();
        }
        const GpuMeshInfo& mi = m_scene->meshInfo(gm.firstMeshInfo);
        if (mi.skinOffset == kInvalidIndex || gm.vertexCount == 0) return false;
        Alloc& a = m_allocs[sm.entityId];
        if (a.stamp == stamp) { // another submesh of the same entity
            cur = a.cur;
            prev = a.prev;
            return true;
        }
        bool prevValid = a.stamp + 1 == stamp;
        if (a.vertexCount != gm.vertexCount || a.meshInfo != gm.firstMeshInfo) {
            if (a.vertexCount) m_pendingFrees.push_back({stamp, a.base, u64(a.vertexCount) * 2});
            a.vertexCount = gm.vertexCount;
            a.meshInfo = gm.firstMeshInfo;
            a.base = m_arena.allocate(*m_device, u64(gm.vertexCount) * 2);
            m_scene->setSkinnedVertexAddress(m_arena.address()); // may have grown
            a.parity = 0;
            prevValid = false;
        } else {
            a.parity ^= 1u;
        }
        a.stamp = stamp;
        a.cur = u32(a.base + u64(a.parity) * a.vertexCount);
        a.prev = prevValid ? u32(a.base + u64(a.parity ^ 1u) * a.vertexCount) : a.cur;
        m_jobs.push_back({sm.entityId, gm.firstMeshInfo, u32(mi.vertexOffset), gm.vertexCount, mi.skinOffset, sm.paletteOffset,
                          sm.paletteCount, a.cur, a.prev});
        if (!prevValid && sm.prevPaletteOffset != ~0u && sm.prevPaletteOffset != sm.paletteOffset) {
            // No output from the previous frame (first frame, re-allocation): skin the snapshot's previous palette into
            // the other half so motion vectors are right from the start.
            a.prev = u32(a.base + u64(a.parity ^ 1u) * a.vertexCount);
            m_jobs.back().prevBase = a.prev;
            m_jobs.push_back({sm.entityId, gm.firstMeshInfo, u32(mi.vertexOffset), gm.vertexCount, mi.skinOffset,
                              sm.prevPaletteOffset, sm.paletteCount, a.prev, a.prev, true});
        }
        cur = a.cur;
        prev = a.prev;
        return true;
    }

    rhi::Device* m_device = nullptr;
    GpuScene* m_scene = nullptr;
    rt::RayTracingSceneApi* m_rtScene = nullptr;
    rhi::PipelineHandle m_pipeline;
    GrowableBuffer m_arena;
    std::unordered_map<u32, Alloc> m_allocs;
    std::vector<Job> m_jobs;
    std::vector<PendingFree> m_pendingFrees;
    u64 m_jobsStamp = 0;
    u64 m_setupStamp = ~0ull;
    SkinnedOutputs m_outputs;
};

} // namespace

std::unique_ptr<IRenderFeature> makeSkinningFeature() { return std::make_unique<SkinningFeature>(); }

} // namespace ox::render::worldfx
