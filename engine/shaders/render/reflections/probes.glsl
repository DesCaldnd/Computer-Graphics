// OxwaldEngine render: local reflection probes (mirrors ox::render::GpuReflectionProbe) and their clustered lists.
#ifndef OX_REFLECTIONS_PROBES_GLSL
#define OX_REFLECTIONS_PROBES_GLSL

#include <render/common/view.glsl>

struct OxReflectionProbe {
    mat4 worldToLocal;   // rigid world → box space (box centred at the origin)
    vec3 extents;        // half size
    float blendDistance;
    vec3 capturePosition;
    float intensity;
    vec4 boundingSphere;
    uint cube;           // prefiltered cube (GGX per mip)
    uint mips;
    uint flags;          // bit 0 box projection
    uint pad;
};

OX_READONLY_BUFFER(OxReflectionProbeBuffer, { OxReflectionProbe p[]; });
// counts[clusterCount] followed by indices[clusterCount × OX_MAX_PROBES_PER_CLUSTER], priority order.
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer OxProbeClusters { uint data[]; };

const uint OX_MAX_PROBES_PER_CLUSTER = 16u;
const uint OX_PROBE_BOX_PROJECTION = 1u;

// Influence weight: 1 inside the box (surfaces lying on the box faces, e.g. a room's floor, are fully covered),
// fading to 0 at blendDistance outside it.
float oxProbeInfluence(OxReflectionProbe p, vec3 worldPos) {
    vec3 local = (p.worldToLocal * vec4(worldPos, 1.0)).xyz;
    vec3 outside = max(abs(local) - p.extents, vec3(0.0));
    float d = length(outside);
    return clamp(1.0 - d / max(p.blendDistance, 1e-3), 0.0, 1.0);
}

// Lookup direction for reflection vector R at worldPos (world space). With box projection the ray is intersected
// with the probe box and the direction from the capture point to the hit is used (parallax correction).
vec3 oxProbeDirection(OxReflectionProbe p, vec3 worldPos, vec3 R) {
    if ((p.flags & OX_PROBE_BOX_PROJECTION) == 0u) return R;
    mat3 rot = mat3(p.worldToLocal);
    vec3 lp = (p.worldToLocal * vec4(worldPos, 1.0)).xyz;
    vec3 lr = rot * R;
    vec3 safe = mix(vec3(1e-6), lr, greaterThan(abs(lr), vec3(1e-6)));
    vec3 t1 = (p.extents - lp) / safe;
    vec3 t2 = (-p.extents - lp) / safe;
    vec3 tmax = max(t1, t2);
    float t = min(tmax.x, min(tmax.y, tmax.z));
    t = max(t, 0.0);
    vec3 hitWorld = worldPos + R * t;
    return hitWorld - p.capturePosition;
}

vec3 oxSampleProbe(OxReflectionProbe p, vec3 dir, float perceptualRoughness) {
    float lod = perceptualRoughness * float(max(p.mips, 1u) - 1u);
    return textureLod(samplerCube(OX_TEXCUBE(p.cube), OX_SAMPLER(OX_SAMPLER_LINEAR_CLAMP)), dir, lod).rgb * p.intensity;
}

#endif
