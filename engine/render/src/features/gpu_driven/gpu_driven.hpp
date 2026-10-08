#pragma once

// GPU-driven rendering (render core draw path, r.GpuDriven): persistent draw sets (instance → batch → per-LOD indirect
// commands, rebuilt only when GpuScene::structureVersion() changes), compute culling jobs per culling view (camera
// two-phase HiZ occlusion, shadow cascades, spot/point lights, custom FeatureContext::cullDrawList filters), GPU LOD
// selection, compacted instance-id lists and fixed-max-count indirect draws (drawIndirectCount when supported).
// Optional meshlet path (r.GpuDriven.Meshlets): instances whose LOD has meshlets are expanded by a cluster culling pass
// (frustum / backface cone / HiZ) that writes a compacted index buffer drawn with one indexed indirect draw per variant.
//
// Counters are read back framesInFlight frames later into RenderStats.

#include "../../renderer_impl.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace ox::render {

// C++ mirror of CullJob (gpu_driven/cull_common.glsl, scalar layout).
struct GpuCullJob {
    glm::vec4 planes[6]{};
    glm::vec4 sphere{0.0f};
    glm::vec4 lodCamera{0.0f};
    f32 lodThreshold = 1.0f;
    f32 drawDistance = 0.0f;
    u32 flags = 0;
    u32 multiplier = 1;
    glm::mat4 viewProj{1.0f};
    u64 items = 0, batches = 0, commands = 0, ids = 0, scratch = 0, visibility = 0, counters = 0, compacted = 0,
        runCounts = 0, meshletInstances = 0;
    u32 itemCount = 0;
    u32 batchCount = 0;
    u32 hiz = kInvalidIndex;
    u32 hizMips = 0;
    glm::vec2 hizSize{0.0f};
    u32 lodOrtho = 0;
    u32 meshletCapacity = 0;
    u32 runCount = 0;
    u32 pad = 0;
};
static_assert(sizeof(GpuCullJob) == 328);

// Mirror of MeshletJob (gpu_driven/meshlet_cull.comp).
struct GpuMeshletJob {
    glm::vec4 planes[6]{};
    glm::mat4 viewProj{1.0f};
    glm::vec4 camera{0.0f};
    u64 instances = 0, dispatch = 0, commands = 0, indices = 0, visible = 0, counters = 0;
    glm::uvec4 variantBase{0};
    glm::uvec4 variantCapacity{0};
    u32 visibleCapacity = 0;
    u32 instanceCapacity = 0;
    u32 flags = 0;
    u32 hiz = kInvalidIndex;
    u32 hizMips = 0;
    u32 pad = 0;
    glm::vec2 hizSize{0.0f};
};
static_assert(sizeof(GpuMeshletJob) == 288);

// Mirror of CullBatch.
struct GpuCullBatch {
    u32 firstCommand = 0;
    u32 idBase = 0;
    u32 lodCount = 1;
    u32 meshIndex = 0;
    u32 capacity = 0;
    u32 run = 0;
    u32 runFirst = 0;
    u32 pad = 0;
};
static_assert(sizeof(GpuCullBatch) == 32);

enum GpuCullFlags : u32 {
    kCullFrustum = 1u,
    kCullSphere = 2u,
    kCullEarly = 4u,
    kCullLate = 8u,
    kCullCompact = 16u,
    kCullDistance = 32u,
    kCullMeshlets = 64u,
    kCullResetVisibility = 128u,
};

// Counter slots (u32) per kind; see cull_common.glsl.
enum GpuCullCounter : u32 {
    kCounterTested = 0,
    kCounterFrustumCulled,
    kCounterOccluded,
    kCounterVisible,
    kCounterTriangles,
    kCounterDraws,
    kCounterMeshletsTested,
    kCounterMeshletsVisible,
    kCounterMeshletTriangles,
    kCounterLodSum,
    kCounterSlotsPerKind = 16,
};
inline constexpr u32 kCullKindMain = 0, kCullKindShadow = 1, kCullKinds = 2;

// Frame-consistent snapshot of the r.GpuDriven.* cvars.
struct GpuDrivenSettings {
    bool enabled = true;
    bool occlusion = true;
    bool shadows = true;
    bool meshlets = false;
    i32 drawCountMode = 0; // 0 auto, 1 max count, 2 drawIndirectCount
    f32 lodErrorPixels = 1.0f;
    bool meshShaders = false; // r.GpuDriven.MeshShaders && DeviceCaps::meshShader/taskShader
    bool parallelRecording = true;
    i32 parallelMinBatches = 256;
    static GpuDrivenSettings fromCVars(const rhi::DeviceCaps& caps);
};

