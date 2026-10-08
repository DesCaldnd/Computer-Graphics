// OxwaldEngine ray tracing: ReSTIR DI reservoirs (Bitterli et al. 2020) for many-light shadows.
//
// The reservoirs resample shadow-casting local lights proportionally to their *unshadowed* contribution
// p̂ = luminance(BRDF · Le · attenuation) at the pixel. The visibility V(y) of the selected light then estimates the
// ratio Σ f·V / Σ f of shadowed to unshadowed local lighting (both RIS estimators share the sample), which the
// forward pass applies to every shadowed local light through a GpuShadow kind 2 channel. Unbiased for a single light,
// consistent (and denoised) for many.
#ifndef OX_RT_RESTIR_COMMON_GLSL
#define OX_RT_RESTIR_COMMON_GLSL

#include "rt_common.glsl"

struct Reservoir {
    uint light;   // light buffer index of the selected sample (OX_INVALID_INDEX = none)
    uint entity;  // its entity id (validates temporal reuse across light list changes)
    float wSum;
    float M;
    float W;      // unbiased contribution weight: wSum / (M · p̂(y))
    float z;      // linear depth of the pixel (reuse validation)
    vec2 normal;  // octahedral normal of the pixel
};
OX_BUFFER(ReservoirBuffer, { Reservoir r[]; });
OX_READONLY_BUFFER(LightIndexBuffer, { uint i[]; });

Reservoir oxReservoirEmpty() {
    Reservoir r;
    r.light = OX_INVALID_INDEX;
    r.entity = 0u;
    r.wSum = 0.0;
    r.M = 0.0;
    r.W = 0.0;
    r.z = 0.0;
    r.normal = vec2(0.5);
    return r;
}

bool oxReservoirUpdate(inout Reservoir r, uint light, uint entity, float w, float count, inout uint rng) {
    r.wSum += w;
    r.M += count;
    if (w > 0.0 && oxRtRandom(rng) * r.wSum <= w) {
        r.light = light;
        r.entity = entity;
        return true;
    }
    return false;
}

// Unshadowed target function of a local light at a surface (luminance of BRDF × radiance × attenuation × NdotL).
float oxRestirTarget(SceneBuffer sb, OxSurface s, uint lightIndex) {
    if (lightIndex == OX_INVALID_INDEX) return 0.0;
    Light l = sb.s.lights.l[lightIndex];
    vec3 toL = l.position - s.position;
    float d2 = dot(toL, toL);
    if (d2 > l.range * l.range) return 0.0;
    vec3 L = toL * inversesqrt(max(d2, 1e-8));
    float att = oxDistanceAttenuation(d2, l.range);
    if (l.type == OX_LIGHT_SPOT) att *= oxSpotAttenuation(L, l.direction, l.spotScale, l.spotOffset);
    if (att <= 0.0) return 0.0;
    return oxLuminance(oxBrdfDirect(s, L, vec3(1.0)) * l.color * att);
}

// G-buffer surface at a pixel (view vector towards the camera).
bool oxRestirSurface(ViewBuffer vb, uint depthTex, uint normalTex, ivec2 gp, out OxSurface s, out float z) {
    float d = OX_FETCH_2D(depthTex, gp, 0).r;
    vec4 nr = OX_FETCH_2D(normalTex, gp, 0);
    z = 0.0;
    if (d <= 0.0) return false;
    vec2 uv = (vec2(gp) + 0.5) * vb.v.renderSize.zw;
    s.position = oxWorldPositionFromDepth(vb, uv, d);
    s.normal = normalize(nr.xyz);
    s.geometricNormal = s.normal;
    s.view = normalize(vb.v.cameraPosition.xyz - s.position);
    // Material colours are unknown here: a grey dielectric keeps the target function smooth (it only steers sampling).
    s.baseColor = vec3(0.5);
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.perceptualRoughness = max(nr.w, 0.3);
    s.occlusion = 1.0;
    s.emissive = vec3(0.0);
    oxSurfaceFinalize(s);
    z = oxLinearDepth(vb, d);
    return true;
}

#endif
