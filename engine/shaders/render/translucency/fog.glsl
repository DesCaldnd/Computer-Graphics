// Fog for translucent surfaces (transparent, refractive, water, particles). Uses the volumetrics team's
// render/volumetrics/fog_sample.glsl when it exists (the C++ side defines OX_HAS_VOLUMETRIC_FOG_SAMPLE only when the
// file declares oxEvaluateVolumetricFog): froxel volume + analytic height fog beyond the grid. Without it, or while
// the Volumetrics feature is inactive, the exponential height fog of the opaque pass is applied.
#ifndef OX_TRANSLUCENCY_FOG_GLSL
#define OX_TRANSLUCENCY_FOG_GLSL

#include <render/common/lighting.glsl>
#ifdef OX_HAS_VOLUMETRIC_FOG_SAMPLE
#include <render/volumetrics/fog_sample.glsl>
#endif

// In-scattered radiance of the height fog at full density (matches passes/forward.frag).
vec3 oxTranslucencyHeightFogRadiance(ViewBuffer vb) {
    vec4 sh[9];
    OxSHBuffer shb = vb.v.irradianceSH;
    for (int k = 0; k < 9; ++k) sh[k] = shb.c[k];
    return vb.v.fogColor.rgb * oxEvalSH9(sh, vec3(0.0, 1.0, 0.0)) * vb.v.iblIntensity;
}

// color: radiance (not pre-exposed) leaving the surface at worldPos towards the camera. uv: render-resolution uv.
// froxels: sampled index of the VolumetricFog 3D texture (OX_INVALID_INDEX = none). Returns fogged radiance.
vec3 oxTranslucencyFog(ViewBuffer vb, SceneBuffer sb, vec3 color, vec3 worldPos, vec2 uv, uint froxels) {
#ifdef OX_HAS_VOLUMETRIC_FOG_SAMPLE
    if (oxFroxelFogActive(vb)) {
        vec4 f = oxEvaluateVolumetricFog(vb, sb, froxels, uv, worldPos);
        return color * f.a + f.rgb / max(vb.v.preExposure, 1e-30);
    }
#endif
    if (vb.v.fogColor.w > 0.5) return oxApplyHeightFog(vb, color, worldPos, oxTranslucencyHeightFogRadiance(vb));
    return color;
}

// Transmittance-only variant for premultiplied/additive particles: returns (in-scatter × alpha weight, transmittance).
vec4 oxTranslucencyFogFactors(ViewBuffer vb, SceneBuffer sb, vec3 worldPos, vec2 uv, uint froxels) {
#ifdef OX_HAS_VOLUMETRIC_FOG_SAMPLE
    if (oxFroxelFogActive(vb)) {
        vec4 f = oxEvaluateVolumetricFog(vb, sb, froxels, uv, worldPos);
        return vec4(f.rgb / max(vb.v.preExposure, 1e-30), f.a);
    }
#endif
    if (vb.v.fogColor.w > 0.5) {
        vec3 fogRadiance = oxTranslucencyHeightFogRadiance(vb);
        // oxApplyHeightFog(c) = mix(c, fog, k): transmittance 1 - k, in-scatter fog × k.
        vec3 k3 = oxApplyHeightFog(vb, vec3(0.0), worldPos, vec3(1.0));
        float k = k3.x;
        return vec4(fogRadiance * k, 1.0 - k);
    }
    return vec4(0.0, 0.0, 0.0, 1.0);
}

#endif
