#pragma once

// Variable rate shading (VK_KHR_fragment_shading_rate, attachment rates). The VariableRateShading feature builds a
// per-tile shading rate image every frame and attaches it to the expensive colour passes of the view (forward opaque,
// world forward, sky, translucency, water, particles at full resolution, volumetric composite). Depth prepass,
// shadow maps, G-buffer-like outputs (Normals, Velocity, EntityID — inputs of TAA / DLSS / SSR), post-processing,
// UI and editor overlays never get it. See docs/dev/modules/render_vrs.md.
//
// CVars: r.VRS (0/1), r.VRS.Mode (Adaptive, Motion, Foveated, DebugOverlay), r.VRS.Threshold, r.VRS.MaxRate,
// r.VRS.Hysteresis, r.VRS.MotionScale, r.VRS.MotionPixels, r.VRS.Periphery, r.VRS.DepthOfField, r.VRS.Passes,
// r.VRS.Overlay.

#include <oxwald/core/types.hpp>
#include <oxwald/rhi/device_caps.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace ox::render {

class FeatureRegistry;

namespace vrs {

enum class Mode : i32 {
    Adaptive = 0,     // content (previous frame luminance gradients) + motion + depth of field (+ periphery if set)
    Motion = 1,       // only motion (and depth of field / periphery): coarser where the screen moves fast
    Foveated = 2,     // only the screen periphery
    DebugOverlay = 3, // Adaptive + colour overlay of the rate image
};

// Shading rate texel encoding (VkFragmentShadingRateAttachment): (log2(width) << 2) | log2(height).
[[nodiscard]] constexpr u32 encodeRate(u32 log2Width, u32 log2Height) { return (log2Width << 2) | log2Height; }
[[nodiscard]] constexpr u32 rateLog2Width(u32 rate) { return (rate >> 2) & 3u; }
[[nodiscard]] constexpr u32 rateLog2Height(u32 rate) { return rate & 3u; }

struct Availability {
    bool available = false;  // attachment shading rate supported with at least 2×2
    u32 texelSize = 0;       // rate image tile size in pixels (square)
    std::string reason;      // empty when available
};
[[nodiscard]] Availability availability(const rhi::DeviceCaps& caps);

// Tile size used for the rate image: the smallest supported square texel size ≥ 8 (NVIDIA: 16).
[[nodiscard]] u32 chooseTexelSize(const rhi::DeviceCaps& caps);

// Largest supported fragment size not exceeding (log2Width, log2Height): shrinks the larger axis first.
[[nodiscard]] u32 fitSupportedRate(u32 log2Width, u32 log2Height, u16 supportedMask);

// --- CPU reference of the per-tile decision (mirrors engine/shaders/render/vrs/vrs_common.glsl) ---

struct TileInput {
    bool statsValid = false; // previous-frame statistics found after reprojection
    f32 meanLuma = 0.0f;     // perceptual luma (vrs_common.glsl: oxVrsLuma), 0..1
    f32 errorX = 0.0f;       // RMS of horizontal per-pixel luma differences
    f32 errorY = 0.0f;
    f32 motionPixelsX = 0.0f; // |velocity| in render pixels per frame
    f32 motionPixelsY = 0.0f;
    f32 minCocPixels = 0.0f;  // smallest depth-of-field circle of confusion radius in the tile (0 = no DOF)
    f32 peripheryDistance = 0.0f; // 0 = screen centre, 1 = corner
    bool prevValid = false;
    u32 prevRate = 0;         // last frame's rate of the reprojected tile
};

struct DecisionParams {
    Mode mode = Mode::Adaptive;
    f32 threshold = 0.1f;   // r.VRS.Threshold
    f32 hysteresis = 0.25f; // r.VRS.Hysteresis
    f32 motionScale = 0.15f;// r.VRS.MotionScale
    f32 motionPixels = 6.0f;// r.VRS.MotionPixels (Motion mode: 2× from this speed, 4× from 3× this speed)
    f32 periphery = 0.0f;   // r.VRS.Periphery: start of the coarse periphery (0 = off; Foveated mode default 0.5)
    bool depthOfField = true;
    u32 maxLog2 = 2;        // r.VRS.MaxRate 2 → 1, 4 → 2
    u16 supportedMask = 0xFFFF;
};

// Returns the encoded rate for one tile.
[[nodiscard]] u32 decideRate(const TileInput& in, const DecisionParams& p);

// Pass names that may receive the rate image (r.VRS.Passes splits on ',') and the ones that never do, whatever the
// cvar says (depth/G-buffer, shadows, post, UI, editor). Prefix patterns end with '*'.
[[nodiscard]] const std::vector<std::string>& defaultPasses();
[[nodiscard]] bool isExcludedPass(std::string_view passName);
// Parses an r.VRS.Passes value: comma separated, blanks trimmed, empty items dropped; empty value = defaultPasses().
[[nodiscard]] std::vector<std::string> parsePassList(std::string_view value);

} // namespace vrs

// Registers the VariableRateShading feature (called from registerBuiltinFeatures).
void registerVrsFeatures(FeatureRegistry& registry);

} // namespace ox::render
