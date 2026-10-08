// OxwaldEngine render / world: CDLOD terrain data (mirrors TerrainGpuParams in features/world/terrain.cpp) and the
// vertex function shared by the depth, forward and shadow passes (identical code → invariant positions for the
// depth-EQUAL forward pass).
#ifndef OX_RENDER_WORLD_TERRAIN_COMMON_GLSL
#define OX_RENDER_WORLD_TERRAIN_COMMON_GLSL

#include <world/cdlod.glsl>
#include "world_common.glsl"

const uint OX_TERRAIN_HOLES = 1u;
const uint OX_TERRAIN_SPLAT = 2u;
const uint OX_TERRAIN_TRIPLANAR = 4u;

struct TerrainParams {
    vec4 originSize;   // origin x, origin z, world size, sample spacing
    vec4 heightParams; // height scale, height offset, resolution, 1 / resolution
    vec4 morphCamera;  // camera position used for LOD/morphing (main view), w = grid dimension
    vec4 splatRect;    // splat origin x, origin z, world size, resolution
    vec4 shading0;     // cos(triplanar slope), height blend, macro variation, tiling breakup
    vec4 shading1;     // 1 / layer tile metres, max layers, layer count, tessellation height
    vec4 skirt[4];     // skirt depth per LOD (16)
    vec4 tess;         // tessellation: range (m), max factor, displacement height (m), enabled
    uint heightTex;
    uint normalTex;
    uint holesTex;
    uint splat0;
    uint splat1;
    uint flags;
    uint entityId;
    uint pad;
    uint layerMaterial[8];
};

struct TerrainPatch {
    vec4 offsetSizeLod; // xy offset (world XZ), z size, w lod
    vec4 morph;         // start, end, 1/(end-start), quadrant mask
};

struct TerrainGridVertex {
    float u;
    float v;
    float skirt;
};

OX_READONLY_BUFFER(TerrainParamsBuffer, { TerrainParams p; });
OX_READONLY_BUFFER(TerrainPatchBuffer, { TerrainPatch p[]; });
OX_READONLY_BUFFER(TerrainGridBuffer, { TerrainGridVertex v[]; });

// Bilinear world height (metres) — mirrors world::Heightfield::sampleHeight (clamped at the borders).
float oxTerrainHeight(TerrainParamsBuffer tp, vec2 xz) {
    float res = tp.p.heightParams.z;
    vec2 s = clamp((xz - tp.p.originSize.xy) / tp.p.originSize.w, vec2(0.0), vec2(res - 1.0));
    ivec2 i0 = min(ivec2(s), ivec2(int(res) - 2));
    vec2 f = s - vec2(i0);
    uint tex = tp.p.heightTex;
    float h00 = texelFetch(OX_TEX2D(tex), i0, 0).r;
    float h10 = texelFetch(OX_TEX2D(tex), i0 + ivec2(1, 0), 0).r;
    float h01 = texelFetch(OX_TEX2D(tex), i0 + ivec2(0, 1), 0).r;
    float h11 = texelFetch(OX_TEX2D(tex), i0 + ivec2(1, 1), 0).r;
    float h = mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
    return tp.p.heightParams.y + h * tp.p.heightParams.x;
}

// CDLOD vertex: world position of grid vertex `g` of patch `inst` (morphed towards the next LOD, skirts pushed down).
vec3 oxTerrainVertex(TerrainParamsBuffer tp, TerrainPatch inst, TerrainGridVertex g) {
    vec2 uv = vec2(g.u, g.v);
    vec2 xz = inst.offsetSizeLod.xy + uv * inst.offsetSizeLod.z;
    float h = oxTerrainHeight(tp, xz);
    float k = oxCdlodMorphFactor(distance(tp.p.morphCamera.xyz, vec3(xz.x, h, xz.y)), inst.morph);
    uv = oxCdlodMorphVertex(uv, tp.p.morphCamera.w, k);
    xz = inst.offsetSizeLod.xy + uv * inst.offsetSizeLod.z;
    h = oxTerrainHeight(tp, xz);
    uint lod = min(uint(inst.offsetSizeLod.w), 15u);
    h -= g.skirt * tp.p.skirt[lod >> 2u][lod & 3u];
    return vec3(xz.x, h, xz.y);
}

