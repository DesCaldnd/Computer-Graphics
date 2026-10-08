#pragma once

// Private helpers shared by the Translucency, Water and Particles features.

#include "../../renderer_impl.hpp"

#include <oxwald/render/features/translucency/translucency.hpp>

#include <string>
#include <vector>

namespace ox::render::translucency {

// Push constants of translucency/refraction_source.comp.
struct SourcePush {
    u32 mode = 0;
    u32 src = kInvalidIndex;
    u32 dst = kInvalidIndex;
    u32 depthSrc = kInvalidIndex;
    u32 depthDst = kInvalidIndex;
    u32 dstWidth = 0, dstHeight = 0;
    u32 srcWidth = 0, srcHeight = 0;
};

struct RefractionSources {
    rhi::RGTexture color; // SceneColorRefraction (mip chain)
    u32 mips = 1;
    rhi::RGTexture depth; // SceneDepthCopy
};

// Returns the frame's refraction sources, declaring the copy passes on first use (any feature may call it; the
// first caller in frame order defines what the copy contains). `pipeline` = translucency/refraction_source.comp.
RefractionSources ensureSources(FeatureContext& ctx, rhi::PipelineHandle pipeline);
rhi::PipelineHandle createSourcePipeline(rhi::Device& device);

// Declares the lighting inputs of translucent shading on a pass: light clusters, shadow maps, volumetric fog.
// Returns the VolumetricFog texture (invalid when absent).
rhi::RGTexture declareLightingReads(rhi::PassBuilder& pass, FrameResources& R);

// Defines for optional contracts of other areas, detected once per process from the shader sources:
//   OX_HAS_VOLUMETRIC_FOG_SAMPLE  render/volumetrics/fog_sample.glsl declares oxEvaluateVolumetricFog
//   OX_HAS_PLANAR_REFLECTIONS     render/reflections/planar.glsl declares oxSamplePlanarReflection
std::vector<rhi::ShaderDefine> contractDefines();

rhi::TextureDesc textureDesc(VkFormat format, Extent2D extent, const char* name, u32 mips = 1);

// Uploads RGBA8 pixels (mip 0 + generated mips) into a new sampled texture (synchronous, init time).
rhi::TextureHandle createTexture2D(rhi::Device& device, const char* name, u32 width, u32 height,
                                   const std::vector<u8>& rgba8, bool srgb = false, bool mips = true);

// Process-wide cvar readers used by more than one feature.
bool refractionEnabled();
i32 refractionMipCount(); // r.Refraction.Mips

} // namespace ox::render::translucency
