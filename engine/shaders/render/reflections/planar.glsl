// OxwaldEngine render: planar reflections contract (mirrors ox::render::GpuPlanarReflections).
//
// VIEW.planarReflections (0 = none this frame) lists the reflectors rendered for this view. Each texture is screen
// aligned with the view: it was rendered from the mirrored camera with the view's own projection (oblique near plane
// on the reflector), so a point lying on the reflector plane is sampled at its own render-resolution screen uv,
// offset by the normal deviation for bumpy mirrors / water. Texels hold radiance × preExposure (RGBA16F).
//
// Consumers (water, mirrors): the primary reflector's texture is also the graph resource "PlanarReflection"; declare
// `.read(R.texture("PlanarReflection"), Access::SampledFragment)` on the pass so it is ordered after the render.
//
//   OxPlanarReflection r;
//   if (oxFindPlanarReflection(pc.view, worldPos, 0.5, r)) {              // tolerance 0.5 m: waves
//       vec3 radiance = oxSamplePlanarReflection(pc.view, r, uv, N).rgb; // not pre-exposed
//   }
#ifndef OX_REFLECTIONS_PLANAR_GLSL
#define OX_REFLECTIONS_PLANAR_GLSL

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include <render/common/view.glsl>

struct OxPlanarReflection {
    vec4 plane;  // world plane (n, d): dot(n, p) + d = 0, n faces the viewer
    vec4 axisX;  // xyz tangent, w half size (0 = unbounded)
    vec4 axisZ;  // xyz bitangent, w half size (0 = unbounded)
    vec4 origin; // xyz centre, w intensity
    vec4 params; // x distortion, y max roughness, z texture/render resolution ratio, w unused
    uint texture;
    uint pad0;
    uint pad1;
    uint pad2;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer OxPlanarReflectionBuffer {
    uint count;
    uint pad0;
    uint pad1;
    uint pad2;
    OxPlanarReflection r[4];
};

uint oxPlanarReflectionCount(ViewBuffer vb) {
    OxPlanarReflectionBuffer b = vb.v.planarReflections;
    return uint64_t(b) == 0ul ? 0u : b.count;
}

// 1 inside the reflector rectangle (and on the plane within `planeTolerance` metres), fading at the border.
float oxPlanarReflectorCoverage(OxPlanarReflection r, vec3 worldPos, float planeTolerance) {
    float dist = dot(r.plane.xyz, worldPos) + r.plane.w;
    if (abs(dist) > planeTolerance) return 0.0;
    vec3 rel = worldPos - r.origin.xyz;
    float w = 1.0;
    if (r.axisX.w > 0.0) w *= clamp((r.axisX.w - abs(dot(rel, r.axisX.xyz))) * 20.0, 0.0, 1.0);
    if (r.axisZ.w > 0.0) w *= clamp((r.axisZ.w - abs(dot(rel, r.axisZ.xyz))) * 20.0, 0.0, 1.0);
    return w;
}

bool oxFindPlanarReflection(ViewBuffer vb, vec3 worldPos, float planeTolerance, out OxPlanarReflection outR) {
    OxPlanarReflectionBuffer b = vb.v.planarReflections;
    if (uint64_t(b) == 0ul) return false;
    uint n = min(b.count, 4u);
    for (uint i = 0u; i < n; ++i) {
        OxPlanarReflection r = b.r[i];
        if (oxPlanarReflectorCoverage(r, worldPos, planeTolerance) > 0.0) {
            outR = r;
            return true;
        }
    }
    return false;
}

// uv: render-resolution screen uv of the shaded point. N: shading normal (world). Returns radiance (not pre-exposed)
// × reflector intensity; the alpha channel is the screen-edge fade of the distorted lookup.
vec4 oxSamplePlanarReflection(ViewBuffer vb, OxPlanarReflection r, vec2 uv, vec3 N) {
    vec3 dn = N - r.plane.xyz;
    vec3 right = vb.v.invView[0].xyz;
    vec3 up = vb.v.invView[1].xyz;
    vec2 suv = uv + vec2(dot(dn, right), -dot(dn, up)) * r.params.x;
    vec2 edge = min(suv, 1.0 - suv);
    float fade = clamp(min(edge.x, edge.y) * 20.0, 0.0, 1.0);
    suv = clamp(suv, vec2(0.0), vec2(1.0));
    vec3 c = OX_SAMPLE_2D_LOD(r.texture, OX_SAMPLER_LINEAR_CLAMP, suv, 0.0).rgb;
    return vec4(c / max(vb.v.preExposure, 1e-30) * r.origin.w, fade);
}

#endif
