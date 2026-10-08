#include "gpu_driven.hpp"

#include <oxwald/render/features/gpu_driven/gpu_driven.hpp>

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/scalability.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace ox::render {

namespace {

using S = Scalability;

CVar<bool> cvGpuDriven("r.GpuDriven", true,
                       "GPU-driven rendering: compute culling + indirect draws for the opaque, prepass and shadow passes "
                       "(0 = CPU frustum culling + instanced draws)");
CVar<bool> cvOcclusion("r.GpuDriven.Occlusion", true, "Two-phase HiZ occlusion culling of camera views");
CVar<bool> cvShadowCulling("r.GpuDriven.Shadows", true, "Cull shadow cascades / lights on the GPU");
CVar<bool> cvMeshlets("r.GpuDriven.Meshlets", false,
                      "Meshlet (cluster) culling of meshes with meshlets into a compacted index buffer (camera views)");
CVar<int> cvParallelRecording("r.ParallelRecording", 2,
                               "Record large CPU-path passes (depth prepass, forward opaque) into secondary command lists "
                               "on the job system (needs RendererDesc::jobs): Off, On, Auto (off on portability "
                               "implementations: MoltenVK replays secondaries serially at submit, see docs/dev/perf.md)",
                               CVarEnum{"Off", "On", "Auto"});
CVar<int> cvParallelMinBatches("r.ParallelRecording.MinBatches", 256,
                               "Minimum instanced batches in a pass before it is recorded in parallel", 16, 100000);
CVar<bool> cvMeshShaders("r.GpuDriven.MeshShaders", false,
                          "Meshlet path through VK_EXT_mesh_shader task/mesh shaders instead of compute expansion "
                          "(only on devices with mesh + task shaders; needs r.GpuDriven.Meshlets)");
CVar<int> cvMeshletBudget("r.GpuDriven.MeshletIndexBudgetMB", 64,
                          "Compacted meshlet index buffer per view and phase (MiB); meshlets beyond it are dropped", 4, 1024);
CVar<int> cvDrawCount("r.GpuDriven.DrawCount", 0, "Indirect draw count source",
                      CVarEnum{"Auto", "MaxCount", "IndirectCount"});
CVar<float> cvLodError("r.GpuDriven.LODErrorPixels", 1.0f,
                       "Screen-space LOD error threshold in pixels (LOD switch distance; scaled by 2^r.ViewDistance.LODBias)",
                       S::ViewDistance, {2.0f, 1.5f, 1.0f, 0.75f});

CVar<int> cvAsyncCompute("r.AsyncCompute", 2,
                          "Async-safe compute passes (light culling, HiZ, features using asyncComputeHint()) on the async "
                          "compute queue: Off, On, Auto (on unless the device is a portability implementation such as MoltenVK)",
                          CVarEnum{"Off", "On", "Auto"});

constexpr u64 kCommandSize = sizeof(VkDrawIndexedIndirectCommand); // 20
constexpr u64 kArenaChunk = 4ull << 20;
constexpr u32 kGroup = 64;

struct CullPush {
    u64 view = 0;
    u64 scene = 0;
    u64 job = 0;
    u64 lods = 0;
    u32 stage = 0;
    u32 pad = 0;
};

DrawBucket bucketOf(const GpuMaterial& m) {
    switch (m.flags & kMaterialBlendMask) {
    case 1: return DrawBucket::Masked;
    case 2: return DrawBucket::Transparent;
    case 3: return DrawBucket::Refractive;
    default: return DrawBucket::Opaque;
    }
}

constexpr u32 kGpuBuckets = (1u << u32(DrawBucket::Opaque)) | (1u << u32(DrawBucket::Masked));

} // namespace

rhi::QueueType asyncComputeHint(const rhi::DeviceCaps& caps) {
    const i32 mode = cvAsyncCompute;
    const bool on = caps.asyncComputeQueue && (mode == 1 || (mode == 2 && !caps.portabilitySubset));
    return on ? rhi::QueueType::Compute : rhi::QueueType::Graphics;
}

GpuDrivenSettings GpuDrivenSettings::fromCVars(const rhi::DeviceCaps& caps) {
    GpuDrivenSettings s;
    s.enabled = cvGpuDriven;
    s.occlusion = cvOcclusion;
    s.shadows = cvShadowCulling;
    s.meshlets = cvMeshlets;
    s.drawCountMode = cvDrawCount;
    s.lodErrorPixels = std::max(cvLodError.get(), 0.01f);
    s.meshShaders = cvMeshShaders && caps.meshShader && caps.taskShader;
    s.parallelRecording = cvParallelRecording == 1 || (cvParallelRecording == 2 && !caps.portabilitySubset);
    s.parallelMinBatches = cvParallelMinBatches;
    return s;
}

struct GpuDriven::DrawSet {
    bool shadow = false;
    u64 version = 0;
    std::vector<glm::uvec2> items;
    std::vector<GpuCullBatch> batches;
    std::vector<DrawIndirectRun> runs;
    std::vector<DrawBucket> runBucket;
    u32 commandCount = 0;
    u32 idCount = 0;
    rhi::BufferHandle itemBuffer, batchBuffer;
    u64 itemCapacity = 0, batchCapacity = 0;
    GpuAllocation pendingItems, pendingBatches;
    bool pending = false;
    // Meshlet path bounds (main set): LOD 0 indices per variant, meshlets.
    u64 meshletIndexBound[kVariantCount]{};
    u64 meshletBound = 0;
};

struct GpuDriven::PassJobs {
    struct Job {
        GpuAllocation params; // GpuCullJob in frame memory
        u32 itemCount = 0, batchCount = 0, runCount = 0;
        bool resetVisibility = false;
        GpuAllocation meshletParams; // GpuMeshletJob (meshlet path)
        GpuAllocation meshletDispatch;  // dispatch args (compute expansion) = task counts (mesh shader path)
        bool meshShaderPath = false;    // only the PREPARE stage runs; the task shader culls
    };
    std::vector<Job> jobs;
};

struct GpuDriven::ViewState {
    struct Chunk {
        rhi::BufferHandle buffer;
        VkDeviceAddress address = 0;
        u64 size = 0, used = 0;
    };
    std::vector<Chunk> chunks;
    rhi::BufferHandle visibility;
    u64 visibilityCapacity = 0;
    u64 visibilityVersion = 0;
    std::shared_ptr<PassJobs> early, late;
    bool lateActive = false;
    GpuAllocation lateParams, lateMeshletParams;
    u64 lastFrame = ~0ull;
    struct MeshletDraw {
        bool valid = false;
        GpuAllocation commands, indices;
        VkDeviceAddress visible = 0;
        // Mesh shader path.
        bool meshShaders = false;
        GpuAllocation taskArgs;
        VkDeviceAddress instances = 0;
        u32 hiz = kInvalidIndex, hizMips = 0;
        glm::vec2 hizSize{0.0f};
    };
    MeshletDraw meshlets[2]; // early, late
};

GpuDriven::GpuDriven(rhi::Device& device, GpuScene& scene, GpuFrameAllocator& frameAlloc, JobSystem* jobs)
    : m_device(&device), m_scene(&scene), m_frameAlloc(&frameAlloc), m_jobs(jobs) {
    const u32 frames = device.framesInFlight();
    m_readback.resize(frames);
    m_readbackFrame.assign(frames, 0);
    for (u32 i = 0; i < frames; ++i) {
        const u64 size = u64(kCounterSlotsPerKind) * kCullKinds * 4;
        m_readback[i] = device.createBuffer({size, rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst,
                                             rhi::MemoryUsage::Readback, "gpuDriven.counters"});
        if (void* p = device.mapped(m_readback[i])) std::memset(p, 0, size);
    }
}

GpuDriven::~GpuDriven() {
    rhi::Device& d = *m_device;
    for (auto& [id, v] : m_views) {
        for (auto& c : v->chunks) d.destroy(c.buffer);
        if (v->visibility) d.destroy(v->visibility);
    }
    for (auto& s : m_sets) {
        if (!s) continue;
        if (s->itemBuffer) d.destroy(s->itemBuffer);
        if (s->batchBuffer) d.destroy(s->batchBuffer);
    }
    for (auto b : m_readback) d.destroy(b);
    for (auto p : {m_cull, m_hiz, m_meshletCull, m_meshletPrepare}) {
        if (p) d.destroy(p);
    }
    for (auto& row : m_meshletPrepass)
        for (auto p : row)
            if (p) d.destroy(p);
    for (auto p : m_meshletForward)
        if (p) d.destroy(p);
    for (auto& row : m_meshPrepass)
        for (auto p : row)
            if (p) d.destroy(p);
    for (auto p : m_meshForward)
        if (p) d.destroy(p);
}

void GpuDriven::ensurePipelines() {
    if (m_pipelinesTried) return;
    m_pipelinesTried = true;
    m_cull = createComputePipeline(*m_device, "gpuDriven.cull", "render/gpu_driven/cull.comp");
    m_hiz = createComputePipeline(*m_device, "gpuDriven.hizEarly", "render/gpu_driven/hiz_early.comp");
}

void GpuDriven::beginFrame(RenderStats& stats) {
    m_settings = GpuDrivenSettings::fromCVars(m_device->caps());
    rhi::Device& dev = *m_device;
    ensurePipelines();
    // The device rejects indirect draws with firstInstance != 0 without drawIndirectFirstInstance; every desktop
    // driver and MoltenVK support it, but stay safe.
    m_active = m_settings.enabled && m_cull && dev.vkPipeline(m_cull) != VK_NULL_HANDLE;

    // Counters of the frame that used this slot (retired: Device::beginFrame waited for it).
    const u32 slot = dev.frameIndex() % u32(m_readback.size());
    auto* c = static_cast<u32*>(dev.mapped(m_readback[slot]));
    if (c && m_readbackFrame[slot] != 0) {
        const u32* m = c + kCullKindMain * kCounterSlotsPerKind;
        const u32* s = c + kCullKindShadow * kCounterSlotsPerKind;
        stats.gpuCulling.valid = true;
        stats.gpuCulling.instancesTested = m[kCounterTested];
        stats.gpuCulling.instancesFrustumCulled = m[kCounterFrustumCulled];
        stats.gpuCulling.instancesOccluded = m[kCounterOccluded];
        stats.gpuCulling.instancesVisible = m[kCounterVisible];
        stats.gpuCulling.drawCommands = m[kCounterDraws];
        stats.gpuCulling.triangles = m[kCounterTriangles];
        stats.gpuCulling.meshletsTested = m[kCounterMeshletsTested];
        stats.gpuCulling.meshletsVisible = m[kCounterMeshletsVisible];
        stats.gpuCulling.meshletTriangles = m[kCounterMeshletTriangles];
        stats.gpuCulling.averageLod = m[kCounterVisible] ? f32(m[kCounterLodSum]) / f32(m[kCounterVisible]) : 0.0f;
        stats.gpuCulling.shadowInstancesTested = s[kCounterTested];
        stats.gpuCulling.shadowInstancesVisible = s[kCounterVisible];
        stats.gpuCulling.shadowDrawCommands = s[kCounterDraws];
        stats.gpuCulling.shadowTriangles = s[kCounterTriangles];
        stats.gpuCulling.latencyFrames = u32(dev.frameNumber() - m_readbackFrame[slot]);
    }
    if (c) std::memset(c, 0, u64(kCounterSlotsPerKind) * kCullKinds * 4);
    m_readbackFrame[slot] = dev.frameNumber();
}

GpuDriven::ViewState& GpuDriven::viewState(ViewId id) {
    auto& v = m_views[id];
    if (!v) v = std::make_unique<ViewState>();
    return *v;
}

void GpuDriven::releaseView(ViewId id) {
    auto it = m_views.find(id);
    if (it == m_views.end()) return;
    for (auto& c : it->second->chunks) m_device->destroy(c.buffer);
    if (it->second->visibility) m_device->destroy(it->second->visibility);
    m_views.erase(it);
}

GpuAllocation GpuDriven::arenaAllocate(ViewState& vs, u64 size, u64 alignment) {
    size = std::max<u64>(size, 4);
    alignment = std::max<u64>(alignment, 16);
    for (auto& c : vs.chunks) {
        const u64 off = (c.used + alignment - 1) & ~(alignment - 1);
        if (off + size <= c.size) {
            c.used = off + size;
            return {nullptr, c.address + off, size, c.buffer, off};
        }
    }
    ViewState::Chunk c;
    c.size = std::max(kArenaChunk, size);
    using U = rhi::BufferUsage;
    c.buffer = m_device->createBuffer(
        {c.size, U::Storage | U::Indirect | U::Index | U::TransferDst | U::TransferSrc, rhi::MemoryUsage::GpuOnly,
         "gpuDriven.arena"});
    c.address = m_device->address(c.buffer);
    c.used = size;
    vs.chunks.push_back(c);
    return {nullptr, c.address, size, c.buffer, 0};
}

GpuDriven::DrawSet& GpuDriven::drawSet(bool shadow, FrameState& fs) {
    auto& slot = m_sets[shadow ? 1 : 0];
    if (!slot) {
        slot = std::make_unique<DrawSet>();
        slot->shadow = shadow;
    }
    DrawSet& set = *slot;
    const GpuScene& scene = *m_scene;
    if (set.version == scene.structureVersion()) return set;
    OX_PROFILE_ZONE_N("GpuDriven::rebuildDrawSet");
    set.version = scene.structureVersion();
    set.items.clear();
    set.batches.clear();
    set.runs.clear();
    set.runBucket.clear();
    set.commandCount = 0;
    set.idCount = 0;
    for (u64& b : set.meshletIndexBound) b = 0;
    set.meshletBound = 0;

    // Sort candidates by (bucket, variant, mesh, material): same grouping as the CPU draw lists (shadow sets batch
    // opaque casters by mesh only).
    struct Key {
        u64 key;
        u32 instance;
    };
    std::vector<Key> keys;
    const auto& instances = scene.instances();
    const GpuMaterial* materials = scene.materials();
    keys.reserve(instances.size());
    for (u32 i = 0; i < instances.size(); ++i) {
        const GpuInstance& inst = instances[i];
        if (!(inst.flags & kInstanceVisible)) continue;
        if (shadow && !(inst.flags & kInstanceCastShadows)) continue;
        const GpuMaterial& mat = materials[inst.materialIndex];
        const DrawBucket bucket = bucketOf(mat);
        if (!(kGpuBuckets & (1u << u32(bucket)))) continue;
        const u32 variant = (bucket == DrawBucket::Masked ? kVariantAlphaTest : 0u) |
                            ((mat.flags & kMaterialDoubleSided) ? kVariantDoubleSided : 0u);
        const u32 material = (shadow && bucket != DrawBucket::Masked) ? 0u : inst.materialIndex;
        const u64 key = (u64(bucket) << 62) | (u64(variant) << 60) | (u64(inst.meshIndex & 0xFFFFFFFu) << 32) | material;
        keys.push_back({key, i});
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.key != b.key ? a.key < b.key : a.instance < b.instance; });
    set.items.resize(keys.size());
    for (usize i = 0; i < keys.size();) {
        usize j = i + 1;
        while (j < keys.size() && keys[j].key == keys[i].key) ++j;
        const DrawBucket bucket = DrawBucket(keys[i].key >> 62);
        const u32 variant = u32(keys[i].key >> 60) & 3u;
        const GpuInstance& inst = instances[keys[i].instance];
        if (set.runs.empty() || set.runBucket.back() != bucket || set.runs.back().variant != variant) {
            DrawIndirectRun r;
            r.firstCommand = set.commandCount;
            r.variant = variant;
            r.countSlot = u32(set.runs.size());
            set.runs.push_back(r);
            set.runBucket.push_back(bucket);
        }
        GpuCullBatch b;
        b.meshIndex = inst.meshIndex;
        b.lodCount = std::clamp<u32>(scene.meshInfo(inst.meshIndex).lodCount, 1, kMaxMeshLods);
        b.firstCommand = set.commandCount;
        b.idBase = set.idCount;
        b.capacity = u32(j - i);
        b.run = u32(set.runs.size() - 1);
        b.runFirst = set.runs.back().firstCommand;
        const u32 batchIndex = u32(set.batches.size());
        set.batches.push_back(b);
        set.commandCount += b.lodCount;
        set.idCount += b.capacity;
        set.runs.back().commandCount += b.lodCount;
        for (usize k = i; k < j; ++k) set.items[k] = {keys[k].instance, batchIndex};
        if (!shadow) {
            const GpuMeshLod& l0 = scene.meshLods(inst.meshIndex)[0];
            u32 maxMeshlets = 0;
            for (u32 l = 0; l < b.lodCount; ++l) maxMeshlets = std::max(maxMeshlets, scene.meshLods(inst.meshIndex)[l].meshletCount);
            if (l0.meshletCount > 0) {
                u64 skinned = 0;
                for (usize k = i; k < j; ++k) skinned += (instances[keys[k].instance].flags & kInstanceSkinned) ? 1 : 0;
                set.meshletIndexBound[variant] += u64(l0.indexCount) * (j - i - skinned);
                set.meshletBound += u64(maxMeshlets) * (j - i - skinned);
            }
        }
        i = j;
    }

    rhi::Device& dev = *m_device;
    auto ensure = [&](rhi::BufferHandle& buf, u64& cap, u64 bytes, const char* name) {
        bytes = std::max<u64>(bytes, 16);
        if (buf && cap >= bytes) return;
        if (buf) dev.destroy(buf);
        cap = std::max(bytes, cap * 2);
        buf = dev.createBuffer({cap, rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuOnly, name});
    };
    ensure(set.itemBuffer, set.itemCapacity, set.items.size() * sizeof(glm::uvec2), "gpuDriven.items");
    ensure(set.batchBuffer, set.batchCapacity, set.batches.size() * sizeof(GpuCullBatch), "gpuDriven.batches");
    // Staged now, copied by the next culling pass (before any job reads it).
    GpuFrameAllocator& fa = fs.r->frameAlloc;
    set.pendingItems = fa.allocate(std::max<u64>(set.items.size() * sizeof(glm::uvec2), 16), 16);
    if (!set.items.empty()) std::memcpy(set.pendingItems.cpu, set.items.data(), set.items.size() * sizeof(glm::uvec2));
    set.pendingBatches = fa.allocate(std::max<u64>(set.batches.size() * sizeof(GpuCullBatch), 16), 16);
    if (!set.batches.empty()) std::memcpy(set.pendingBatches.cpu, set.batches.data(), set.batches.size() * sizeof(GpuCullBatch));
    set.pending = true;
    return set;
}

