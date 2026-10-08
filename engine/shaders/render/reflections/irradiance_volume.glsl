// OxwaldEngine render: irradiance volumes — the IndirectDiffuse probe-grid layout shared by baked raster GI and
// ray traced DDGI (mirrors ox::render::GpuIrradianceVolumes in features/reflections/reflection_gpu_types.hpp).
//
// Probe data:
//   * OxIrradianceProbe (64 B): sh[0..3].rgb = SH L1 (L0, L1y, L1z, L1x) pre-convolved with the clamped cosine and
//     divided by π (oxIrradianceL1 returns the outgoing radiance of a white Lambertian surface); sh[0].w = valid.
//   * Depth moments: one RGBA16F atlas per volume, a 10×10 tile per probe (64 tiles per row; interior 8×8
//     octahedral texels, oxOctEncode of the direction from the probe, + 1-texel mirrored border),
//     x = mean distance, y = mean squared distance.
// A DDGI updater writes the same buffers (blend new ray results with hysteresis) and keeps sh[0].w = 1.
#ifndef OX_REFLECTIONS_IRRADIANCE_VOLUME_GLSL
#define OX_REFLECTIONS_IRRADIANCE_VOLUME_GLSL

#include <render/common/math.glsl>
#include <render/common/view.glsl>

struct OxIrradianceProbe {
    vec4 sh[4];
};

struct OxIrradianceVolume {
    mat4 worldToGrid;
    mat4 gridToWorld;
    uvec4 probeCount;      // xyz, w = first probe in the shared probe buffer
    vec4 params;           // x intensity, y normal bias (m), z view bias (m), w blend distance (m)
    vec4 params2;          // x max moment distance (m), y probe spacing (m)
    vec3 boxHalfExtents;
    uint momentAtlas;      // bindless sampled index (RGBA16F moments, OX_IRRADIANCE_ATLAS_PROBES_PER_ROW tiles per row)
    mat4 worldToLocal;
};

OX_BUFFER(OxIrradianceProbeBuffer, { OxIrradianceProbe p[]; });

OX_READONLY_BUFFER(OxIrradianceVolumeBuffer, {
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
    OxIrradianceProbeBuffer probes;
    uvec2 pad3;
    OxIrradianceVolume v[];
});

const uint OX_IRRADIANCE_MOMENT_TEXELS = 8u;
const uint OX_IRRADIANCE_MOMENT_TILE = 10u;
const uint OX_IRRADIANCE_ATLAS_PROBES_PER_ROW = 64u;

vec3 oxIrradianceL1(OxIrradianceProbe p, vec3 n) {
    vec3 r = p.sh[0].rgb * 0.282095 + (p.sh[1].rgb * n.y + p.sh[2].rgb * n.z + p.sh[3].rgb * n.x) * 0.488603;
    return max(r, vec3(0.0));
}

uvec3 oxIrradianceProbeCoord(OxIrradianceVolume v, uint linear) {
    uvec3 c = v.probeCount.xyz;
    return uvec3(linear % c.x, (linear / c.x) % c.y, linear / (c.x * c.y));
}

uint oxIrradianceProbeLinear(OxIrradianceVolume v, uvec3 coord) {
    uvec3 c = v.probeCount.xyz;
    return coord.x + coord.y * c.x + coord.z * c.x * c.y;
}

// Atlas uv of direction `dir` (from the probe) inside the tile of volume-local probe `local`.
vec2 oxIrradianceMomentUv(uint local, vec3 dir, vec2 atlasSize) {
    uvec2 tile = uvec2(local % OX_IRRADIANCE_ATLAS_PROBES_PER_ROW, local / OX_IRRADIANCE_ATLAS_PROBES_PER_ROW);
    vec2 oct = oxOctEncode(normalize(dir)); // [0,1]²
    vec2 texel = vec2(tile * OX_IRRADIANCE_MOMENT_TILE) + 1.0 + oct * float(OX_IRRADIANCE_MOMENT_TEXELS);
    return texel / atlasSize;
}

// Fade of a volume at worldPos: 1 inside the box, 0 at blendDistance outside it.
float oxIrradianceVolumeWeight(OxIrradianceVolume v, vec3 worldPos) {
    vec3 local = abs((v.worldToLocal * vec4(worldPos, 1.0)).xyz);
    vec3 outside = max(local - v.boxHalfExtents, vec3(0.0));
    float d = length(outside);
    return clamp(1.0 - d / max(v.params.w, 1e-3), 0.0, 1.0);
}

// DDGI-style trilinear probe interpolation with backface and Chebyshev visibility weights.
// P: surface position, N: shading normal, V: surface → camera (normalised). Returns radiance (irradiance / π).
vec3 oxSampleIrradianceVolume(OxIrradianceVolumeBuffer vols, uint index, vec3 P, vec3 N, vec3 V) {
    OxIrradianceVolume v = vols.v[index];
    vec3 biased = P + N * v.params.y + V * v.params.z;
    vec3 g = (v.worldToGrid * vec4(biased, 1.0)).xyz;
    vec3 maxCoord = vec3(v.probeCount.xyz) - 1.0;
    vec3 gc = clamp(g, vec3(0.0), maxCoord);
    vec3 base = min(floor(gc), max(maxCoord - 1.0, vec3(0.0)));
    vec3 alpha = clamp(gc - base, 0.0, 1.0);
    uint atlas = v.momentAtlas;
    vec2 atlasSize = atlas != OX_INVALID_INDEX ? vec2(textureSize(OX_TEX2D(atlas), 0)) : vec2(1.0);
    float maxDist = v.params2.x;
    OxIrradianceProbeBuffer probes = vols.probes;

    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < 8u; ++i) {
        uvec3 off = uvec3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
        uvec3 coord = uvec3(min(base + vec3(off), maxCoord));
        uint local = oxIrradianceProbeLinear(v, coord);
        uint probe = v.probeCount.w + local;
        OxIrradianceProbe pr = probes.p[probe];
        if (pr.sh[0].w < 0.5) continue;
        vec3 probePos = (v.gridToWorld * vec4(vec3(coord), 1.0)).xyz;
        vec3 tri = mix(1.0 - alpha, alpha, vec3(off));
        float w = tri.x * tri.y * tri.z;

        vec3 toProbe = probePos - P;
        float toProbeLen = length(toProbe);
        vec3 dirToProbe = toProbeLen > 1e-4 ? toProbe / toProbeLen : N;
        float backface = (dot(dirToProbe, N) + 1.0) * 0.5;
        float wv = backface * backface + 0.2;

        vec3 probeToPoint = biased - probePos;
        float dist = length(probeToPoint);
        if (atlas != OX_INVALID_INDEX && dist > 1e-4) {
            vec2 uv = oxIrradianceMomentUv(local, probeToPoint, atlasSize);
            vec2 m = OX_SAMPLE_2D_LOD(atlas, OX_SAMPLER_LINEAR_CLAMP, uv, 0.0).xy;
            float d = min(dist, maxDist);
            if (d > m.x) {
                float variance = abs(m.y - m.x * m.x);
                float diff = d - m.x;
                float cheb = variance / (variance + diff * diff);
                cheb = max(cheb * cheb * cheb, 0.0);
                wv *= max(cheb, 0.05);
            }
        }
        wv = max(wv, 1e-6);
        const float crush = 0.2;
        if (wv < crush) wv *= wv * wv / (crush * crush);
        w *= wv;
        sum += w * oxIrradianceL1(pr, N);
        wsum += w;
    }
    return wsum > 1e-6 ? sum / wsum * v.params.x : vec3(0.0);
}

#endif
