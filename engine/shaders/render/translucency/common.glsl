// Shared helpers of translucent shading: view depth, refraction source sampling, Fresnel, Beer-Lambert.
#ifndef OX_TRANSLUCENCY_COMMON_GLSL
#define OX_TRANSLUCENCY_COMMON_GLSL

#include <render/common/lighting.glsl>
#include <render/common/material.glsl>
#include "fog.glsl"

// Positive view-space distance of a world position (orthographic aware, matches passes/forward.frag).
float oxTranslucencyViewDepth(ViewBuffer vb, vec3 worldPos) {
    vec3 camPos = vb.v.cameraPosition.xyz;
    if ((vb.v.flags & OX_VIEW_ORTHOGRAPHIC) != 0u) return dot(worldPos - camPos, -vb.v.invView[2].xyz);
    return -(vb.v.view * vec4(worldPos, 1.0)).z;
}

vec3 oxTranslucencyViewVector(ViewBuffer vb, vec3 worldPos) {
    if ((vb.v.flags & OX_VIEW_ORTHOGRAPHIC) != 0u) return normalize(vb.v.invView[2].xyz);
    return normalize(vb.v.cameraPosition.xyz - worldPos);
}

// Linear depth of the opaque scene (SceneDepthCopy, R32F reversed-Z device depth) at a render-resolution uv.
float oxSceneLinearDepth(ViewBuffer vb, uint depthCopy, vec2 uv) {
    float d = OX_SAMPLE_2D_LOD(depthCopy, OX_SAMPLER_NEAREST_CLAMP, uv, 0.0).r;
    return d <= 0.0 ? 1e6 : oxLinearDepth(vb, d);
}

// SceneColorRefraction: pre-exposed radiance, mip 0 = sharp copy, higher mips = Gaussian pyramid.
vec3 oxSampleRefraction(uint tex, uint mips, vec2 uv, float perceptualRoughness) {
    float lod = clamp(perceptualRoughness, 0.0, 1.0) * float(max(mips, 1u) - 1u);
    return OX_SAMPLE_2D_LOD(tex, OX_SAMPLER_LINEAR_CLAMP, clamp(uv, vec2(0.0), vec2(1.0)), lod).rgb;
}

float oxF0FromIor(float ior) {
    float r = (ior - 1.0) / (ior + 1.0);
    return r * r;
}

float oxSchlick(float cosTheta, float f0) { return f0 + (1.0 - f0) * oxPow5(1.0 - clamp(cosTheta, 0.0, 1.0)); }

// Beer-Lambert transmittance. absorptionColor = colour reached after `absorptionDistance` metres.
vec3 oxBeerLambert(vec3 absorptionColor, float absorptionDistance, float distance) {
    if (absorptionDistance <= 0.0) return vec3(1.0);
    return pow(max(absorptionColor, vec3(1e-4)), vec3(max(distance, 0.0) / absorptionDistance));
}

// Weighted Blended OIT weight (McGuire & Bavoil 2013, eq. 10) from the positive view depth; fp16 safe.
float oxOitWeight(float alpha, float viewDepth) {
    float z = viewDepth / 200.0;
    return alpha * clamp(0.03 / (1e-5 + z * z * z * z), 1e-2, 3e3);
}

#endif
