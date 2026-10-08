#pragma once

// RenderStats: numbers for the editor / ImGui statistics overlay. Renderer::stats() returns the last frame.

#include <oxwald/core/types.hpp>
#include <oxwald/rhi/profiling.hpp>

#include <string>
#include <vector>

namespace ox::render {

struct PassTiming {
    std::string name; // "<view>/<pass>"
    f64 gpuMs = 0.0;
    f64 startMs = 0.0; // first occurrence, relative to the frame's first GPU timestamp
    bool asyncCompute = false;
};

// GPU-driven culling counters (r.GpuDriven), read back `latencyFrames` frames late (all views of that frame).
struct GpuCullingStats {
    bool valid = false;
    u32 latencyFrames = 0;
    // Camera views (two-phase occlusion: visible = drawn in phase 1 + newly visible in phase 2).
    u32 instancesTested = 0;
    u32 instancesFrustumCulled = 0; // frustum + draw distance
    u32 instancesOccluded = 0;      // HiZ
    u32 instancesVisible = 0;
    u32 drawCommands = 0;           // non-empty indirect commands
    u64 triangles = 0;              // instanced path
    f32 averageLod = 0.0f;
    u32 meshletsTested = 0;         // r.GpuDriven.Meshlets
    u32 meshletsVisible = 0;
    u64 meshletTriangles = 0;
    // Shadow cascades / lights / custom cullDrawList jobs.
    u32 shadowInstancesTested = 0;
    u32 shadowInstancesVisible = 0;
    u32 shadowDrawCommands = 0;
    u64 shadowTriangles = 0;
};

// Texture mip streaming (r.Streaming.*).
struct TextureStreamingStats {
    bool enabled = false;
    u32 streamedTextures = 0;   // textures managed by the streamer
    u32 pendingRequests = 0;    // mip loads in flight
    u32 mipsLoaded = 0;         // this frame
    u32 mipsEvicted = 0;        // this frame
    u64 residentBytes = 0;      // streamed textures, current residency
    u64 wantedBytes = 0;        // residency the feedback asks for (before the budget)
    u64 budgetBytes = 0;
};

struct VramCategory {
    std::string name;
    u64 bytes = 0;
};

struct RenderStats {
    u64 frame = 0;
    // GPU time per render graph pass of the last retired frame (graph timestamps), and the sum.
    std::vector<PassTiming> passes;
    f64 gpuFrameMs = 0.0;
    f64 cpuRenderMs = 0.0; // renderer CPU time (setup + recording) of the last frame
    f64 gpuFrameWallMs = 0.0;   // first timestamp to last (overlapping async compute counted once)
    f64 asyncComputeMs = 0.0;   // passes on the async compute queue
    f64 asyncOverlapMs = 0.0;   // of which overlapped with graphics-queue passes

    u32 views = 0;
    u32 drawCalls = 0;          // API draw calls (an indirect multi-draw counts once)
    u32 indirectDrawCalls = 0;  // of which indirect (GPU-driven)
    u32 indirectCommands = 0;   // indirect commands submitted (fixed max count incl. empty padding)
    u64 triangles = 0;
    u32 instances = 0;         // GPU scene instances (entity × submesh)
    u32 visibleInstances = 0;  // after camera culling, all views
    u32 lights = 0;            // uploaded lights (all views)
    u32 shadowedLights = 0;
    u32 shadowMapsRendered = 0; // tiles / cascades / cube faces re-rendered (cache misses)
    u32 shadowMapsCached = 0;
    u32 renderGraphPasses = 0;
    u32 renderGraphCompiles = 0;  // non-cached plan compiles (graph rebuilds) this frame
    u32 featuresEnabled = 0;

    u64 vramUsageBytes = 0;
    u64 vramBudgetBytes = 0;
    u32 textures = 0;
    u32 buffers = 0;
    u64 geometryBytes = 0;
    u32 pendingAssetLoads = 0;
    std::vector<VramCategory> vram; // renderer-owned GPU memory by category

    bool gpuDriven = false; // r.GpuDriven path active for the main draw lists
    GpuCullingStats gpuCulling;
    TextureStreamingStats streaming;
    u32 pipelinesCompiling = 0; // async PSO compiles in flight
    u32 parallelRecordedChunks = 0; // secondary command lists recorded on the job system (r.ParallelRecording)

    void resetCounters();
    [[nodiscard]] std::string toString() const;
};

} // namespace ox::render
