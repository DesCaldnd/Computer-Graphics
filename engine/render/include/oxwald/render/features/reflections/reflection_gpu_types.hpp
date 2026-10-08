#pragma once

// GPU data layouts of the reflections-ao area (scalar layout; mirrored in engine/shaders/render/reflections/*.glsl).
//
// * Reflection probes: GpuReflectionProbe array + clustered probe lists (same 16×9×24 grid as LightClusters).
// * Planar reflections: GpuPlanarReflections, addressed by GpuViewConstants::planarReflections (water/mirror
//   contract, see render/reflections/planar.glsl).
// * Irradiance volumes (IndirectDiffuse): GpuIrradianceVolumes + per-probe SH L1 + octahedral depth-moment atlas.
//   This is the shared layout of baked raster GI and ray traced DDGI: a DDGI implementation updates the same
//   probe SH buffer / moment atlas and reuses render/reflections/irradiance_volume.glsl to sample them.

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>

namespace ox::render {

// --- reflection probes -------------------------------------------------------------------------------------

inline constexpr u32 kMaxReflectionProbes = 256;          // visible probes per view
inline constexpr u32 kMaxReflectionProbesPerCluster = 16; // list capacity per cluster

enum GpuReflectionProbeFlags : u32 {
    kProbeBoxProjection = 1u << 0,
};

struct GpuReflectionProbe {
    glm::mat4 worldToLocal{1.0f};      // rigid: world → probe box space (metres, box centred at the origin)
    glm::vec3 extents{1.0f};           // box half size
    f32 blendDistance = 1.0f;          // influence fades from 1 at the box to 0 this far outside it
    glm::vec3 capturePosition{0.0f};   // world
    f32 intensity = 1.0f;
    glm::vec4 boundingSphere{0.0f};    // world xyz + radius (cluster culling)
    u32 cube = ~0u;                    // bindless sampled index of the prefiltered cube (GGX per mip)
    u32 mips = 1;                      // roughness r samples lod = r × (mips − 1), like the sky IBL
    u32 flags = 0;                     // GpuReflectionProbeFlags
    u32 pad = 0;
};
static_assert(sizeof(GpuReflectionProbe) == 128);

// --- planar reflections ------------------------------------------------------------------------------------

inline constexpr u32 kMaxPlanarReflections = 4;

// One reflector. Its texture is screen aligned with the view (same projection, rendered from the mirrored camera):
// a point on the plane is sampled at its own screen uv (+ normal distortion). RGBA16F, radiance × preExposure.
struct GpuPlanarReflection {
    glm::vec4 plane{0.0f, 1.0f, 0.0f, 0.0f}; // world plane (normal xyz, d): dot(n, p) + d = 0, normal faces the viewer
    glm::vec4 axisX{0.0f};  // xyz world tangent, w half size (0 = unbounded)
    glm::vec4 axisZ{0.0f};  // xyz world bitangent, w half size (0 = unbounded)
    glm::vec4 origin{0.0f}; // xyz world centre, w = intensity
    glm::vec4 params{0.0f}; // x distortion (uv per unit normal deviation), y max roughness, z uv scale (texture
                            // res / render res), w unused
    u32 texture = ~0u;      // bindless sampled index (sample with linear clamp at the screen uv)
    u32 pad0 = 0, pad1 = 0, pad2 = 0;
};
static_assert(sizeof(GpuPlanarReflection) == 96);

struct GpuPlanarReflections {
    u32 count = 0;
    u32 pad0 = 0, pad1 = 0, pad2 = 0;
    GpuPlanarReflection reflectors[kMaxPlanarReflections];
};
static_assert(sizeof(GpuPlanarReflections) == 16 + 96 * kMaxPlanarReflections);

// --- irradiance volumes (IndirectDiffuse) ------------------------------------------------------------------

inline constexpr u32 kMaxIrradianceVolumes = 8;      // per view
inline constexpr u32 kIrradianceMomentTexels = 8;    // interior octahedral texels per probe side
inline constexpr u32 kIrradianceMomentTile = 10;     // + 1 texel border on each side (DDGI layout)
inline constexpr u32 kIrradianceAtlasProbesPerRow = 64;

// Per probe: 4 × vec4. rgb of sh[i] = SH L1 coefficient i (order: L0, L1y, L1z, L1x — the first four bands of
// oxEvalSH9), already convolved with the clamped cosine and divided by π, so evaluating gives the outgoing radiance
// of a white Lambertian surface (= IndirectDiffuse). sh[0].w = 1 when the probe holds valid data, sh[1].w = probe
// state flags (reserved for DDGI: relocation/classification), sh[2..3].w unused.
struct GpuIrradianceProbe {
    glm::vec4 sh[4];
};
static_assert(sizeof(GpuIrradianceProbe) == 64);

// Volume grid: probe (i, j, k) sits at gridToWorld × (i, j, k, 1); its data is entry `firstProbe + linear index`
// of the shared probe buffer, linear index = i + j·countX + k·countX·countY. Depth moments live in the volume's own
// moment atlas (RGBA16F: x = mean distance, y = mean squared distance), tile `linear index` at
// (t % kIrradianceAtlasProbesPerRow, t / kIrradianceAtlasProbesPerRow), 10×10 texels: the octahedral interior
// (oxOctEncode of the direction from the probe) at texels 1..8 plus a DDGI-style mirrored 1-texel border.
struct GpuIrradianceVolume {
    glm::mat4 worldToGrid{1.0f};       // world → continuous probe coordinates
    glm::mat4 gridToWorld{1.0f};
    glm::uvec4 probeCount{0};          // xyz, w = first probe index in the shared probe buffer
    glm::vec4 params{0.0f};            // x intensity, y normal bias (m), z view bias (m), w blend distance (m)
    glm::vec4 params2{0.0f};           // x max moment distance (m), y probe spacing (m, smallest axis), zw unused
    glm::vec3 boxHalfExtents{0.0f};    // box half size (metres) in worldToLocal space, for the blend fade
    u32 momentAtlas = ~0u;             // bindless sampled index of this volume's moment atlas
    glm::mat4 worldToLocal{1.0f};      // rigid: world → box space (centred)
};
static_assert(sizeof(GpuIrradianceVolume) == 256);

// Per view (frame memory), volumes in blending order (priority descending).
struct GpuIrradianceVolumes {
    u32 count = 0;
    u32 pad0 = 0, pad1 = 0, pad2 = 0;
    u64 probes = 0;              // address of the shared GpuIrradianceProbe[] buffer
    u64 pad3 = 0;
    GpuIrradianceVolume volumes[kMaxIrradianceVolumes];
};

} // namespace ox::render
