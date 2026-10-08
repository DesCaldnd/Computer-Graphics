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
};

struct RenderStats {
    u64 frame = 0;
    // GPU time per render graph pass of the last retired frame (graph timestamps), and the sum.
    std::vector<PassTiming> passes;
    f64 gpuFrameMs = 0.0;
    f64 cpuRenderMs = 0.0; // renderer CPU time (setup + recording) of the last frame

    u32 views = 0;
    u32 drawCalls = 0;
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

    void resetCounters();
    [[nodiscard]] std::string toString() const;
};

} // namespace ox::render
