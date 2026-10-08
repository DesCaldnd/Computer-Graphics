#pragma once

// GPU-driven rendering & performance (area gpu-driven). The culling itself is part of the core draw path
// (FeatureContext::cullDrawList, r.GpuDriven*); this header exposes the helpers other features use.

#include <oxwald/rhi/device_caps.hpp>
#include <oxwald/rhi/types.hpp>

namespace ox::render {

class FeatureRegistry;

// Queue hint for async-compute-safe passes (r.AsyncCompute: 0 off, 1 on, 2 auto = on unless the device is a
// portability implementation; on MoltenVK the queues are serialised anyway and the cross-queue semaphores only add
// latency, see docs/dev/perf.md). Returns QueueType::Compute when enabled, else Graphics.
// A pass is async-safe when every resource it touches is declared to the render graph (the graph then inserts the
// cross-queue semaphores and ownership transfers) and it does not write memory the graph does not know about.
// Built-in async passes: LightCulling, HiZ. Not async: GpuCull / GpuCull.Late (untracked indirect buffers, on the
// critical path of the depth prepass), HiZ.Early (same).
[[nodiscard]] rhi::QueueType asyncComputeHint(const rhi::DeviceCaps& caps);

// Registers the area's features (texture streaming feedback, pipeline warm-up). Called from registerBuiltinFeatures.
void registerGpuDrivenFeatures(FeatureRegistry& registry);

} // namespace ox::render
