#pragma once

// Shared constants of the render module: well-known graph resource names, their formats, and small value types.
// The resource contracts (who writes / reads what) are documented in docs/dev/modules/render.md.

#include <oxwald/core/types.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <string_view>

namespace ox::render {

// Formats of the well-known resources. Feature code must use these constants instead of literals so a format
// change is a one-line edit.
namespace formats {
inline constexpr VkFormat kSceneColor = VK_FORMAT_R16G16B16A16_SFLOAT; // pre-exposed linear radiance
inline constexpr VkFormat kDepth = VK_FORMAT_D32_SFLOAT;               // reversed-Z (near = 1, far = 0)
inline constexpr VkFormat kNormals = VK_FORMAT_R16G16B16A16_SFLOAT;    // xyz world normal, w perceptual roughness
inline constexpr VkFormat kVelocity = VK_FORMAT_R16G16_SFLOAT;         // uvCurrent - uvPrevious
inline constexpr VkFormat kEntityId = VK_FORMAT_R32_UINT;              // 0 = none, else entt id + 1
inline constexpr VkFormat kHiZ = VK_FORMAT_R32_SFLOAT;                 // min (= farthest, reversed-Z) depth pyramid
inline constexpr VkFormat kShadowMask = VK_FORMAT_R8_UNORM;            // sun visibility
inline constexpr VkFormat kAO = VK_FORMAT_R8_UNORM;
inline constexpr VkFormat kReflections = VK_FORMAT_R16G16B16A16_SFLOAT; // rgb radiance, a = confidence
inline constexpr VkFormat kIndirectDiffuse = VK_FORMAT_R16G16B16A16_SFLOAT;
inline constexpr VkFormat kSceneColorLDR = VK_FORMAT_R8G8B8A8_UNORM;   // display-encoded (sRGB curve)
inline constexpr VkFormat kShadowDepth = VK_FORMAT_D32_SFLOAT;
inline constexpr VkFormat kSelectionMask = VK_FORMAT_R8_UNORM;
inline constexpr VkFormat kVolumetricFog = VK_FORMAT_R16G16B16A16_SFLOAT; // froxels: rgb in-scatter, a transmittance
} // namespace formats

// Names of the blackboard entries in FrameResources. Optional inputs may be absent; consumers fall back to the
// documented default.
namespace res {
// Written by the core pipeline.
inline constexpr std::string_view kSceneColorHDR = "SceneColorHDR";   // render res until Upscale, output res after
inline constexpr std::string_view kDepth = "Depth";                   // render res
inline constexpr std::string_view kNormals = "Normals";               // render res
inline constexpr std::string_view kVelocity = "Velocity";             // render res
inline constexpr std::string_view kEntityId = "EntityID";             // render res, editor views only
inline constexpr std::string_view kHiZ = "HiZ";                       // render res, full mip chain
inline constexpr std::string_view kLightClusters = "LightClusters";   // buffer, see clusters.glsl
inline constexpr std::string_view kSceneColorLDR = "SceneColorLDR";   // output res, after tonemapping
inline constexpr std::string_view kOutput = "Output";                 // the view's final target (imported)
// Shadows (ShadowsRaster or ShadowsRT).
inline constexpr std::string_view kShadowMask = "ShadowMask";         // render res, sun visibility (default 1)
inline constexpr std::string_view kShadowAtlas = "ShadowAtlas";       // spot lights
inline constexpr std::string_view kShadowCascades = "ShadowCascades"; // CSM 2D array
inline constexpr std::string_view kPointShadows = "PointShadows";     // cube array
// Optional lighting inputs (produced at InjectionPoint::Lighting or earlier, consumed by the lighting pass).
inline constexpr std::string_view kAO = "AO";                                   // default 1 (white)
inline constexpr std::string_view kReflectionsSpecular = "ReflectionsSpecular"; // default: IBL only
inline constexpr std::string_view kIndirectDiffuse = "IndirectDiffuse";         // default: IBL irradiance
inline constexpr std::string_view kVolumetricFog = "VolumetricFog";             // froxel 3D texture
// Editor.
inline constexpr std::string_view kSelectionMask = "SelectionMask";
} // namespace res

struct Extent2D {
    u32 width = 0;
    u32 height = 0;
    bool operator==(const Extent2D&) const = default;
    [[nodiscard]] f32 aspect() const { return height ? f32(width) / f32(height) : 1.0f; }
};

// Entity ids written to the EntityID target: 0 = no entity, otherwise the entt id + 1.
[[nodiscard]] constexpr u32 encodeEntityId(u32 enttValue) { return enttValue + 1u; }
[[nodiscard]] constexpr u32 decodeEntityId(u32 pickId) { return pickId - 1u; }
inline constexpr u32 kNoEntity = 0u;

} // namespace ox::render
