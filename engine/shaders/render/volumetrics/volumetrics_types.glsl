// OxwaldEngine volumetrics: GPU structures of the fog / cloud passes (mirror engine/render/src/features/
// volumetrics/volumetrics_gpu.hpp, scalar layout) and extra storage image aliases.
#ifndef OX_VOLUMETRICS_TYPES_GLSL
#define OX_VOLUMETRICS_TYPES_GLSL

#include <render/common/view.glsl>

layout(set = 0, binding = 1, rgba8) uniform image3D oxVolImages3D_rgba8[];

// vec4-only layout (no vec3 / matrix members: robust across SPIR-V → MSL scalar-layout translation).
struct FogVolume {
    vec4 unitRow0;      // world → unit shape space, affine rows (box: [-1,1]³, sphere / ellipsoid: unit ball)
    vec4 unitRow1;
    vec4 unitRow2;
    vec4 albedoDensity; // rgb single scattering albedo, a extinction (1/m)
    vec4 emissionG;     // rgb emission (display-relative), a anisotropy
    vec4 params;        // x falloff, y noise intensity, z noise frequency (1/m), w shape (0 box, 1 sphere, 2 ellipsoid)
    vec4 noiseOffset;   // xyz metres (velocity × time)
};

OX_READONLY_BUFFER(FogVolumeBuffer, { FogVolume v[]; });

const uint OX_FOG_SUN_SHADOWS = 1u;
const uint OX_FOG_LOCAL_SHADOWS = 2u;
const uint OX_FOG_CLOUD_SHADOWS = 4u;
const uint OX_FOG_VISIBILITY = 8u;
const uint OX_FOG_JITTER = 16u;

struct FogConstants {
    uvec4 grid;          // xyz froxels, w = fog volume count
    vec4 jitter;         // xyz froxel-space jitter of this frame, w = history weight (0 = no history)
    vec4 prevCamera;     // xyz previous camera position, w = history exposure scale
    vec4 prevForward;    // xyz previous camera forward
    vec4 emission;       // rgb height fog emission (display-relative), w = local light intensity
    vec4 wind;           // xyz world wind (m/s), w = time (s)
    vec4 cloudLayer;     // x base altitude, y thickness, z coverage, w shadow strength
    vec4 cloudWeather;   // xy weather offset (m), z 1 / weather scale, w extinction (1/m)
    FogVolumeBuffer volumes;
    uint noiseTexture;   // 3D RGBA8 (r = Perlin-Worley), tiles every noise period
    uint weatherTexture; // 2D RGBA8 weather map (r = coverage, g = type)
    uint visibilityTexture;
    uint flags;
    uint pad0;
    uint pad1;
};

// 16-byte aligned: the struct holds a 64-bit buffer reference (loads of the whole struct need 8-byte alignment).
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer FogConstantsBuffer { FogConstants c; };

struct CloudConstants {
    vec4 layer;     // x base altitude, y thickness, z coverage, w type
    vec4 shape;     // x extinction (1/m), y 1 / shape scale, z 1 / detail scale, w detail strength
    vec4 wind;      // xyz shape noise offset (m), w 1 / weather scale
    vec4 weather;   // xy weather offset (m), z planet radius (m), w max march distance (m)
    vec4 lighting;  // x ambient, y sun, z forward g, w backward g
    vec4 albedo;    // rgb, w = horizon fade start (fraction of the max distance)
    uvec4 size;     // xy trace size, zw cloud size
    uvec4 march;    // x checker (1, 2, 4), yz pixel offset updated this frame, w primary steps
    uvec4 misc;     // x light steps, y history valid, z frame index, w history exposure scale (float bits)
    uint baseNoise;
    uint detailNoise;
    uint weatherTexture;
    uint depthTexture;
};

OX_READONLY_BUFFER(CloudConstantsBuffer, { CloudConstants c; });

#endif
