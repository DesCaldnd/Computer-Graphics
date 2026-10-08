#pragma once

// Reflections & ambient occlusion area (raster): reflection probes, SSR, planar reflections, GTAO/SSAO and baked
// irradiance volumes. Features (each with its r.Feature.<Name> toggle):
//
//   "Reflections"       group "Reflections"     Lighting + AfterOpaque → ReflectionsSpecular (+ HiZClosest,
//                                                 PlanarReflection). Probes (clustered lists, box projection,
//                                                 sky IBL fallback) + planar + SSR (Hi-Z, stochastic GGX, spatial
//                                                 resolve, temporal accumulation, previous-frame colour pyramid).
//   "AmbientOcclusion"  group "AO"              Lighting → AO (GTAO or SSAO, half/full res, temporal denoise).
//   "IrradianceVolumes" group "IndirectDiffuse" Lighting → IndirectDiffuse (baked SH L1 probe grids with DDGI-style
//                                                 depth-moment visibility; layout in reflection_gpu_types.hpp).
//
// The ray traced variants (raytracing area) join the same groups with a higher priority.
// Components: components/reflections.hpp. Shaders: engine/shaders/render/reflections/.

#include <oxwald/core/result.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/render/components/reflections.hpp>
#include <oxwald/render/features/reflections/reflection_gpu_types.hpp>
#include <oxwald/render/snapshot.hpp>

#include <filesystem>
#include <string_view>
#include <vector>

