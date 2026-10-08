#include <oxwald/render/render_stats.hpp>

#include <format>

namespace ox::render {

void RenderStats::resetCounters() {
    passes.clear();
    gpuFrameMs = 0.0;
    cpuRenderMs = 0.0;
    views = 0;
    drawCalls = 0;
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
}

std::string RenderStats::toString() const {
    std::string s = std::format(
        "frame {}: GPU {:.2f} ms, CPU {:.2f} ms, {} views, {} draws, {} tris, {}/{} instances, {} lights ({} shadowed, "
        "{} maps rendered / {} cached), {} passes ({} graph compiles), VRAM {:.1f}/{:.1f} MiB\n",
        frame, gpuFrameMs, cpuRenderMs, views, drawCalls, triangles, visibleInstances, instances, lights, shadowedLights,
        shadowMapsRendered, shadowMapsCached, renderGraphPasses, renderGraphCompiles, f64(vramUsageBytes) / (1 << 20),
        f64(vramBudgetBytes) / (1 << 20));
    for (const PassTiming& p : passes) s += std::format("  {:<28} {:7.3f} ms\n", p.name, p.gpuMs);
    return s;
}

} // namespace ox::render
