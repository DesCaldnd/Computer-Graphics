#include <oxwald/render/render_stats.hpp>

#include <format>

namespace ox::render {

void RenderStats::resetCounters() {
    passes.clear();
    gpuFrameMs = 0.0;
    cpuRenderMs = 0.0;
    gpuFrameWallMs = 0.0;
    asyncComputeMs = 0.0;
    asyncOverlapMs = 0.0;
    views = 0;
    drawCalls = 0;
    indirectDrawCalls = 0;
    indirectCommands = 0;
    indirectCountDrawCalls = 0;
    meshShaderDrawCalls = 0;
    parallelRecordedChunks = 0;
    triangles = 0;
    instances = 0;
    visibleInstances = 0;
    lights = 0;
    shadowedLights = 0;
    shadowMapsRendered = 0;
    shadowMapsCached = 0;
    renderGraphPasses = 0;
    renderGraphCompiles = 0;
    featuresEnabled = 0;
    vram.clear();
    gpuDriven = false;
    gpuCulling = {};
    streaming.mipsLoaded = 0;
    streaming.mipsEvicted = 0;
}

std::string RenderStats::toString() const {
    std::string s = std::format(
        "frame {}: GPU {:.2f} ms, CPU {:.2f} ms, {} views, {} draws, {} tris, {}/{} instances, {} lights ({} shadowed, "
        "{} maps rendered / {} cached), {} passes ({} graph compiles), VRAM {:.1f}/{:.1f} MiB\n",
        frame, gpuFrameMs, cpuRenderMs, views, drawCalls, triangles, visibleInstances, instances, lights, shadowedLights,
        shadowMapsRendered, shadowMapsCached, renderGraphPasses, renderGraphCompiles, f64(vramUsageBytes) / (1 << 20),
        f64(vramBudgetBytes) / (1 << 20));
    if (gpuCulling.valid) {
        const GpuCullingStats& g = gpuCulling;
        s += std::format("  gpu culling ({} frames late): {} tested, {} frustum, {} occluded, {} visible, {} draws, "
                         "{} tris, avg LOD {:.2f}; meshlets {}/{}; shadows {}/{} instances, {} draws, {} tris\n",
                         g.latencyFrames, g.instancesTested, g.instancesFrustumCulled, g.instancesOccluded,
                         g.instancesVisible, g.drawCommands, g.triangles, g.averageLod, g.meshletsVisible,
                         g.meshletsTested, g.shadowInstancesVisible, g.shadowInstancesTested, g.shadowDrawCommands,
                         g.shadowTriangles);
    }
    if (asyncComputeMs > 0.0) {
        s += std::format("  async compute {:.3f} ms ({:.3f} ms overlapped), GPU wall {:.3f} ms\n", asyncComputeMs,
                         asyncOverlapMs, gpuFrameWallMs);
    }
    if (indirectDrawCalls) {
        s += std::format("  indirect: {} calls ({} with GPU draw count), {} commands\n", indirectDrawCalls,
                         indirectCountDrawCalls, indirectCommands);
    }
    if (streaming.enabled) {
        s += std::format("  streaming: {} textures, {:.1f}/{:.1f} MiB (wanted {:.1f}), {} pending, +{} -{} mips\n",
                         streaming.streamedTextures, f64(streaming.residentBytes) / (1 << 20),
                         f64(streaming.budgetBytes) / (1 << 20), f64(streaming.wantedBytes) / (1 << 20),
                         streaming.pendingRequests, streaming.mipsLoaded, streaming.mipsEvicted);
    }
    for (const VramCategory& v : vram) s += std::format("  vram {:<22} {:8.2f} MiB\n", v.name, f64(v.bytes) / (1 << 20));
    for (const PassTiming& p : passes) s += std::format("  {:<28} {:7.3f} ms\n", p.name, p.gpuMs);
    return s;
}

} // namespace ox::render
