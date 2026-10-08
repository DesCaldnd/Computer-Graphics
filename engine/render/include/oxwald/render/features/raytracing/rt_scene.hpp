#pragma once

// CPU side of the ray tracing scene: TLAS instance table (masks, SBT offsets, dirty tracking, update-vs-rebuild),
// BLAS build/compaction/refit scheduling (backend interface so the logic is unit-testable without RT hardware), the
// LOD policy and the deformed-geometry hook for skinned meshes. The GPU implementation lives in
// src/features/raytracing/rt_scene_gpu.cpp.
//
// Shader-side mirror: engine/shaders/render/raytracing/rt_common.glsl (RtInstance, RtSceneHeader).

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>
#include <oxwald/rhi/handles.hpp>

#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace ox::render::rt {

// --- instance masks (8 bits, ray mask & instance mask != 0 → candidate) -----------------------------------------
enum RtInstanceMask : u32 {
    kMaskOpaque = 1u << 0,            // opaque material
    kMaskAlphaTested = 1u << 1,       // alpha-tested material (any-hit / candidate loop evaluates the cutoff)
    kMaskTranslucent = 1u << 2,       // transparent or refractive material
    kMaskShadowOpaque = 1u << 3,      // casts shadows, opaque or alpha tested
    kMaskShadowTranslucent = 1u << 4, // casts coloured transmission shadows
};
inline constexpr u32 kMaskSolid = kMaskOpaque | kMaskAlphaTested;
inline constexpr u32 kMaskAllGeometry = kMaskOpaque | kMaskAlphaTested | kMaskTranslucent;
inline constexpr u32 kMaskAllShadows = kMaskShadowOpaque | kMaskShadowTranslucent;

// --- shader binding table layout ------------------------------------------------------------------------------
// Hit groups are ordered [hitGroup][rayType]; an instance's SBT record offset selects its hit group and the trace
// call adds the ray type (sbtRecordOffset = rayType, sbtRecordStride = kRayTypeCount).
enum class RtHitGroup : u32 { Opaque = 0, AlphaTested = 1, Translucent = 2, Count };
enum class RtRayType : u32 { Radiance = 0, Shadow = 1, Count };
inline constexpr u32 kRayTypeCount = u32(RtRayType::Count);
inline constexpr u32 kHitGroupCount = u32(RtHitGroup::Count);
inline constexpr u32 kMaxCustomIndex = (1u << 24) - 1;

[[nodiscard]] constexpr u32 sbtRecordOffset(RtHitGroup g) { return u32(g) * kRayTypeCount; }
// assets::BlendMode values (Opaque, AlphaTest, Transparent, Refractive) → hit group.
[[nodiscard]] constexpr RtHitGroup hitGroupForBlend(u32 blend) {
    return blend == 0 ? RtHitGroup::Opaque : blend == 1 ? RtHitGroup::AlphaTested : RtHitGroup::Translucent;
}
[[nodiscard]] u32 instanceMask(u32 blend, bool castShadows);

// --- LOD policy -----------------------------------------------------------------------------------------------
// RT effects are tolerant to geometric simplification (shadows, AO, GI, glossy reflections), so BLASes use the
// coarsest LOD by default (requestedLod = -1). Mirror-like reflections may ask for a finer LOD.
[[nodiscard]] u32 selectBlasLod(u32 lodCount, i32 requestedLod);

// --- per-frame TLAS input --------------------------------------------------------------------------------------
struct TlasSource {
    u32 gpuInstance = 0;   // GpuInstance slot (instance table index)
    u32 meshInfo = 0;      // GpuMeshInfo index of the drawn LOD
    u32 material = 0;      // GpuMaterial index
    u32 blend = 0;         // assets::BlendMode
    bool doubleSided = false;
    bool castShadows = true;
    bool skinned = false;
    bool deformedBlas = false; // BLAS built/refitted from deformed (skinned) positions
    glm::mat4 world{1.0f};
    u64 blasKey = 0;       // BlasScheduler key (0 = no BLAS yet → instance skipped)
    u64 blas = 0;          // backend handle (e.g. rhi::AccelStructHandle bits), 0 = not ready
};

struct TlasEntry {
    u32 gpuInstance = 0;
    u32 customIndex = 0; // = entry index (RtInstance table index in shaders)
    u32 mask = 0;
    u32 sbtOffset = 0;
    u32 flags = 0;       // VkGeometryInstanceFlagsKHR
    u64 blas = 0;
    glm::mat3x4 transform{1.0f}; // row-major 3×4 (VkTransformMatrixKHR)
};

