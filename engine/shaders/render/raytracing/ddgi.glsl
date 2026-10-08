// OxwaldEngine ray tracing: DDGI probe volume (mirrors ox::render::rt::DdgiVolumeGpu, see ddgi.hpp for the layout).
// Sampling (oxDdgiIrradiance) works from any shader stage; it needs no ray tracing support.
#ifndef OX_RT_DDGI_GLSL
#define OX_RT_DDGI_GLSL

#include <render/common/math.glsl>
#include <common/bindless.glsl>

const uint OX_DDGI_IRRADIANCE_TEXELS = 8u;
const uint OX_DDGI_DEPTH_TEXELS = 16u;

OX_BUFFER(DdgiProbeStateBuffer, { ivec4 s[]; });

struct DdgiVolume {
    ivec3 minCoord;        // world probe coordinate of the window's first probe
    float spacing;
    ivec3 counts;
    uint raysPerProbe;
    uint irradianceTexture; // bindless sampled indices (RGBA16F / RG16F atlases)
    uint depthTexture;
    uint enabled;
    uint frame;
    float normalBias;
    float viewBias;
    float hysteresis;
    float depthSharpness;
    vec4 rayRotation;      // quaternion
    DdgiProbeStateBuffer probeState;
    uint pad0;
    uint pad1;
};

// Reference to a DdgiVolume in memory (8-byte aligned: it contains a 64-bit address).
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer DdgiVolumeRef { DdgiVolume v; };

int oxDdgiMod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }

uint oxDdgiStorageIndex(DdgiVolume v, ivec3 worldCoord) {
    ivec3 s = ivec3(oxDdgiMod(worldCoord.x, v.counts.x), oxDdgiMod(worldCoord.y, v.counts.y),
                    oxDdgiMod(worldCoord.z, v.counts.z));
    return uint(s.x + s.y * v.counts.x + s.z * v.counts.x * v.counts.y);
}

ivec3 oxDdgiWorldCoord(DdgiVolume v, uint storage) {
    ivec3 slot = ivec3(int(storage) % v.counts.x, (int(storage) / v.counts.x) % v.counts.y,
                       int(storage) / (v.counts.x * v.counts.y));
    return v.minCoord + ivec3(oxDdgiMod(slot.x - v.minCoord.x, v.counts.x), oxDdgiMod(slot.y - v.minCoord.y, v.counts.y),
                              oxDdgiMod(slot.z - v.minCoord.z, v.counts.z));
}

vec3 oxDdgiProbePosition(DdgiVolume v, ivec3 worldCoord) { return (vec3(worldCoord) + 0.5) * v.spacing; }

uvec2 oxDdgiTileInterior(DdgiVolume v, uint storage, uint texels) {
    uint perRow = uint(v.counts.x * v.counts.y);
    uint tile = texels + 2u;
    return uvec2((storage % perRow) * tile + 1u, (storage / perRow) * tile + 1u);
}

uvec2 oxDdgiAtlasSize(DdgiVolume v, uint texels) {
    uint tile = texels + 2u;
    return uvec2(uint(v.counts.x * v.counts.y) * tile, uint(v.counts.z) * tile);
}

vec3 oxQuatRotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

// Spherical Fibonacci direction i of n, rotated by the per-frame random rotation.
vec3 oxDdgiRayDirection(DdgiVolume v, uint i) {
    float n = float(v.raysPerProbe);
    float phi = OX_TWO_PI * fract(float(i) / 1.61803398875);
    float cosTheta = 1.0 - (2.0 * float(i) + 1.0) / n;
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return oxQuatRotate(v.rayRotation, vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}

// uv of an octahedral direction inside a probe tile of the atlas.
vec2 oxDdgiAtlasUv(DdgiVolume v, uint storage, vec3 dir, uint texels) {
    vec2 oct = oxOctEncode(dir); // [0,1]
    vec2 interior = vec2(oxDdgiTileInterior(v, storage, texels));
    vec2 atlas = vec2(oxDdgiAtlasSize(v, texels));
    return (interior + oct * float(texels)) / atlas;
}

// Irradiance / π (diffuse radiance of a white Lambertian surface) at a surface point. N: shading normal, V: direction
// towards the viewer (used for the view bias). Trilinear over the 8 surrounding probes with backface and Chebyshev
// visibility weights (Majercik et al. 2019).
vec3 oxDdgiIrradiance(DdgiVolume v, vec3 P, vec3 N, vec3 V) {
    if (v.enabled == 0u) return vec3(0.0);
    vec3 biased = P + N * v.normalBias + V * v.viewBias;
    vec3 grid = biased / v.spacing - 0.5;
    ivec3 base = ivec3(floor(grid));
    vec3 alpha = clamp(grid - vec3(base), 0.0, 1.0);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < 8u; ++i) {
        ivec3 off = ivec3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
        ivec3 c = base + off;
        if (any(lessThan(c, v.minCoord)) || any(greaterThanEqual(c, v.minCoord + v.counts))) continue;
        uint s = oxDdgiStorageIndex(v, c);
        vec3 probePos = oxDdgiProbePosition(v, c);
        vec3 toProbe = probePos - P;
        float dist = length(toProbe);
        vec3 dirToProbe = dist > 1e-4 ? toProbe / dist : N;
        vec3 tri = mix(1.0 - alpha, alpha, vec3(off));
        float w = tri.x * tri.y * tri.z;
        // Smooth backface: (dot + 1) / 2 squared, + 0.2.
        float bf = (dot(dirToProbe, N) + 1.0) * 0.5;
        w *= bf * bf + 0.2;
        // Chebyshev visibility from the depth moments seen from the probe.
        vec3 probeToPoint = biased - probePos;
        float d = length(probeToPoint);
        vec2 m = textureLod(sampler2D(OX_TEX2D(v.depthTexture), OX_SAMPLER(OX_SAMPLER_LINEAR_CLAMP)),
                            oxDdgiAtlasUv(v, s, probeToPoint / max(d, 1e-4), OX_DDGI_DEPTH_TEXELS), 0.0).rg;
        float cheb = 1.0;
        if (d > m.x) {
            float variance = abs(m.y - m.x * m.x);
            float delta = d - m.x;
            cheb = variance / (variance + delta * delta);
            cheb = max(cheb * cheb * cheb, 0.0);
        }
        w *= max(cheb, 0.05);
        // Crush tiny weights (light leaking).
        const float crush = 0.2;
        if (w < crush) w *= w * w / (crush * crush);
        vec3 irr = textureLod(sampler2D(OX_TEX2D(v.irradianceTexture), OX_SAMPLER(OX_SAMPLER_LINEAR_CLAMP)),
                              oxDdgiAtlasUv(v, s, N, OX_DDGI_IRRADIANCE_TEXELS), 0.0).rgb;
        // Irradiance is blended in a perceptual (gamma 5) space for stability; undo it here.
        irr = pow(max(irr, vec3(0.0)), vec3(2.5));
        sum += irr * w;
        wsum += w;
    }
    vec3 r = wsum > 1e-5 ? sum / wsum : vec3(0.0);
    return r * r;
}

#endif
