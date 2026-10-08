#pragma once

// SVGF-style spatio-temporal denoiser (Schied et al. 2017), shared by the ray traced effects and usable by any
// feature with a noisy screen-space signal (it does not need ray tracing hardware):
//   1. Temporal: reprojects history with Velocity, validates each of the 2×2 bilinear taps against the previous
//      linear depth + normal (disocclusion → history restarts), accumulates colour and luminance moments with
//      alpha = max(1 / (historyLength + 1), 1 / maxHistory).
//   2. Variance: temporal variance from the moments, or a 7×7 bilateral spatial estimate while the history is short.
//   3. À-trous: `iterations` passes of a 5×5 B3 kernel with step 1, 2, 4, ... and edge-stopping on depth (gradient
//      scaled), normals and luminance (variance guided). The first iteration's output becomes the colour history
//      (as in SVGF), the last one is written in the requested output format.
//   4. Optional joint-bilateral upsample when the signal is at half resolution.
//
// Shaders: engine/shaders/render/raytracing/denoise_*.comp. Histories are per view (FeatureContext::history).

#include <oxwald/render/render_feature.hpp>
#include <oxwald/rhi/handles.hpp>

#include <string>
#include <string_view>

namespace ox::render::rt {

enum class DenoiseOutput : u8 { RGBA16F, RGBA8, R8 };

struct DenoiserSettings {
    bool enabled = true;           // false: the input is passed through (still resolved to the output format)
    u32 iterations = 4;            // À-trous passes (0..5)
    u32 maxHistory = 32;           // frames: minimum blend factor 1 / maxHistory
    f32 phiColor = 4.0f;           // luminance edge stop (× sqrt(variance))
    f32 phiNormal = 128.0f;        // normal edge stop exponent
    f32 phiDepth = 1.0f;           // depth edge stop (× depth gradient)
    glm::vec4 lumaWeights{0.2126f, 0.7152f, 0.0722f, 0.0f}; // channel weights of the guiding luminance
    DenoiseOutput output = DenoiseOutput::RGBA16F;
    bool temporal = true;          // false: spatial only
};

struct DenoiserInputs {
    rhi::RGTexture signal;          // noisy input (RGBA16F), at render resolution / scale
    rhi::RGTexture depth, normals, velocity; // render resolution G-buffer (Normals: xyz normal, w roughness)
    Extent2D signalExtent;          // size of `signal`
    Extent2D renderExtent;          // size of the G-buffer
    u32 scale = 1;                  // render / signal ratio (1 or 2)
};

struct DenoiserOutputs {
    rhi::RGTexture result;   // denoised, signal resolution (or render resolution after upsampling)
    rhi::RGTexture variance; // R16F filtered variance (debug)
};

class SvgfDenoiser {
public:
    // Creates pipelines (plain compute: works on every device).
    bool initialize(rhi::Device& device);
    void shutdown(rhi::Device& device);
    [[nodiscard]] bool initialized() const { return m_temporal.valid(); }

    // Declares the denoiser passes. `name` keys the per-view histories (unique per effect). When `upsampleTo` has a
    // size, the result is upsampled (joint bilateral on depth/normals) to that extent.
    DenoiserOutputs denoise(FeatureContext& ctx, std::string_view name, const DenoiserInputs& inputs,
                            const DenoiserSettings& settings, std::optional<Extent2D> upsampleTo = std::nullopt);

    // Joint-bilateral upsample only (exposed for effects that skip denoising).
    rhi::RGTexture upsample(FeatureContext& ctx, std::string_view name, rhi::RGTexture source, Extent2D sourceExtent,
                            const DenoiserInputs& gbuffer, DenoiseOutput output);

private:
    rhi::PipelineHandle m_temporal, m_variance, m_atrous[3], m_upsample[3];
};

[[nodiscard]] VkFormat denoiseOutputFormat(DenoiseOutput o);

} // namespace ox::render::rt