void GpuDriven::fillJob(FrameState& fs, ViewState& vs, DrawSet& set, GpuCullJob& job, u32 kind) {
    rhi::Device& dev = *m_device;
    job.items = dev.address(set.itemBuffer);
    job.batches = dev.address(set.batchBuffer);
    job.itemCount = u32(set.items.size());
    job.batchCount = u32(set.batches.size());
    job.runCount = u32(set.runs.size());
    job.commands = arenaAllocate(vs, set.commandCount * kCommandSize, 16).address;
    job.ids = arenaAllocate(vs, u64(set.idCount) * 4, 16).address;
    job.scratch = arenaAllocate(vs, u64(set.items.size()) * 8, 16).address;
    job.runCounts = arenaAllocate(vs, u64(set.runs.size()) * 4, 16).address;
    job.counters = dev.address(m_readback[dev.frameIndex() % u32(m_readback.size())]) + u64(kind) * kCounterSlotsPerKind * 4;
    (void)fs;
}

void GpuDriven::makeList(DrawList& list, const DrawSet& set, const GpuCullJob& job, u32 bucketMask, u64 commandsOffset,
                         rhi::BufferHandle buffer, u64 countOffset, u32 multiplier) const {
    // Reuses `list`'s capacity (steady state: no heap allocation).
    list.batches.clear();
    list.indirectRuns.clear();
    list.indirectCountBuffer = {};
    list.indirectCountOffset = 0;
    list.instanceCount = 0;
    list.triangleCount = 0;
    list.instanceIds = job.ids;
    list.indirectBuffer = buffer;
    list.indirectOffset = commandsOffset;
    list.indirectMultiplier = multiplier;
    const bool useCount = (job.flags & kCullCompact) != 0;
    for (usize r = 0; r < set.runs.size(); ++r) {
        if (!(bucketMask & (1u << u32(set.runBucket[r])))) continue;
        DrawIndirectRun run = set.runs[r];
        if (!useCount) run.countSlot = ~0u;
        list.indirectRuns.push_back(run);
    }
    if (useCount && !list.indirectRuns.empty()) {
        list.indirectCountBuffer = buffer;
        list.indirectCountOffset = countOffset;
    }
}

