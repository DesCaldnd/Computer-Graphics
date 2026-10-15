#pragma once

// Split rendering: run light, portable render graph passes on a secondary GPU (typically the CPU's integrated GPU)
// while the rest of the frame stays on the main (discrete) GPU. Guide: docs/guide/35-split-rendering.md.
//
// Features opt in per pass by declaring a *portable segment* (one or more compute passes whose push constants hold
// only bindless texture indices and plain values) with addPortableSegment() instead of graph().addPass(). When split
// rendering is off, unsupported, or not profitable, the segment is declared as ordinary compute passes — exactly what
// the feature would have declared by hand. When the scheduler places a segment on the secondary device:
//   frame N:     "Split.Export.<name>" copies the segment's inputs into memory both devices can reach;
//   worker:      waits for frame N to retire on the primary, runs the passes on the secondary (same SPIR-V);
//   frame N + L: "Split.Import.<name>" copies the results of frame N into the segment's outputs (L = latency).
// The first L frames after enabling run on the primary. If the secondary fails (device lost, timeout) the import
// copies the input through for that frame and split rendering switches itself off.
//
// CVars: r.SplitRendering (0 Off, 1 On = offload every portable segment, 2 Auto = only when the cost model says it
// saves primary GPU time), r.SplitRendering.Adapter, r.SplitRendering.AllowSameAdapter (tests: secondary VkDevice on
// the primary GPU; env OX_SPLIT_RENDERING_ALLOW_SAME_ADAPTER=1), r.SplitRendering.Transfer, r.SplitRendering.Latency,
// r.SplitRendering.MinGainMs, r.SplitRendering.Calibrate. Stats: RenderStats::split.

#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/render_stats.hpp>
#include <oxwald/rhi/render_graph.hpp>

#include <array>
#include <string>
#include <vector>

namespace ox::rhi {
struct DeviceDesc;
}

namespace ox::render {

class Renderer;

enum class SplitRenderingMode : i32 { Off = 0, On = 1, Auto = 2 };

// One texture used by a portable pass: the bindless index of `texture` on the device that runs the pass is written
// as a u32 at `pushOffset` of the push constants.
struct PortableTextureBinding {
    rhi::RGTexture texture;
    u32 pushOffset = 0;
    bool write = false; // storage image written completely by the pass (overwrite); false = sampled read
};

struct PortablePassDesc {
    std::string name;
    rhi::PipelineHandle pipeline; // compute pipeline created on the primary device (the secondary recompiles its desc)
    std::array<u8, rhi::kMaxPushConstantSize> push{}; // texture index slots are filled per device
    u32 pushSize = 0;
    std::vector<PortableTextureBinding> textures;
    std::array<u32, 3> threads{1, 1, 1}; // dispatchThreads(x, y, z)

    template <class T>
    void setPush(const T& value) {
        static_assert(sizeof(T) <= rhi::kMaxPushConstantSize);
        std::memcpy(push.data(), &value, sizeof(T));
        pushSize = sizeof(T);
    }
};

struct PortableSegmentDesc {
    std::string name; // stable across frames: scheduler state and timings are keyed by (view, name)
    std::vector<PortablePassDesc> passes;
    f32 costHintMs = 0.1f; // expected primary GPU time until measured
    // false = keep it on the primary this frame (e.g. a pass also samples a texture outside the graph).
    bool portable = true;
};

// Declares the segment's passes on the view graph, or the cross-device export/import pair (see above).
// Textures read before being written inside the segment are its inputs; textures whose last access is a write are
// its results. Results must be fully overwritten by the segment.
void addPortableSegment(FeatureContext& ctx, const PortableSegmentDesc& desc);

// Optional primary-device extensions for the shared-memory transfer paths (external memory win32/fd + host);
// call before rhi::Device::create. Without them split rendering still works through staging copies.
void appendSplitRenderingVulkanExtensions(rhi::DeviceDesc& desc);

// Status of the renderer's split rendering (same as Renderer::stats().split, but current).
[[nodiscard]] SplitRenderingStats splitRenderingStatus(Renderer& renderer);
// Tests: makes the secondary device behave as if it was lost.
void splitRenderingSimulateDeviceLost(Renderer& renderer);

// Registers the SplitRendering feature (called by registerBuiltinFeatures).
void registerSplitRenderingFeature(FeatureRegistry& registry);

} // namespace ox::render
