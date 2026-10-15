#pragma once

// Variable rate shading: render graph building blocks shared by the VariableRateShading feature and the GPU tests
// (which drive the passes with synthetic inputs).

#include <oxwald/render/features/vrs/vrs.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/rhi/render_graph.hpp>

#include <string>
#include <vector>

namespace ox::rhi {
class Device;
}

namespace ox::render::vrs {

inline constexpr VkFormat kRateFormat = VK_FORMAT_R8_UINT;
inline constexpr VkFormat kStatsFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
inline constexpr std::string_view kShadingRateResource = "ShadingRate"; // FrameResources name of this frame's image

struct Pipelines {
    rhi::PipelineHandle analyze, generate, overlay;
    bool create(rhi::Device& device);
    void destroy(rhi::Device& device);
};

[[nodiscard]] inline Extent2D tileCount(Extent2D render, u32 tileSize) {
    return {(render.width + tileSize - 1) / tileSize, (render.height + tileSize - 1) / tileSize};
}

struct GenerateInputs {
    VkDeviceAddress view = 0; // only dereferenced for depth of field
    rhi::RGTexture depth, velocity, prevStats, prevRate, outRate;
    bool historyValid = false;
    DecisionParams params;
    bool depthOfField = false;
    f32 cocScale = 0.0f; // render pixels per unit of (1 - focus / depth)
    f32 focus = 0.0f;
    Extent2D renderSize;
    u32 tileSize = 16;
    u32 refreshPeriod = 0; // r.VRS.RefreshPeriod: every N frames each tile is shaded at full rate once (0 = never)
    u32 frame = 0;         // frame counter for the staggered refresh pattern
};
void addGeneratePass(rhi::RenderGraph& graph, const Pipelines& p, const GenerateInputs& in);

struct AnalyzeInputs {
    rhi::RGTexture color, rate, outStats;
    // Statistics of the opaque image of the same frame (AfterOpaque): per axis the larger relative error of the two
    // is kept, so opaque surfaces seen only through refraction / water / glass keep their detail.
    rhi::RGTexture opaqueStats;
    bool combineOpaque = false;
    bool rateValid = false;
    Extent2D renderSize;
    u32 tileSize = 16;
};
void addAnalyzePass(rhi::RenderGraph& graph, const Pipelines& p, const AnalyzeInputs& in,
                    std::string_view name = "VRS.Analyze");

// Attaches `rate` to every declared graphics pass whose name is in `names` (and not excluded) and whose render area
// equals `renderSize`. Returns how many passes got it. Does not allocate.
u32 attachToPasses(rhi::RenderGraph& graph, rhi::RGTexture rate, u32 tileSize, Extent2D renderSize,
                   const std::vector<std::string>& names);


} // namespace ox::render::vrs