namespace {

// Address → (buffer, offset) of an arena allocation.
struct ArenaRef {
    rhi::BufferHandle buffer;
    u64 offset = 0;
};

} // namespace

void GpuDriven::setupView(FrameState& fs) {
    fs.gpuDriven = false;
    if (!m_active) return;
    OX_PROFILE_ZONE_N("GpuDriven::setupView");
    RenderView& view = *fs.view;
    ViewState& vs = viewState(view.id());
    for (auto& c : vs.chunks) c.used = 0;
    // Pass job lists persist per view (capacity reuse); the previous frame's graph already executed.
    if (!vs.early) vs.early = std::make_shared<PassJobs>();
    vs.early->jobs.clear();
    vs.lateActive = false;
    vs.lateParams = vs.lateMeshletParams = {};
    vs.lastFrame = view.frameIndex();
    DrawSet& set = drawSet(false, fs);
    fs.gpuDriven = true;

    const RenderSettings& st = fs.settings;
    const CameraParams& cam = view.camera();
    const f32 drawDistance = st.drawDistance > 0.0f ? st.drawDistance : (cam.farPlane > 0.0f ? cam.farPlane : 0.0f);
    const Frustum frustum = view.frustum();
    const bool twoPhase = m_settings.occlusion && st.frustumCulling;

    GpuCullJob job;
    for (u32 p = 0; p < 6; ++p) job.planes[p] = glm::vec4(frustum.planes[p].normal, frustum.planes[p].d);
    job.flags = (st.frustumCulling ? kCullFrustum : 0u) | (drawDistance > 0.0f ? kCullDistance : 0u);
    job.drawDistance = drawDistance;
    const LodSelection lod = FeatureContext(fs, nullptr, InjectionPoint::PreDepth).lodSelection();
    job.lodCamera = glm::vec4(lod.cameraPosition, lod.projScale);
    job.lodThreshold = lod.thresholdPixels;
    job.lodOrtho = lod.orthographic ? 1u : 0u;
    job.viewProj = fs.constants.viewProj;
    const bool compact = m_device->caps().drawIndirectCount && m_settings.drawCountMode != 1;
    if (compact) job.flags |= kCullCompact;

    bool resetVisibility = false;
    if (twoPhase) {
        const u64 bytes = std::max<u64>(set.items.size() * 4, 16);
        if (!vs.visibility || vs.visibilityCapacity < bytes) {
            if (vs.visibility) m_device->destroy(vs.visibility);
            vs.visibilityCapacity = std::max(bytes, vs.visibilityCapacity * 2);
            vs.visibility = m_device->createBuffer({vs.visibilityCapacity, rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst,
                                                    rhi::MemoryUsage::GpuOnly, "gpuDriven.visibility"});
            resetVisibility = true;
        }
        if (vs.visibilityVersion != set.version) resetVisibility = true;
        vs.visibilityVersion = set.version;
        job.visibility = m_device->address(vs.visibility);
    }

    const bool meshlets = m_settings.meshlets && set.meshletBound > 0 && ensureMeshletPipelines();
    const bool meshShaderPath = meshlets && m_settings.meshShaders && ensureMeshShaderPipelines();
    vs.meshlets[0] = vs.meshlets[1] = {};
    auto addJob = [&](PassJobs& pass, GpuCullJob j, bool resetVis) -> std::pair<GpuCullJob, GpuAllocation> {
        fillJob(fs, vs, set, j, kCullKindMain);
        if (compact) j.compacted = arenaAllocate(vs, set.commandCount * kCommandSize, 16).address;
        if (resetVis) j.flags |= kCullResetVisibility;
        PassJobs::Job rec;
        if (meshlets) {
            const bool late = (j.flags & kCullLate) != 0;
            j.flags |= kCullMeshlets;
            j.meshletCapacity = u32(set.items.size());
            j.meshletInstances = arenaAllocate(vs, 8 + u64(set.items.size()) * 8, 16).address;
            GpuMeshletJob mj;
            std::copy(std::begin(j.planes), std::end(j.planes), std::begin(mj.planes));
            mj.viewProj = fs.constants.viewProj;
            mj.camera = glm::vec4(glm::vec3(fs.constants.cameraPosition), (fs.constants.flags & 1u) ? 1.0f : 0.0f);
            mj.instances = j.meshletInstances;
            rec.meshletDispatch = arenaAllocate(vs, 16, 16);
            mj.dispatch = rec.meshletDispatch.address;
            ViewState::MeshletDraw& md = vs.meshlets[late ? 1 : 0];
            md.meshShaders = meshShaderPath;
            md.taskArgs = rec.meshletDispatch;
            md.instances = j.meshletInstances;
            md.hizMips = j.hizMips;
            md.hizSize = j.hizSize;
            rec.meshShaderPath = meshShaderPath;
            md.commands = arenaAllocate(vs, kVariantCount * kCommandSize, 16);
            mj.commands = md.commands.address;
            // Index regions per variant, bounded by LOD 0 and scaled down to the budget.
            const u64 budget = u64(std::max(cvMeshletBudget.get(), 4)) << 18; // MiB → u32 indices
            u64 total = 0;
            for (u64 b : set.meshletIndexBound) total += b;
            const f64 scale = total > budget ? f64(budget) / f64(total) : 1.0;
            u32 base = 0;
            for (u32 v = 0; v < kVariantCount; ++v) {
                const u32 cap = u32(f64(set.meshletIndexBound[v]) * scale) / 3 * 3;
                mj.variantBase[v] = base;
                mj.variantCapacity[v] = cap;
                base += cap;
            }
            md.indices = arenaAllocate(vs, std::max<u64>(base, 1) * 4, 16);
            mj.indices = md.indices.address;
            mj.visibleCapacity = u32(std::min<u64>(set.meshletBound, 1u << 25));
            md.visible = arenaAllocate(vs, u64(mj.visibleCapacity) * 8, 16).address;
            mj.visible = md.visible;
            mj.counters = j.counters;
            mj.instanceCapacity = j.meshletCapacity;
            mj.flags = late ? kCullLate : 0u;
            mj.hizMips = j.hizMips;
            mj.hizSize = j.hizSize;
            md.valid = true;
            rec.meshletParams = fs.r->frameAlloc.allocate(sizeof(GpuMeshletJob), 16);
            std::memcpy(rec.meshletParams.cpu, &mj, sizeof(mj));
            if (late) vs.lateMeshletParams = rec.meshletParams;
        }
        GpuAllocation a = fs.r->frameAlloc.allocate(sizeof(GpuCullJob), 16);
        std::memcpy(a.cpu, &j, sizeof(j));
        rec.params = a;
        rec.itemCount = j.itemCount;
        rec.batchCount = j.batchCount;
        rec.runCount = j.runCount;
        rec.resetVisibility = resetVis;
        pass.jobs.push_back(rec);
        return {j, a};
    };
    auto listsFor = [&](const GpuCullJob& j, ViewDrawLists& out) {
        // Commands (or compacted commands) and run counts live in the arena chunk of the respective allocation.
        auto locate = [&](VkDeviceAddress addr) -> ArenaRef {
            for (auto& c : vs.chunks) {
                if (addr >= c.address && addr < c.address + c.size) return {c.buffer, addr - c.address};
            }
            return {};
        };
        const ArenaRef cmds = locate((j.flags & kCullCompact) ? j.compacted : j.commands);
        const ArenaRef counts = locate(j.runCounts);
        for (DrawBucket b : {DrawBucket::Opaque, DrawBucket::Masked}) {
            DrawList& l = out.buckets[u32(b)];
            makeList(l, set, j, 1u << u32(b), cmds.offset, cmds.buffer, counts.offset, 1);
            if (l.indirectCountBuffer) l.indirectCountBuffer = counts.buffer;
        }
    };

    GpuCullJob early = job;
    if (twoPhase) early.flags |= kCullEarly;
    auto [earlyJob, earlyAlloc] = addJob(*vs.early, early, resetVisibility);
    listsFor(earlyJob, fs.gpuEarly);
    fs.gpuLateActive = false;
    if (twoPhase) {
        if (!vs.late) vs.late = std::make_shared<PassJobs>();
        vs.late->jobs.clear();
        vs.lateActive = true;
        GpuCullJob late = job;
        late.flags |= kCullLate;
        // Half-resolution pyramid (hiz_early.comp): fullMipCount - 1 stored mips.
        late.hizMips = std::max(rhi::fullMipCount(view.renderExtent().width, view.renderExtent().height), 2u) - 1;
        late.hizSize = glm::vec2(f32(view.renderExtent().width), f32(view.renderExtent().height));
        auto [lateJob, lateAlloc] = addJob(*vs.late, late, false);
        vs.lateParams = lateAlloc;
        listsFor(lateJob, fs.gpuLate);
        fs.gpuLateActive = true;
    }

    const VkDeviceAddress viewAddr = fs.constantsAlloc.address, sceneAddr = fs.headerAlloc.address;
    std::shared_ptr<PassJobs> pass = vs.early;
    fs.view->graph().addPass("GpuCull", rhi::PassType::Compute).sideEffect().execute(
        [this, pass, viewAddr, sceneAddr](rhi::PassContext& ctx) { recordJobs(ctx.cmd, *pass, viewAddr, sceneAddr); });
}

