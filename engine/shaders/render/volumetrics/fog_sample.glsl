// OxwaldEngine volumetrics: per-pixel fog for translucent surfaces, water, particles (and the opaque composite).
//
//   #include <render/volumetrics/fog_sample.glsl>
//   // fogTexture = sampled index of the VolumetricFog resource (declare .read(fog, Access::SampledFragment));
//   // OX_INVALID_INDEX when the resource is absent (then only the analytic height fog is applied, if any).
//   vec4 fog = oxEvaluateVolumetricFog(pc.view, pc.scene, fogTexture, uv, worldPos);
//   outColor.rgb = oxApplyVolumetricFog(outColor.rgb, fog);            // pre-exposed radiance
//   // premultiplied-alpha blending (src·1 + dst·(1-a)): rgb = rgb · fog.a + fog.rgb · alpha
//
// Result: rgb = in-scattered radiance between the camera and the surface × VIEW.preExposure, a = transmittance.
// `uv` is the render-resolution uv of the pixel (gl_FragCoord.xy * VIEW.renderSize.zw). Inside the froxel grid the
// VolumetricFog 3D texture is sampled (lights, shadows, local fog volumes); beyond it the exponential height fog of
// the EnvironmentComponent is integrated analytically (unshadowed sun + sky), which is also what the sky gets.
// When the Volumetrics feature is inactive (VIEW.volumetricFogGrid.w == 0) the result is (0, 0, 0, 1) and callers
// may fall back to oxApplyHeightFog (common/lighting.glsl) when VIEW.fogColor.w > 0.5.
#ifndef OX_VOLUMETRICS_FOG_SAMPLE_GLSL
#define OX_VOLUMETRICS_FOG_SAMPLE_GLSL

#include "fog_common.glsl"

// Froxel volume only: integrated fog from the camera to `viewDepth` (positive view-space depth).
vec4 oxSampleVolumetricFog(ViewBuffer vb, uint fogTexture, vec2 uv, float viewDepth) {
    if (fogTexture == OX_INVALID_INDEX || !oxFroxelFogActive(vb)) return vec4(0.0, 0.0, 0.0, 1.0);
    float depthSlices = vb.v.volumetricFogGrid.z;
    // Texel z holds the integral up to the far edge of slice z.
    float s = oxFroxelDepthToSlice(vb, viewDepth) * depthSlices;
    float z = s - 1.0;
    vec4 f = OX_SAMPLE_3D(fogTexture, OX_SAMPLER_LINEAR_CLAMP, vec3(uv, (max(z, 0.0) + 0.5) / depthSlices));
    if (z < 0.0) f = mix(vec4(0.0, 0.0, 0.0, 1.0), f, clamp(s, 0.0, 1.0));
    return f;
}

// Froxel volume + analytic height fog beyond the grid for a view ray (unit `dir`) ending at `rayDistance`
// (= viewDepth × ray scale; sky: VIEW.volumetricFogLighting.w).
vec4 oxEvaluateVolumetricFogRay(ViewBuffer vb, SceneBuffer sb, uint fogTexture, vec2 uv, vec3 dir, float viewDepth,
                                float rayDistance) {
    vec4 near = oxSampleVolumetricFog(vb, fogTexture, uv, viewDepth);
    if (!oxFroxelFogActive(vb)) return near;
    float gridEnd = vb.v.volumetricFogGrid.w * (rayDistance / max(viewDepth, 1e-4));
    vec4 far = oxAnalyticHeightFog(vb, sb, dir, gridEnd, rayDistance);
    return vec4(near.rgb + near.a * far.rgb, near.a * far.a);
}

vec4 oxEvaluateVolumetricFog(ViewBuffer vb, SceneBuffer sb, uint fogTexture, vec2 uv, vec3 worldPos) {
    vec3 cam = vb.v.cameraPosition.xyz;
    vec3 ray = worldPos - cam;
    float dist = length(ray);
    float viewDepth = max(dot(ray, oxFogCameraForward(vb)), 1e-4);
    return oxEvaluateVolumetricFogRay(vb, sb, fogTexture, uv, ray / max(dist, 1e-6), viewDepth, dist);
}

vec3 oxApplyVolumetricFog(vec3 preExposedColor, vec4 fog) { return preExposedColor * fog.a + fog.rgb; }

#endif