namespace ox::render {

class FeatureRegistry;
class Renderer;

namespace reflections {

// Graph resources published by this area (besides the core contracts AO / ReflectionsSpecular / IndirectDiffuse).
namespace res {
// R32F, render resolution, full mip chain: max (= nearest, reversed-Z) depth pyramid for screen-space tracing.
// The core "HiZ" is the min (farthest) pyramid used for occlusion culling.
inline constexpr std::string_view kHiZClosest = "HiZClosest";
// RGBA16F screen-aligned planar reflection of the primary reflector, radiance × preExposure (the water contract;
// every reflector is listed in VIEW.planarReflections, see render/reflections/planar.glsl).
inline constexpr std::string_view kPlanarReflection = "PlanarReflection";
// RGBA16F render resolution: rgb SSR radiance (not pre-exposed), a = confidence (after temporal accumulation).
inline constexpr std::string_view kSSR = "SSR";
} // namespace res

// Feature names (also r.Feature.<Name>) and exclusive groups.
inline constexpr std::string_view kReflectionsFeature = "Reflections";
inline constexpr std::string_view kAmbientOcclusionFeature = "AmbientOcclusion";
inline constexpr std::string_view kIrradianceVolumesFeature = "IrradianceVolumes";

// --- snapshot extension (filled by the extract hook) --------------------------------------------------------

struct SnapshotReflectionProbe {
    u32 entityId = 0;
    Uuid uuid;                 // IdComponent (baked data key), nil when the entity has none
    glm::mat4 world{1.0f};
    ReflectionProbeComponent probe;
};

struct SnapshotPlanarReflector {
    u32 entityId = 0;
    glm::mat4 world{1.0f};
    PlanarReflectorComponent reflector;
};

struct SnapshotIrradianceVolume {
    u32 entityId = 0;
    Uuid uuid;
    glm::mat4 world{1.0f};
    IrradianceVolumeComponent volume;
};

struct ReflectionSnapshot final : ISnapshotExtension {
    std::vector<SnapshotReflectionProbe> probes;
    std::vector<SnapshotPlanarReflector> planars;
    std::vector<SnapshotIrradianceVolume> volumes;
    void clear() override {
        probes.clear();
        planars.clear();
        volumes.clear();
    }
};

// Extract hook (installed by registerReflectionTypes()): also usable directly by custom extract paths.
void extractReflections(const World& world, RenderSnapshot& out);

// --- baking ----------------------------------------------------------------------------------------------------

// Prefiltered (GGX per mip) cube map of a probe: RGBA16F texels, mip-major then face (+X,-X,+Y,-Y,+Z,-Z).
struct BakedCubemap {
    u32 size = 0; // mip 0 face size
    u32 mips = 0;
    std::vector<u8> data;
    [[nodiscard]] bool valid() const { return size > 0 && mips > 0 && !data.empty(); }
};

// Baked irradiance volume: probe SH (GpuIrradianceProbe per probe) + moment tiles (RGBA16F, 10×10 per probe,
// probes in linear order, one tile after the other).
struct BakedIrradianceVolume {
    glm::ivec3 probeCount{0};
    std::vector<GpuIrradianceProbe> probes;
    std::vector<u8> moments;
    [[nodiscard]] bool valid() const { return !probes.empty(); }
};

// "Bake probes": re-captures every reflection probe (all update modes) and irradiance volume over the next frames
// (budgets: r.ReflectionProbes.CapturesPerFrame, r.GI.IrradianceVolumes.ProbesPerFrame). Keep rendering frames
// until bakeInProgress() is false, then read the results back for saving.
void requestBake(Renderer& renderer);
[[nodiscard]] bool bakeInProgress(Renderer& renderer);

// Readback of the current GPU data (waits for the GPU; call between frames). Keyed by the entity Uuid.
[[nodiscard]] std::vector<std::pair<Uuid, BakedCubemap>> readBakedProbes(Renderer& renderer);
[[nodiscard]] std::vector<std::pair<Uuid, BakedIrradianceVolume>> readBakedVolumes(Renderer& renderer);
// Installs previously baked data (scene load): Baked probes / volumes with this Uuid use it instead of capturing.
void setBakedProbe(Renderer& renderer, const Uuid& probe, BakedCubemap data);
void setBakedVolume(Renderer& renderer, const Uuid& volume, BakedIrradianceVolume data);

// Render-owned binary containers: ".oxcube" (probe) and ".oxirr" (irradiance volume). Little endian, magic + version.
Status saveOxCube(const std::filesystem::path& path, const BakedCubemap& cube);
Result<BakedCubemap> loadOxCube(const std::filesystem::path& path);
Status saveOxIrradiance(const std::filesystem::path& path, const BakedIrradianceVolume& volume);
Result<BakedIrradianceVolume> loadOxIrradiance(const std::filesystem::path& path);

// --- math shared with the shaders (exposed for tests) ------------------------------------------------------------

// View and projection of cube face `face` (+X,-X,+Y,-Y,+Z,-Z) at `position`, matching oxCubeDirection: texel uv of
// the rendered face maps to the cube direction oxCubeDirection(face, uv). The basis is mirrored relative to a
// regular camera, so captures rasterise with clockwise front faces. Reversed-Z, infinite far plane.
void cubeFaceMatrices(u32 face, glm::vec3 position, f32 nearPlane, glm::mat4& view, glm::mat4& proj);
// Reflection matrix about the plane dot(n, p) + d = 0 (n normalised).
[[nodiscard]] glm::mat4 reflectionMatrix(glm::vec4 plane);
// Lengyel's oblique near plane for a reversed-Z projection: `proj` is a finite reversed-Z perspective (Vulkan Y
// flip allowed), `clipPlaneView` the clip plane in view space (normal towards the visible side). Returns the
// projection whose near plane is the clip plane and whose far plane keeps the original far corner.
[[nodiscard]] glm::mat4 obliqueReversedZ(const glm::mat4& proj, glm::vec4 clipPlaneView);
// Finite reversed-Z perspective with the x/y scales of `proj` (possibly infinite / jittered).
[[nodiscard]] glm::mat4 finiteReversedZ(const glm::mat4& proj, f32 nearPlane, f32 farPlane);

} // namespace reflections

// Adds the Reflections / AmbientOcclusion / IrradianceVolumes features to a renderer (renderer.cpp marker line).
// Also registers the area's types, extract hook and cvars.
void registerReflectionFeatures(FeatureRegistry& registry);
// Forces registration of the area's cvars (file statics).
void registerReflectionCVars();

} // namespace ox::render