bool GpuDriven::declareLate(FrameState& fs, rhi::RGTexture depth) {
    if (!fs.gpuDriven || !fs.gpuLateActive) return false;
    ViewState& vs = viewState(fs.view->id());
    if (!vs.late || !vs.lateActive) return false;
    rhi::RenderGraph& graph = fs.view->graph();
    const Extent2D re = fs.view->renderExtent();
    const u32 mips = std::max(rhi::fullMipCount(re.width, re.height), 2u) - 1;
    rhi::TextureDesc hd;
    hd.format = formats::kHiZ;
    hd.width = std::max(re.width >> 1, 1u);
    hd.height = std::max(re.height >> 1, 1u);
    hd.mipLevels = mips;
    hd.usage = rhi::TextureUsage::None;
    hd.name = "HiZEarly";
    const rhi::RGTexture hiz = graph.createTexture(hd);
    const rhi::PipelineHandle hizPipe = m_hiz;
    graph.addPass("HiZ.Early", rhi::PassType::Compute)
        .read(depth, rhi::Access::SampledCompute)
        .overwrite(hiz, rhi::Access::StorageWriteCompute)
        .execute([hizPipe, depth, hiz, re, mips](rhi::PassContext& ctx) {
            struct {
                u32 src, dst;
                u32 srcSize[2], dstSize[2];
                u32 fromDepth;
            } pc{};
            ctx.cmd.bindPipeline(hizPipe);
            u32 w = re.width, h = re.height;
            for (u32 mip = 0; mip < mips; ++mip) {
                const u32 dw = std::max(w >> 1, 1u), dh = std::max(h >> 1, 1u);
                pc.src = mip == 0 ? ctx.sampledIndex(depth) : ctx.storageIndex(hiz, mip - 1);
                pc.dst = ctx.storageIndex(hiz, mip);
                pc.srcSize[0] = w, pc.srcSize[1] = h, pc.dstSize[0] = dw, pc.dstSize[1] = dh;
                pc.fromDepth = mip == 0 ? 1u : 0u;
                ctx.cmd.pushConstants(pc);
                ctx.cmd.dispatch((dw + 7) / 8, (dh + 7) / 8);
                ctx.cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
                w = dw, h = dh;
            }
        });
    const VkDeviceAddress viewAddr = fs.constantsAlloc.address, sceneAddr = fs.headerAlloc.address;
    std::shared_ptr<PassJobs> pass = vs.late;
    GpuAllocation params = vs.lateParams, meshletParams = vs.lateMeshletParams;
    graph.addPass("GpuCull.Late", rhi::PassType::Compute)
        .read(hiz, rhi::Access::SampledCompute)
        .sideEffect()
        .execute([this, pass, params, meshletParams, hiz, viewAddr, sceneAddr, viewId = fs.view->id()](rhi::PassContext& ctx) {
            auto* job = static_cast<GpuCullJob*>(params.cpu);
            job->hiz = ctx.sampledIndex(hiz);
            if (meshletParams.cpu) static_cast<GpuMeshletJob*>(meshletParams.cpu)->hiz = job->hiz;
            if (auto it = m_views.find(viewId); it != m_views.end()) it->second->meshlets[1].hiz = job->hiz;
            recordJobs(ctx.cmd, *pass, viewAddr, sceneAddr);
        });
    return true;
}