// Mirrors `RtInstance` in rt_common.glsl (32 bytes): instance → mesh → geometry/material lookup for hit shading.
struct RtInstanceGpu {
    u32 gpuInstance = 0;
    u32 meshInfo = 0;
    u32 material = 0;
    u32 firstIndex = 0;
    i32 vertexOffset = 0;
    u32 flags = 0; // bit 0 double sided, bit 1 skinned, bit 2 deformed BLAS (hit attributes from the skinning output), bits 8-15 blend
    u32 mask = 0;
    u32 pad = 0;
};
static_assert(sizeof(RtInstanceGpu) == 32);

// Dense TLAS instance list rebuilt from the scene every frame with change classification:
//   * transforms (and masks / SBT offsets) changed only → TLAS update (refit)
//   * an instance added/removed, a BLAS swapped (built, compacted, rebuilt) or instance flags changed → rebuild
//   * `maxUpdatesBeforeRebuild` consecutive updates → rebuild (refits degrade traversal quality)
class TlasInstanceTable {
public:
    struct Result {
        bool rebuild = false;   // full build needed (else update / nothing)
        bool changed = false;   // anything to upload/build at all
        u32 added = 0, removed = 0, moved = 0, retagged = 0, blasSwapped = 0;
        u32 skipped = 0;        // sources without a ready BLAS
    };

    // `sources` may be in any order; entries are sorted by gpuInstance so the order is stable across frames.
    Result update(std::span<const TlasSource> sources);
    [[nodiscard]] const std::vector<TlasEntry>& entries() const { return m_entries; }
    [[nodiscard]] const std::vector<RtInstanceGpu>& gpuInstances() const { return m_gpu; }
    [[nodiscard]] std::optional<u32> entryOf(u32 gpuInstance) const;
    [[nodiscard]] u32 updatesSinceRebuild() const { return m_updatesSinceRebuild; }
    u32 maxUpdatesBeforeRebuild = 120;
    void forceRebuild() { m_forceRebuild = true; }

private:
    std::vector<TlasEntry> m_entries;
    std::vector<RtInstanceGpu> m_gpu;
    std::unordered_map<u32, u32> m_index; // gpuInstance → entry
    u32 m_updatesSinceRebuild = 0;
    bool m_forceRebuild = true;
};

// Fills the instance-side fields of a TlasSource / RtInstanceGpu from a world matrix.
[[nodiscard]] glm::mat3x4 toTlasTransform(const glm::mat4& world);

// --- BLAS scheduling --------------------------------------------------------------------------------------------

struct BlasRequest {
    u64 key = 0;            // unique per (mesh geometry, LOD, generation); see blasKey()
    u32 meshInfo = 0;
    u32 firstIndex = 0, indexCount = 0;
    i32 vertexOffset = 0;
    u32 vertexCount = 0;
    bool deformable = false; // skinned / deformed: allowUpdate, never compacted, refitted when deformed
    f32 priority = 0.0f;     // higher first (e.g. screen coverage, instance count)
    u32 gpuInstance = ~0u;   // deformables: the instance whose deformed geometry this BLAS holds (one BLAS each)
};
[[nodiscard]] u64 blasKey(u32 meshInfo, u32 firstIndex, u32 indexCount, i32 vertexOffset, u32 vertexCount, bool deformable);

// What the scheduler drives. The rhi implementation builds synchronously (Device::createBlas also compacts), the
// test mock completes builds/queries over several frames like an asynchronous GPU queue would.
class IBlasBackend {
public:
    using Handle = u64; // 0 = invalid
    virtual ~IBlasBackend() = default;
    virtual Handle build(const BlasRequest& request, bool allowCompaction) = 0;
    // True once the build (or compaction copy) finished on the GPU.
    [[nodiscard]] virtual bool isComplete(Handle h) = 0;
    // Compacted size after a build with allowCompaction finished (query result), nullopt while unknown or when the
    // backend compacts internally (see compactsInternally()).
    [[nodiscard]] virtual std::optional<u64> compactedSize(Handle h) = 0;
    // Starts the compaction copy into a new, smaller AS; the source stays valid until destroyed.
    virtual Handle compact(Handle h, u64 compactedSize) = 0;
    // Records a refit from the request's (deformed) geometry.
    virtual void refit(Handle h, const BlasRequest& request) = 0;
    // Deferred destruction (GPU may still use it this frame).
    virtual void destroy(Handle h) = 0;
    [[nodiscard]] virtual u64 memorySize(Handle h) = 0;
    [[nodiscard]] virtual bool compactsInternally() const { return false; }
};