class GpuDriven {
public:
    // `jobs` (optional): meshlet pipelines compile in the background (instanced path as placeholder until ready).
    GpuDriven(rhi::Device& device, GpuScene& scene, GpuFrameAllocator& frameAlloc, JobSystem* jobs = nullptr);
    ~GpuDriven();
    GpuDriven(const GpuDriven&) = delete;
    GpuDriven& operator=(const GpuDriven&) = delete;

    // Renderer::beginFrame: reads the counters of the retired frame using this frame slot into `stats`.
    void beginFrame(RenderStats& stats);
    [[nodiscard]] const GpuDrivenSettings& settings() const { return m_settings; }
    // GPU-driven path usable this frame (cvar, pipelines, device features).
    [[nodiscard]] bool active() const { return m_active; }

    // Main view (after SceneUpload is declared): declares the "GpuCull" pass and fills fs.gpuEarly (+ fs.gpuLate
    // when two-phase occlusion is on; recorded by declareLate()).
    void setupView(FrameState& fs);
    // After the early depth prepass: HiZ of the early depth + late (phase 2) culling. Returns false when there is no
    // late phase this frame.
    bool declareLate(FrameState& fs, rhi::RGTexture depth);
    // Custom / shadow culling job. Returns a CPU list (buildDrawList) when the filter is not GPU-cullable.
    DrawList cull(FrameState& fs, const DrawFilter& filter, u32 multiplier);
    void releaseView(ViewId view);
    // GPU memory owned by GPU-driven rendering (draw sets, per-view arenas, visibility), bytes.
    [[nodiscard]] u64 memoryBytes() const;

    // Meshlet draws (r.GpuDriven.Meshlets) of one occlusion phase into an open rendering scope: one indexed indirect
    // draw per pipeline variant over the compacted index buffer. `push` is a DrawPush block (drawIds is replaced).
    void drawMeshlets(FrameState& fs, rhi::CommandList& cmd, bool late, bool forward, bool entity, const void* push,
                      u32 pushSize);

    struct DrawSet;
    struct ViewState;
    struct PassJobs;

private:
    void ensurePipelines();
    bool ensureMeshletPipelines();
    bool ensureMeshShaderPipelines();
    DrawSet& drawSet(bool shadow, FrameState& fs);
    ViewState& viewState(ViewId id);
    GpuAllocation arenaAllocate(ViewState& vs, u64 size, u64 alignment);
    void fillJob(FrameState& fs, ViewState& vs, DrawSet& set, GpuCullJob& job, u32 kind);
    void makeList(DrawList& list, const DrawSet& set, const GpuCullJob& job, u32 bucketMask, u64 commandsOffset,
                  rhi::BufferHandle buffer, u64 countOffset, u32 multiplier) const;
    void recordJobs(rhi::CommandList& cmd, PassJobs& jobs, VkDeviceAddress view, VkDeviceAddress scene);

    rhi::Device* m_device;
    GpuScene* m_scene;
    GpuFrameAllocator* m_frameAlloc;
    JobSystem* m_jobs = nullptr;
    GpuDrivenSettings m_settings;
    bool m_active = false;
    bool m_pipelinesTried = false;
    rhi::PipelineHandle m_cull, m_hiz, m_meshletCull, m_meshletPrepare;
    rhi::PipelineHandle m_meshletPrepass[kVariantCount][2];
    rhi::PipelineHandle m_meshletForward[kVariantCount];
    rhi::PipelineHandle m_meshPrepass[kVariantCount][2]; // VK_EXT_mesh_shader path
    rhi::PipelineHandle m_meshForward[kVariantCount];
    bool m_meshPipelinesTried = false;
    std::unique_ptr<DrawSet> m_sets[2];
    std::unordered_map<ViewId, std::unique_ptr<ViewState>> m_views;
    std::vector<rhi::BufferHandle> m_readback; // per frame slot: counters (u32 × kCounterSlotsPerKind × kCullKinds)
    std::vector<u64> m_readbackFrame;          // device frame number the slot was last written in (0 = never)
};

} // namespace ox::render