DrawList GpuDriven::cull(FrameState& fs, const DrawFilter& filter, u32 multiplier) {
    auto cpu = [&] {
        FeatureContext fc(fs, nullptr, InjectionPoint::PreDepth);
        return fc.buildDrawList(filter);
    };
    if (!fs.gpuDriven || !m_settings.shadows) return cpu();
    if ((filter.bucketMask & ~kGpuBuckets) != 0) return cpu();
    if (filter.requiredInstanceFlags != 0 && filter.requiredInstanceFlags != kInstanceCastShadows) return cpu();
    ViewState& vs = viewState(fs.view->id());
    if (!vs.early) return cpu();
    DrawSet& set = drawSet(filter.requiredInstanceFlags == kInstanceCastShadows, fs);

    GpuCullJob job;
    if (filter.frustum) {
        for (u32 p = 0; p < 6; ++p) job.planes[p] = glm::vec4(filter.frustum->planes[p].normal, filter.frustum->planes[p].d);
        job.flags |= kCullFrustum;
    }
    if (filter.sphere) {
        job.sphere = glm::vec4(filter.sphere->center, filter.sphere->radius);
        job.flags |= kCullSphere;
    }
    if (filter.lod) {
        job.lodCamera = glm::vec4(filter.lod->cameraPosition, filter.lod->projScale);
        job.lodThreshold = filter.lod->thresholdPixels;
        job.lodOrtho = filter.lod->orthographic ? 1u : 0u;
    }
    job.multiplier = std::max(multiplier, 1u);
    const bool compact = m_device->caps().drawIndirectCount && m_settings.drawCountMode != 1;
    if (compact) job.flags |= kCullCompact;
    fillJob(fs, vs, set, job, filter.requiredInstanceFlags ? kCullKindShadow : kCullKindMain);
    if (compact) job.compacted = arenaAllocate(vs, set.commandCount * kCommandSize, 16).address;
    GpuAllocation a = fs.r->frameAlloc.allocate(sizeof(GpuCullJob), 16);
    std::memcpy(a.cpu, &job, sizeof(job));
    vs.early->jobs.push_back({a, job.itemCount, job.batchCount, job.runCount, false});

    auto locate = [&](VkDeviceAddress addr) -> std::pair<rhi::BufferHandle, u64> {
        for (auto& c : vs.chunks) {
            if (addr >= c.address && addr < c.address + c.size) return {c.buffer, addr - c.address};
        }
        return {};
    };
    const auto [cmdBuf, cmdOff] = locate(compact ? job.compacted : job.commands);
    const auto [cntBuf, cntOff] = locate(job.runCounts);
    DrawList l;
    makeList(l, set, job, filter.bucketMask, cmdOff, cmdBuf, cntOff, job.multiplier);
    if (l.indirectCountBuffer) l.indirectCountBuffer = cntBuf;
    return l;
}