// Texture coordinate of world XZ on a sample grid (texel centres on the samples).
vec2 oxTerrainSampleUv(vec2 xz, vec2 origin, float spacing, float res) {
    return ((xz - origin) / spacing + 0.5) / res;
}

// A quad is a hole when any of its four corner samples is (world::Heightfield convention).
bool oxTerrainIsHole(TerrainParamsBuffer tp, vec2 xz) {
    if ((tp.p.flags & OX_TERRAIN_HOLES) == 0u) return false;
    float res = tp.p.heightParams.z;
    vec2 s = (xz - tp.p.originSize.xy) / tp.p.originSize.w;
    ivec2 i0 = clamp(ivec2(floor(s)), ivec2(0), ivec2(int(res) - 2));
    uint tex = tp.p.holesTex;
    float m = texelFetch(OX_TEX2D(tex), i0, 0).r + texelFetch(OX_TEX2D(tex), i0 + ivec2(1, 0), 0).r +
              texelFetch(OX_TEX2D(tex), i0 + ivec2(0, 1), 0).r + texelFetch(OX_TEX2D(tex), i0 + ivec2(1, 1), 0).r;
    return m > 0.5;
}

// Detail height [0,1] for tessellation displacement: the strongest splat layer's height proxy (ORM occlusion, else
// albedo luminance), sampled at an explicit LOD (no derivatives in the tessellation stages).
float oxTerrainDetailHeight(TerrainParamsBuffer tp, SceneBuffer sb, vec2 xz, float lod) {
    uint layerCount = uint(tp.p.shading1.z);
    if ((tp.p.flags & OX_TERRAIN_SPLAT) == 0u || layerCount == 0u) return 0.5;
    vec2 suv = oxTerrainSampleUv(xz, tp.p.splatRect.xy, tp.p.splatRect.z / max(tp.p.splatRect.w - 1.0, 1.0), tp.p.splatRect.w);
    vec4 s0 = OX_SAMPLE_2D_LOD(tp.p.splat0, OX_SAMPLER_LINEAR_CLAMP, suv, 0.0);
    vec4 s1 = layerCount > 4u ? OX_SAMPLE_2D_LOD(tp.p.splat1, OX_SAMPLER_LINEAR_CLAMP, suv, 0.0) : vec4(0.0);
    float w[8] = float[](s0.x, s0.y, s0.z, s0.w, s1.x, s1.y, s1.z, s1.w);
    uint best = 0u;
    for (uint i = 1u; i < min(layerCount, 8u); ++i) {
        if (w[i] > w[best]) best = i;
    }
    Material m = sb.s.materials.m[tp.p.layerMaterial[best]];
    vec2 uv = xz * tp.p.shading1.x * m.uvTiling + m.uvOffset;
    if (m.ormTexture != OX_INVALID_INDEX) return OX_SAMPLE_2D_LOD(m.ormTexture, m.samplerIndex, uv, lod).r;
    if (m.albedoTexture != OX_INVALID_INDEX) {
        vec3 a = OX_SAMPLE_2D_LOD(m.albedoTexture, m.samplerIndex, uv, lod).rgb;
        return clamp(dot(a, vec3(0.2126, 0.7152, 0.0722)) * 2.0, 0.0, 1.0);
    }
    return 0.5;
}

vec3 oxTerrainMacroNormal(TerrainParamsBuffer tp, vec2 xz) {
    vec2 uv = oxTerrainSampleUv(xz, tp.p.originSize.xy, tp.p.originSize.w, tp.p.heightParams.z);
    vec3 n = OX_SAMPLE_2D(tp.p.normalTex, OX_SAMPLER_LINEAR_CLAMP, uv).xyz * 2.0 - 1.0;
    return normalize(n);
}

#endif
