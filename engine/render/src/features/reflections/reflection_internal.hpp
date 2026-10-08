#pragma once

// Private helpers of the reflections-ao area: cvars, the shared scene-capture renderer, feature factories.

#include "../../renderer_impl.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>

#include <memory>
#include <unordered_map>

namespace ox::render::reflections {

// --- cvars (cvars.cpp) ---------------------------------------------------------------------------------------
namespace cv {
// Reflections group.
extern CVar<bool> probes;
extern CVar<int> probeResolution;
extern CVar<bool> probeRealtime;
extern CVar<int> probeRealtimeFaces;
extern CVar<int> probeMaxPerPixel;
extern CVar<int> probeCapturesPerFrame;
extern CVar<bool> ssr;
extern CVar<int> ssrQuality;
extern CVar<int> ssrMaxSteps;
extern CVar<bool> ssrHalfRes;
extern CVar<float> ssrMaxRoughness;
extern CVar<float> ssrThickness;
extern CVar<bool> ssrTemporal;
extern CVar<bool> planar;
extern CVar<float> planarScale;
extern CVar<int> planarMax;
extern CVar<int> captureMaxLocalLights;
// GlobalIllumination group.
extern CVar<int> aoMethod; // 0 off, 1 SSAO, 2 GTAO
extern CVar<int> aoQuality;
extern CVar<bool> aoHalfRes;
extern CVar<float> aoRadius;
extern CVar<float> aoIntensity;
extern CVar<bool> aoTemporal;
extern CVar<bool> giVolumes;
extern CVar<int> giProbesPerFrame;
std::vector<std::string> reflectionNames();
std::vector<std::string> aoNames();
std::vector<std::string> giNames();
} // namespace cv

// --- scene capture (capture.cpp) ------------------------------------------------------------------------------

// Renders opaque + masked geometry and the sky from an arbitrary camera (reflection probe faces, irradiance probe
// faces, mirrored planar views) into one layer of an RGBA16F target: rgb = radiance × constants.preExposure,
// a = distance to the camera. Reuses the renderer's vertex stage (passes/mesh.vert), material and lighting code.
class SceneCapture {
public:
    bool init(rhi::Device& device);
    void shutdown(rhi::Device& device);

    // Capture view constants derived from the current view's (shadows, IBL, sun, exposure): view/projection
    // replaced, no light clusters (local lights are looped in the shader), no planar reflections.
    static GpuViewConstants makeConstants(const GpuViewConstants& main, const glm::mat4& view, const glm::mat4& proj,
                                          glm::vec3 position, Extent2D size, f32 preExposure);

    struct Request {
        const char* name = "Capture";
        rhi::RGTexture target; // RGBA16F, ColorAttachment usage
        u32 layer = 0;
        Extent2D size;
        GpuViewConstants constants;
        DrawFilter filter;     // frustum / sphere culling of the capture
        bool sunDisk = false;
        rhi::ClearColor clear{};
        glm::uvec4 scissor{0, 0, 0, 0}; // x, y, width, height in target pixels (0 size = whole target)
    };
    void addPass(FeatureContext& ctx, const Request& request);

private:
    rhi::PipelineHandle m_mesh[kVariantCount];
    rhi::PipelineHandle m_sky;
};

// Persistent texture imported at most once per graph (several passes share one RGTexture → correct barriers).
class ImportCache {
public:
    rhi::RGTexture get(FeatureContext& ctx, rhi::TextureHandle texture);
    void reset() { m_map.clear(); }

private:
    std::unordered_map<u64, rhi::RGTexture> m_map;
};

// Common helpers.
[[nodiscard]] rhi::TextureDesc textureDesc(VkFormat format, Extent2D e, const char* name, u32 mips = 1);
[[nodiscard]] u64 frameCounter(FeatureContext& ctx);
[[nodiscard]] glm::mat4 rigidInverse(const glm::mat4& world); // world without scale → inverse
[[nodiscard]] glm::mat4 removeScale(const glm::mat4& world);
[[nodiscard]] u64 hashBytes(const void* data, usize size, u64 seed = 0);

// --- features ---------------------------------------------------------------------------------------------------
std::unique_ptr<IRenderFeature> makeReflectionsFeature();
std::unique_ptr<IRenderFeature> makeAmbientOcclusionFeature();
std::unique_ptr<IRenderFeature> makeIrradianceVolumesFeature();

class ReflectionsFeature;
class IrradianceVolumesFeature;
ReflectionsFeature* findReflections(Renderer& renderer);
IrradianceVolumesFeature* findIrradiance(Renderer& renderer);
void probesRequestBake(Renderer& renderer);
[[nodiscard]] bool probesBakeInProgress(Renderer& renderer);

} // namespace ox::render::reflections