void GpuDriven::recordJobs(rhi::CommandList& cmd, PassJobs& pass, VkDeviceAddress view, VkDeviceAddress scene) {
    if (pass.jobs.empty()) return;
    // Order after previous readers of the arena / draw sets (earlier passes and frames on this queue).
    cmd.memoryBarrier(rhi::Access::General, rhi::Access::General);
    bool copied = false;
    for (auto& s : m_sets) {
        if (!s || !s->pending) continue;
        if (!s->items.empty())
            cmd.copyBuffer(s->pendingItems.buffer, s->itemBuffer, s->items.size() * sizeof(glm::uvec2), s->pendingItems.offset, 0);
        if (!s->batches.empty())
            cmd.copyBuffer(s->pendingBatches.buffer, s->batchBuffer, s->batches.size() * sizeof(GpuCullBatch),
                           s->pendingBatches.offset, 0);
        s->pending = false;
        copied = true;
    }
    if (copied) cmd.memoryBarrier(rhi::Access::TransferWrite, rhi::Access::StorageWriteCompute);
    cmd.bindPipeline(m_cull);
    CullPush pc;
    pc.view = view;
    pc.scene = scene;
    pc.lods = m_scene->meshLodAddress();
    // One dispatch per stage for all jobs (gl_WorkGroupID.y = job): dispatch count, not job count, dominates the cost
    // of many small culling views (cascades, local lights) on tile GPUs.
    const u32 jobCount = u32(pass.jobs.size());
    GpuAllocation table = m_frameAlloc->allocate(u64(jobCount) * 8, 16);
    u32 maxThreads[4] = {1, 0, 0, 0};
    for (u32 i = 0; i < jobCount; ++i) {
        const PassJobs::Job& j = pass.jobs[i];
        static_cast<u64*>(table.cpu)[i] = j.params.address;
        maxThreads[0] = std::max({maxThreads[0], j.batchCount, j.runCount, j.resetVisibility ? j.itemCount : 0u});
        maxThreads[1] = std::max(maxThreads[1], j.itemCount);
        maxThreads[2] = std::max(maxThreads[2], j.batchCount);
        maxThreads[3] = std::max(maxThreads[3], j.itemCount);
    }
    pc.job = table.address;
    for (u32 stage = 0; stage < 4; ++stage) {
        if (maxThreads[stage] > 0) {
            pc.stage = stage;
            cmd.pushConstants(pc);
            cmd.dispatch((maxThreads[stage] + kGroup - 1) / kGroup, jobCount);
        }
        cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::StorageWriteCompute);
    }
    bool anyMeshlets = false;
    for (const PassJobs::Job& j : pass.jobs) anyMeshlets = anyMeshlets || j.meshletParams.address != 0;
    if (anyMeshlets) {
        cmd.bindPipeline(m_meshletCull);
        for (u32 stage = 0; stage < 3; ++stage) {
            for (const PassJobs::Job& j : pass.jobs) {
                if (!j.meshletParams.address || (j.meshShaderPath && stage > 0)) continue;
                pc.job = j.meshletParams.address;
                pc.stage = stage;
                cmd.pushConstants(pc);
                if (stage == 1) cmd.dispatchIndirect(j.meshletDispatch.buffer, j.meshletDispatch.offset);
                else cmd.dispatch(1);
            }
            // Stage 0 writes the dispatch arguments: make them visible to the indirect dispatch.
            cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::General);
        }
    }
    // Indirect arguments + instance ids are consumed by the following draws.
    cmd.memoryBarrier(rhi::Access::StorageWriteCompute, rhi::Access::General);
}

