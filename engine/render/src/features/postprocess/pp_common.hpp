#pragma once

// Private helpers shared by the postprocess-upscalers features.

#include <oxwald/core/cvar.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/snapshot.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/render_graph.hpp>

#include <memory>
#include <string_view>

namespace ox::render::pp {

// --- cvars (pp_cvars.cpp) ---
extern CVar<int> cvTaaQuality;          // r.TAA.Quality 0..3
extern CVar<int> cvTaaSamples;          // r.AntiAliasing.Samples (jitter period of plain TAA)
extern CVar<float> cvTaaSharpness;      // r.TAA.Sharpness
extern CVar<float> cvTaaCurrentWeight;  // r.TAA.CurrentFrameWeight
extern CVar<bool> cvTaaAntiFlicker;     // r.TAA.AntiFlicker
extern CVar<int> cvFxaaQuality;         // r.FXAA.Quality
extern CVar<float> cvUpscalerSharpness; // r.Upscaler.Sharpness
extern CVar<bool> cvBloom;
extern CVar<int> cvBloomQuality;
extern CVar<float> cvBloomIntensity;
extern CVar<bool> cvDof;
extern CVar<int> cvDofQuality;
extern CVar<bool> cvMotionBlur;
extern CVar<int> cvMotionBlurQuality;
extern CVar<bool> cvAutoExposure;
extern CVar<float> cvExposureMinEV;
extern CVar<float> cvExposureMaxEV;
extern CVar<float> cvExposureSpeedUp;
extern CVar<float> cvExposureSpeedDown;
extern CVar<float> cvSharpen;
extern CVar<bool> cvColorGrading;
extern CVar<bool> cvVignette;
extern CVar<bool> cvChromaticAberration;
extern CVar<bool> cvFilmGrain;
void registerCVars();

// Internal blackboard names.
inline constexpr std::string_view kBloomResult = "PostProcess.Bloom"; // half-res RGBA16F, pre-exposed
inline constexpr std::string_view kExposure = "Exposure";            // R32F 1×1 absolute exposure (core contract)

// The temporal upscaler (if any) that replaces TAA this frame. DLSS falls back to TAAU when it cannot run.
[[nodiscard]] UpscalerType effectiveUpscaler(const RenderSettings& s, const rhi::DeviceCaps& caps);
[[nodiscard]] bool temporalUpscalerActive(const RenderSettings& s, const rhi::DeviceCaps& caps);
// DLSS can run on this device (compiled with NGX, NVIDIA GPU, NGX initialised without error so far).
[[nodiscard]] bool dlssUsable(const rhi::DeviceCaps& caps);

// Settings of the view being set up (volumes blended for the view's camera position).
[[nodiscard]] PostProcessSettings viewSettings(FeatureContext& ctx);
// f-number of the view's camera: matched against the snapshot cameras by transform, else the primary camera.
[[nodiscard]] f32 viewAperture(FeatureContext& ctx);

// Rec.709 → Rec.709 white balance (CAT02 adaptation of a `temperature`/`tint` illuminant to D65, identity at 6500 K).
[[nodiscard]] glm::mat3 whiteBalanceMatrix(f32 temperature, f32 tint);

rhi::TextureDesc texDesc(VkFormat format, u32 width, u32 height, const char* name, u32 mips = 1);
[[nodiscard]] inline u32 divUp(u32 a, u32 b) { return (a + b - 1) / b; }

// Contrast adaptive sharpening (AMD CAS, MIT) of `src` into a new texture; used by TAA and the r.Sharpen pass.
class CasPass {
public:
    void init(rhi::Device& device);
    void shutdown(rhi::Device& device);
    // Returns the sharpened texture (same size/format as src, RGBA16F).
    rhi::RGTexture add(FeatureContext& ctx, rhi::RGTexture src, Extent2D size, f32 sharpness, const char* name);

private:
    rhi::PipelineHandle m_pipeline;
};

// Feature factories (one per .cpp).
std::unique_ptr<IRenderFeature> makeAutoExposureFeature();
std::unique_ptr<IRenderFeature> makeTaaFeature();
std::unique_ptr<IRenderFeature> makeTaauFeature();
std::unique_ptr<IRenderFeature> makeDlssFeature();
std::unique_ptr<IRenderFeature> makeFsr1Feature();
std::unique_ptr<IRenderFeature> makeBloomFeature();
std::unique_ptr<IRenderFeature> makeDepthOfFieldFeature();
std::unique_ptr<IRenderFeature> makeMotionBlurFeature();
std::unique_ptr<IRenderFeature> makeSharpenFeature();
std::unique_ptr<IRenderFeature> makeCompositeFeature();
std::unique_ptr<IRenderFeature> makeLdrPostFeature();

} // namespace ox::render::pp
