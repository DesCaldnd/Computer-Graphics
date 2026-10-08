#pragma once

// Translucency, refraction, water and GPU particles (area translucency-water-particles).
//
// Features (registered by registerTranslucencyFeatures, each with its r.Feature.<Name> toggle):
//   "Translucency" AfterOpaque: SceneColorRefraction (blurred mip chain of SceneColorHDR) + SceneDepthCopy (R32F),
//                  back-face depth of refractive objects; Translucency: refractive objects (screen-space refraction,
//                  Beer-Lambert absorption, Schlick Fresnel, TIR), transparent objects (Weighted Blended OIT or
//                  sorted back to front), hashed alpha test toggle for the depth prepass.
//   "Water"        AfterOpaque: caustics on underwater geometry; Translucency: Gerstner water surface (refraction,
//                  absorption, SSR / planar / sky reflections, shore + crest foam), underwater post effect.
//   "Particles"    AfterOpaque: GPU simulation (emit / simulate with depth collisions / indirect args);
//                  Translucency: billboard / stretched / mesh particles into a low-resolution buffer, soft particles,
//                  depth-aware upsampling composite.
//
// Data: components in <oxwald/render/components/translucency.hpp> are extracted into TranslucencySnapshot (a
// RenderSnapshot extension) by an extract hook installed by registerTranslucencyTypes().

#include <oxwald/core/math.hpp>
#include <oxwald/render/components/translucency.hpp>
#include <oxwald/render/gpu_types.hpp>
#include <oxwald/render/snapshot.hpp>

#include <string_view>
#include <vector>

namespace ox::render {

class FeatureRegistry;

namespace res {
inline constexpr std::string_view kSceneColorRefraction = "SceneColorRefraction"; // RGBA16F mip chain, pre-exposed
inline constexpr std::string_view kSceneDepthCopy = "SceneDepthCopy";             // R32F copy of opaque Depth
inline constexpr std::string_view kRefractionBackDepth = "RefractionBackDepth";   // D32, nearest back faces
// Optional input from the reflections team: planar reflection of the water plane, RGBA16F pre-exposed radiance at
// render resolution, sampled with the screen uv (offset by the water normal). Water falls back to SSR / sky.
inline constexpr std::string_view kPlanarReflection = "PlanarReflection";
} // namespace res

// GpuViewConstants::flags bit set by the Translucency feature when hashed (stochastic) alpha testing is active.
inline constexpr u32 kViewFlagHashedAlpha = 1u << 2;
// GpuMaterial::flags bit (gpu_types.hpp kMaterialSorted): transparent material that always takes the sorted path
// (assets: renderQueueOffset != 0).
inline constexpr u32 kMaterialSortedTranslucency = kMaterialSorted;

// world::GerstnerParamsGpu layout (528 bytes): 16 × {dirK = (dir.x, dir.z, k, omega), amp = (A, qa, phase, 0)},
// info = (count, base height, time, 0). Byte-compatible: a memcpy from gameplay::WaterRenderItem::params works.
struct GerstnerParams {
    struct Wave {
        glm::vec4 dirK{0.0f};
        glm::vec4 amp{0.0f};
    };
    Wave waves[16];
    glm::vec4 info{0.0f};
};
static_assert(sizeof(GerstnerParams) == 528);

// Packs waves exactly like world::GerstnerWaves::setWaves (k = 2π/λ, ω = sqrt(g k)·speedScale, qa = steepness/(k N)).
GerstnerParams packGerstnerWaves(const std::vector<WaterWave>& waves, f32 baseHeight, f32 time);
// CPU evaluation (tests, camera-underwater test): Lagrangian displacement and Eulerian height at world XZ.
glm::vec3 gerstnerDisplacement(const GerstnerParams& p, glm::vec2 x0, f32 time);
f32 gerstnerHeight(const GerstnerParams& p, glm::vec2 xz, f32 time, u32 iterations = 4);
// Sum of wave amplitudes (vertical bound of the surface around the base height).
f32 gerstnerAmplitudeSum(const GerstnerParams& p);

struct SnapshotWater {
    u32 entityId = 0;
    GerstnerParams params; // info.y = base height, info.z = time
    glm::vec2 center{0.0f};
    glm::vec2 size{0.0f};  // <= 0 = unbounded
    WaterSurfaceComponent look; // optics/foam/caustics parameters (waves/size fields unused here)
};

struct SnapshotParticleEmitter {
    u32 entityId = 0;
    glm::mat4 world{1.0f};
    ParticleEmitterComponent emitter;
};

struct TranslucencySnapshot final : ISnapshotExtension {
    std::vector<SnapshotWater> water;
    std::vector<SnapshotParticleEmitter> emitters;
    void clear() override {
        water.clear();
        emitters.clear();
    }
};

// For bridges that own their water data (e.g. gameplay::WorldRenderData::water → memcpy params): call after
// render::extract() on the same snapshot.
void addWaterSurface(RenderSnapshot& snapshot, const SnapshotWater& water);

// Called from registerBuiltinFeatures (renderer.cpp marker line).
void registerTranslucencyFeatures(FeatureRegistry& registry);

} // namespace ox::render