bool GpuDriven::ensureMeshletPipelines() {
    rhi::Device& dev = *m_device;
    auto ready = [&] {
        bool all = dev.isPipelineReady(m_meshletCull);
        for (u32 v = 0; v < kVariantCount; ++v) {
            all = all && dev.isPipelineReady(m_meshletForward[v]) && dev.isPipelineReady(m_meshletPrepass[v][0]) &&
                  dev.isPipelineReady(m_meshletPrepass[v][1]);
        }
        return all;
    };
    if (m_meshletCull) return ready();
    // 13 pipelines: compiled on the job system when available; the instanced path is the placeholder until then.
    auto graphics = [&](const rhi::GraphicsPipelineDesc& d) {
        return m_jobs ? dev.createGraphicsPipelineAsync(d, *m_jobs) : dev.createGraphicsPipeline(d);
    };
    rhi::ComputePipelineDesc cd;
    cd.name = "gpuDriven.meshletCull";
    cd.shader = rhi::ShaderStageDesc::file("render/gpu_driven/meshlet_cull.comp");
    m_meshletCull = m_jobs ? dev.createComputePipelineAsync(cd, *m_jobs) : dev.createComputePipeline(cd);
    for (u32 variant = 0; variant < kVariantCount; ++variant) {
        for (u32 entity = 0; entity < 2; ++entity) {
            rhi::GraphicsPipelineDesc d;
            d.name = std::format("gpuDriven.meshletPrepass.v{}{}", variant, entity ? ".id" : "");
            d.vertex = rhi::ShaderStageDesc::file("render/gpu_driven/meshlet.vert");
            std::vector<rhi::ShaderDefine> defs;
            if (variant & kVariantAlphaTest) defs.push_back({"OX_ALPHA_TEST"});
            if (entity) defs.push_back({"OX_ENTITY_ID"});
            d.fragment = rhi::ShaderStageDesc::file("render/passes/depth_prepass.frag", defs);
            d.colorFormats = {formats::kNormals, formats::kVelocity};
            if (entity) {
                d.colorFormats.push_back(formats::kEntityId);
                rhi::BlendState noBlend;
                noBlend.srcColor = noBlend.srcAlpha = VK_BLEND_FACTOR_ONE;
                noBlend.dstColor = noBlend.dstAlpha = VK_BLEND_FACTOR_ZERO;
                noBlend.writeMask = VK_COLOR_COMPONENT_R_BIT;
                d.blend = {rhi::BlendState::opaque(), rhi::BlendState::opaque(), noBlend};
            }
            d.depthFormat = formats::kDepth;
            d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
            d.raster.cullMode = (variant & kVariantDoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            d.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            m_meshletPrepass[variant][entity] = graphics(d);
        }
        rhi::GraphicsPipelineDesc f;
        f.name = std::format("gpuDriven.meshletForward.v{}", variant);
        f.vertex = rhi::ShaderStageDesc::file("render/gpu_driven/meshlet.vert");
        f.fragment = rhi::ShaderStageDesc::file("render/passes/forward.frag");
        f.colorFormats = {formats::kSceneColor};
        f.depthFormat = formats::kDepth;
        f.depth = {true, false, VK_COMPARE_OP_EQUAL};
        f.raster.cullMode = (variant & kVariantDoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        f.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        m_meshletForward[variant] = graphics(f);
    }
    return ready();
}

bool GpuDriven::ensureMeshShaderPipelines() {
    rhi::Device& dev = *m_device;
    if (!m_meshPipelinesTried) {
        m_meshPipelinesTried = true;
        for (u32 variant = 0; variant < kVariantCount; ++variant) {
            for (u32 entity = 0; entity < 3; ++entity) { // 0, 1: prepass (+ entity id), 2: forward
                rhi::GraphicsPipelineDesc d;
                d.name = std::format("gpuDriven.mesh{}.v{}", entity == 2 ? "Forward" : entity ? "Prepass.id" : "Prepass", variant);
                d.task = rhi::ShaderStageDesc::file("render/gpu_driven/meshlet.task");
                d.mesh = rhi::ShaderStageDesc::file("render/gpu_driven/meshlet.mesh");
                std::vector<rhi::ShaderDefine> defs;
                if (entity < 2 && (variant & kVariantAlphaTest)) defs.push_back({"OX_ALPHA_TEST"});
                if (entity == 1) defs.push_back({"OX_ENTITY_ID"});
                if (entity == 2) {
                    d.fragment = rhi::ShaderStageDesc::file("render/passes/forward.frag");
                    d.colorFormats = {formats::kSceneColor};
                    d.depth = {true, false, VK_COMPARE_OP_EQUAL};
                } else {
                    d.fragment = rhi::ShaderStageDesc::file("render/passes/depth_prepass.frag", defs);
                    d.colorFormats = {formats::kNormals, formats::kVelocity};
                    if (entity == 1) {
                        d.colorFormats.push_back(formats::kEntityId);
                        rhi::BlendState noBlend;
                        noBlend.srcColor = noBlend.srcAlpha = VK_BLEND_FACTOR_ONE;
                        noBlend.dstColor = noBlend.dstAlpha = VK_BLEND_FACTOR_ZERO;
                        noBlend.writeMask = VK_COLOR_COMPONENT_R_BIT;
                        d.blend = {rhi::BlendState::opaque(), rhi::BlendState::opaque(), noBlend};
                    }
                    d.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
                }
                d.depthFormat = formats::kDepth;
                d.raster.cullMode = (variant & kVariantDoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
                d.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                const rhi::PipelineHandle p = m_jobs ? dev.createGraphicsPipelineAsync(d, *m_jobs) : dev.createGraphicsPipeline(d);
                if (entity == 2) m_meshForward[variant] = p;
                else m_meshPrepass[variant][entity] = p;
            }
        }
    }
    bool all = true;
    for (u32 v = 0; v < kVariantCount; ++v) {
        all = all && dev.isPipelineReady(m_meshForward[v]) && dev.isPipelineReady(m_meshPrepass[v][0]) &&
              dev.isPipelineReady(m_meshPrepass[v][1]);
    }
    return all;
}

void GpuDriven::drawMeshlets(FrameState& fs, rhi::CommandList& cmd, bool late, bool forward, bool entity, const void* push,
                             u32 pushSize) {
    if (!fs.gpuDriven) return;
    auto it = m_views.find(fs.view->id());
    if (it == m_views.end()) return;
    const ViewState::MeshletDraw& md = it->second->meshlets[late ? 1 : 0];
    if (!md.valid) return;
    if (md.meshShaders) {
        struct MeshPush {
            u8 draw[48]; // DrawPush (view, scene, drawIds, inputs[6])
            u64 lods;
            u32 hiz, hizMips;
            glm::vec2 hizSize;
            u32 late, variant;
        } mp{};
        static_assert(sizeof(MeshPush) == 80);
        std::memcpy(mp.draw, push, std::min<u32>(pushSize, 48));
        std::memcpy(mp.draw + 16, &md.instances, sizeof(u64));
        mp.lods = m_scene->meshLodAddress();
        mp.hiz = md.hiz;
        mp.hizMips = md.hizMips;
        mp.hizSize = md.hizSize;
        mp.late = late ? 1u : 0u;
        for (u32 v = 0; v < kVariantCount; ++v) {
            const rhi::PipelineHandle p = forward ? m_meshForward[v] : m_meshPrepass[v][entity ? 1 : 0];
            if (!p) continue;
            mp.variant = v;
            cmd.bindPipeline(p);
            cmd.pushConstants(mp);
            cmd.drawMeshTasksIndirect(md.taskArgs.buffer, md.taskArgs.offset);
            fs.stats->drawCalls += 1;
            fs.stats->meshShaderDrawCalls += 1;
            fs.stats->indirectDrawCalls += 1;
            fs.stats->indirectCommands += 1;
        }
        return;
    }
    u8 pcData[rhi::kMaxPushConstantSize];
    std::memcpy(pcData, push, std::min<u32>(pushSize, sizeof(pcData)));
    std::memcpy(pcData + 16, &md.visible, sizeof(u64)); // drawIds slot = visible meshlet table
    cmd.bindIndexBuffer(md.indices.buffer, md.indices.offset);
    constexpr u32 stride = sizeof(VkDrawIndexedIndirectCommand);
    for (u32 v = 0; v < kVariantCount; ++v) {
        const rhi::PipelineHandle p = forward ? m_meshletForward[v] : m_meshletPrepass[v][entity ? 1 : 0];
        if (!p) continue;
        cmd.bindPipeline(p);
        cmd.pushConstants(pcData, pushSize);
        cmd.drawIndexedIndirect(md.commands.buffer, md.commands.offset + u64(v) * stride, 1, stride);
        fs.stats->drawCalls += 1;
        fs.stats->indirectDrawCalls += 1;
        fs.stats->indirectCommands += 1;
    }
}

u64 GpuDriven::memoryBytes() const {
    u64 bytes = 0;
    for (const auto& s : m_sets) {
        if (s) bytes += s->itemCapacity + s->batchCapacity;
    }
    for (const auto& [id, v] : m_views) {
        for (const auto& c : v->chunks) bytes += c.size;
        bytes += v->visibilityCapacity;
    }
    return bytes;
}

void registerGpuDrivenFeatures(FeatureRegistry& registry) { (void)registry; }

} // namespace ox::render