struct BlasSchedulerConfig {
    u32 maxBuildsPerFrame = 16;
    u64 maxTrianglesPerFrame = 2'000'000; // build budget (the first build of a frame always goes through)
    u32 maxCompactionsPerFrame = 8;
    u32 evictAfterFrames = 300;            // unused BLASes are destroyed after this many frames
    u32 maxRefitsBeforeRebuild = 90;       // deformables get a full rebuild after this many refits
};

class BlasScheduler {
public:
    enum class State : u8 { Queued, Building, Built, CompactionQueued, Compacting, Compacted };
    struct Stats {
        u32 resident = 0, queued = 0, building = 0, compacting = 0;
        u32 builtThisFrame = 0, compactedThisFrame = 0, refitsThisFrame = 0, rebuildsThisFrame = 0, evictedThisFrame = 0;
        u64 memoryBytes = 0, savedByCompaction = 0;
    };

    BlasScheduler(IBlasBackend& backend, BlasSchedulerConfig config = {}) : m_backend(&backend), m_config(config) {}
    ~BlasScheduler();
    BlasScheduler(const BlasScheduler&) = delete;
    BlasScheduler& operator=(const BlasScheduler&) = delete;

    void beginFrame(u64 frame);
    // Declares that the BLAS is needed this frame (queues it the first time). Re-requesting refreshes its priority.
    void request(const BlasRequest& request);
    // Deformed geometry changed this frame: refit (or rebuild after maxRefitsBeforeRebuild).
    void markDeformed(u64 key);
    // Runs the state machine: polls completions, starts compactions, starts new builds within the budget, refits,
    // evicts BLASes not requested for evictAfterFrames.
    void update();
    // Usable BLAS for TLAS instances (the uncompacted original is used until the compacted copy completes).
    [[nodiscard]] IBlasBackend::Handle ready(u64 key) const;
    [[nodiscard]] std::optional<State> state(u64 key) const;
    [[nodiscard]] const Stats& stats() const { return m_stats; }
    // Keys whose handle changed this frame (TLAS must rebuild).
    [[nodiscard]] bool handlesChanged() const { return m_handlesChanged; }
    void clear(); // destroys everything (r.RayTracing turned off, device loss)
    BlasSchedulerConfig& config() { return m_config; }

private:
    struct Item {
        BlasRequest request;
        State state = State::Queued;
        IBlasBackend::Handle handle = 0;    // usable / being built
        IBlasBackend::Handle compacted = 0; // compaction target
        u64 compactedBytes = 0;
        u64 lastRequested = 0;
        u32 refits = 0;
        bool deformedThisFrame = false;
    };
    IBlasBackend* m_backend;
    BlasSchedulerConfig m_config;
    std::unordered_map<u64, Item> m_items;
    u64 m_frame = 0;
    Stats m_stats;
    bool m_handlesChanged = false;
};

// --- deformed geometry hook (world-skinning team) --------------------------------------------------------------
// Skinned/deformed instances are traced against their bind pose unless a provider returns post-deformation
// object-space positions for them. The buffer must contain `vertexCount` positions (float3, `stride` bytes apart,
// usage AccelStructInput | Storage), written before InjectionPoint::AfterDepth of the frame; the TLAS pass refits the
// instance's own BLAS from it when `version` changed. Register through RayTracingSceneApi (rt_api.hpp).
struct DeformedGeometry {
    rhi::BufferHandle positions;
    u64 offset = 0;      // bytes
    u32 vertexCount = 0;
    u32 stride = 12;
    u64 version = 0;     // bump when the contents changed (e.g. the frame number)
};
using DeformedGeometryProvider = std::function<std::optional<DeformedGeometry>(u32 gpuInstance)>;

// --- path tracer accumulation -----------------------------------------------------------------------------------
// Progressive accumulation restarts when anything visible changes. The key is hashed from the camera matrices, the
// scene (moved instances, instance/material/light changes) and the settings.
class AccumulationTracker {
public:
    // Returns true when the accumulation must restart this frame; counts frames otherwise.
    bool update(u64 cameraHash, u64 sceneHash, u64 settingsHash);
    [[nodiscard]] u32 sampleCount() const { return m_samples; }
    void addSamples(u32 n) { m_samples += n; }
    void reset() { m_valid = false; }

private:
    u64 m_camera = 0, m_scene = 0, m_settings = 0;
    u32 m_samples = 0;
    bool m_valid = false;
};
[[nodiscard]] u64 hashMatrix(const glm::mat4& m);

} // namespace ox::render::rt
