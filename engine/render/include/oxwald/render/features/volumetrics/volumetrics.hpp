#pragma once

// Volumetrics feature area: froxel volumetric fog (height fog + FogVolumeComponent, all clustered lights with
// shadows, IBL ambient, Henyey-Greenstein phase, temporal reprojection), volumetric clouds (CloudLayerComponent,
// ray marched with 1/16 checkerboard temporal updates) and their composite onto SceneColorHDR (after opaque + sky,
// before translucency), including sky fog / aerial perspective beyond the froxel grid.
//
// Resources (FrameResources):
//   VolumetricFog              3D RGBA16F, froxel grid: rgb in-scattered radiance × VIEW.preExposure integrated
//                              from the camera to the far edge of each slice, a = transmittance. Published at
//                              InjectionPoint::Lighting. Translucent passes sample it with
//                              render/volumetrics/fog_sample.glsl (oxEvaluateVolumetricFog).
//   VolumetricFogVisibility    (input, optional) 3D RGBA16F at the froxel grid size, written before the fog
//                              lighting pass (InjectionPoint::Lighting with order < kFogOrder, or earlier) by a ray
//                              traced visibility feature: r = sun visibility, g = local light visibility (ratio of
//                              shadowed to unshadowed local in-scattering), b = sky / ambient visibility, a unused.
//                              When present it replaces the shadow map lookups. Froxel centres:
//                              oxFroxelWorldPosition(view, vec3(coord) + 0.5 + oxFroxelJitter(view)).
//   VolumetricClouds           2D RGBA16F at cloud resolution: rgb in-scatter × preExposure, a = transmittance.
//   VolumetricCloudsDepth      2D RGBA16F: x = mean cloud distance (m), y = scene view depth used (m, 65000 = sky).

#include <oxwald/render/components/volumetrics.hpp>
#include <oxwald/render/snapshot.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace ox {
class World;
}

namespace ox::render {
class FeatureRegistry;
}

namespace ox::render::volumetrics {

inline constexpr std::string_view kFeatureName = "Volumetrics";
inline constexpr std::string_view kVolumetricFogVisibility = "VolumetricFogVisibility";
inline constexpr std::string_view kVolumetricClouds = "VolumetricClouds";
inline constexpr std::string_view kVolumetricCloudsDepth = "VolumetricCloudsDepth";
inline constexpr VkFormat kVisibilityFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Order of the Volumetrics feature at InjectionPoint::Lighting (visibility producers must run before it) and at
// InjectionPoint::AfterOpaque (composite, after the Sky at -1000).
inline constexpr i32 kFogOrder = 100;

// Froxel grid: uniform in screen x/y, exponential in view depth:
//   viewDepth(s) = (2^(s · log2(1 + far · scale)) - 1) / scale,  s ∈ [0, 1] (slice / gridZ)
// Small `scale` → nearly linear, large → more slices near the camera. Mirrors render/volumetrics/fog_common.glsl.
struct FroxelGrid {
    u32 x = 160, y = 90, z = 64;
    f32 farDistance = 128.0f; // view depth of the last slice's far edge (m)
    f32 depthScale = 32.0f;

    [[nodiscard]] f32 logRange() const;
    [[nodiscard]] f32 sliceToDepth(f32 slice01) const;
    [[nodiscard]] f32 depthToSlice(f32 viewDepth) const;
    [[nodiscard]] u64 froxelCount() const { return u64(x) * y * z; }
};

// Grid of the current cvar values (r.VolumetricFog.GridSize*, Distance, DepthDistributionScale), optionally with
// the VolumetricFogComponent distance override of a snapshot.
[[nodiscard]] FroxelGrid froxelGridFromCVars(const RenderSnapshot* snapshot = nullptr);

struct SnapshotFogVolume {
    FogVolumeComponent volume;
    glm::mat4 world{1.0f};
    u32 entityId = 0;
};

// Snapshot extension filled by the volumetrics extract hook (and by the world bridge for the wind).
struct VolumetricsSnapshot final : ISnapshotExtension {
    std::vector<SnapshotFogVolume> volumes;
    std::optional<VolumetricFogComponent> fog;
    std::optional<CloudLayerComponent> clouds;
    bool hasWind = false;
    glm::vec3 wind{0.0f}; // m/s, world space

    void clear() override;
};

// World integration: the WorldRenderData bridge calls this from its extract hook with the global WindComponent
// velocity (direction.x · speed, 0, direction.y · speed). Scrolls fog volume noise and the clouds.
void setWorldWind(RenderSnapshot& snapshot, glm::vec3 metresPerSecond);

// True when the snapshot has something for the froxel fog (enabled environment fog or a fog volume).
[[nodiscard]] bool fogActive(const RenderSnapshot& snapshot);

// Extract hook (installed by registerVolumetricsTypes): FogVolume, VolumetricFog and CloudLayer components.
void extractVolumetrics(const World& world, RenderSnapshot& out);

// Reflection + ComponentRegistry for the components above + the extract hook (idempotent; called from
// render::registerRenderTypes()).
void registerVolumetricsTypes();

// Adds the Volumetrics feature to a renderer's registry (also registers the types).
void registerVolumetricsFeatures(FeatureRegistry& registry);

} // namespace ox::render::volumetrics
