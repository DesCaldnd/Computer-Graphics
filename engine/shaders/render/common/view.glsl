// OxwaldEngine render: per-view constants (mirrors ox::render::GpuViewConstants, scalar layout).
//
// Every render push-constant block starts with `ViewBuffer view; SceneBuffer scene;` (16 bytes); use
// OX_RENDER_PUSH(...) from scene.glsl or declare them yourself. Access through the VIEW / SCENE macros.
//
// Conventions: right-handed world, Y up; matrices include the Vulkan Y flip (NDC y down, uv = ndc.xy * 0.5 + 0.5,
// uv (0,0) = top-left); reversed-Z (near = 1, far = 0); SceneColorHDR holds radiance × VIEW.preExposure.
#ifndef OX_RENDER_VIEW_GLSL
#define OX_RENDER_VIEW_GLSL

#include <common/bindless.glsl>

// Extra storage image aliases used by render passes (same binding as bindless.glsl, other formats).
layout(set = 0, binding = 1, r8) uniform image2D oxImages2D_r8[];
layout(set = 0, binding = 1, rg16f) uniform image2D oxImages2D_rg16f[];
layout(set = 0, binding = 1, r16f) uniform image2D oxImages2D_r16f[];

layout(buffer_reference) buffer LightClusters;
// Irradiance SH9 (see oxEvalSH9 in pbr.glsl); always valid (zero when there is no environment).
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer OxSHBuffer { vec4 c[9]; };

struct ViewConstants {
    mat4 view;
    mat4 proj;               // jittered
    mat4 viewProj;           // jittered
    mat4 invView;
    mat4 invProj;
    mat4 invViewProj;
    mat4 unjitteredViewProj;
    mat4 prevUnjitteredViewProj;
    mat4 prevViewProj;
    vec4 cameraPosition;     // xyz, w = time (s)
    vec4 renderSize;         // xy, zw = 1 / xy
    vec4 outputSize;
    vec4 jitter;             // xy current (NDC), zw previous
    float nearPlane;
    float farPlane;          // 0 = infinite
    float preExposure;
    float exposure;
    float ev100;
    float deltaTime;
    uint frameIndex;
    uint debugView;
    uint flags;              // bit 0 orthographic, bit 1 editor
    uint lightClusterCount;
    float lodBias;
    float mipBias;
    uvec4 clusterGrid;       // x, y, z slices, max lights per cluster
    vec4 clusterDepth;       // slice scale, slice bias, near, far
    LightClusters clusters;
    int sunLight;
    uint cascadeCount;
    mat4 cascadeViewProj[4];
    vec4 cascadeSplits;
    vec4 cascadeTexelWorld;
    vec4 cascadeDepthRange;
    vec4 shadowParams;       // x blend fraction, y filter radius (texels), z pcss (0/1), w pcf taps
    uint cascadeTexture;
    uint shadowAtlas;
    uint pointShadows;
    uint shadowAtlasSize;
    float sunBias;
    float sunNormalBias;
    float csmResolution;
    float sunAngularRadius;  // radians (PCSS)
    OxSHBuffer irradianceSH; // 9 × vec4
    uint shPad0;
    uint shPad1;
    uint prefilteredCube;
    uint brdfLut;
    uint skyCube;
    uint prefilteredMips;
    float iblIntensity;
    float skyIntensity;
    uint skyMode;            // 0 procedural, 1 cubemap, 2 Preetham
    uint pad1;
    vec4 fogColor;           // rgb, w = enabled
    vec4 fogParams;          // density, height falloff, start distance
    vec4 preetham[8];
    uint whiteTexture;
    uint blackTexture;
    uint flatNormalTexture;
    uint pad2;
};

OX_READONLY_BUFFER(ViewBuffer, { ViewConstants v; });

const uint OX_VIEW_ORTHOGRAPHIC = 1u;
const uint OX_VIEW_EDITOR = 2u;

// Debug views (ox::render::DebugView).
const uint OX_DEBUG_NONE = 0u;
const uint OX_DEBUG_ALBEDO = 1u;
const uint OX_DEBUG_NORMALS = 2u;
const uint OX_DEBUG_ROUGHNESS = 3u;
const uint OX_DEBUG_METALLIC = 4u;
const uint OX_DEBUG_AO = 5u;
const uint OX_DEBUG_EMISSIVE = 6u;
const uint OX_DEBUG_LIGHT_COMPLEXITY = 7u;
const uint OX_DEBUG_OVERDRAW = 8u;
const uint OX_DEBUG_SHADOW_CASCADES = 9u;
const uint OX_DEBUG_WIREFRAME = 10u;
const uint OX_DEBUG_VELOCITY = 11u;
const uint OX_DEBUG_DEPTH = 12u;
const uint OX_DEBUG_SHADOW_MASK = 13u;

// --- depth / position helpers. They take the 8-byte buffer reference (pc.view), never the struct by value:
// copying ViewConstants out of the buffer would load ~1.7 KB per call.

// Positive view-space distance along the view direction for a reversed-Z device depth.
float oxLinearDepth(ViewBuffer vb, float deviceDepth) {
    if ((vb.v.flags & OX_VIEW_ORTHOGRAPHIC) != 0u) {
        vec4 p = vb.v.invProj * vec4(0.0, 0.0, deviceDepth, 1.0);
        return -p.z / p.w;
    }
    // proj[2][2] * z + proj[3][2] = depth * -z  (works for finite and infinite reversed-Z)
    return vb.v.proj[3][2] / max(deviceDepth + vb.v.proj[2][2], 1e-7);
}

vec3 oxViewPositionFromDepth(ViewBuffer vb, vec2 uv, float deviceDepth) {
    vec4 p = vb.v.invProj * vec4(uv * 2.0 - 1.0, deviceDepth, 1.0);
    return p.xyz / p.w;
}

vec3 oxWorldPositionFromDepth(ViewBuffer vb, vec2 uv, float deviceDepth) {
    vec4 p = vb.v.invViewProj * vec4(uv * 2.0 - 1.0, deviceDepth, 1.0);
    return p.xyz / p.w;
}

// Direction from the camera through a uv (world space, normalised).
vec3 oxViewRay(ViewBuffer vb, vec2 uv) {
    mat4 ivp = vb.v.invViewProj;
    vec4 farP = ivp * vec4(uv * 2.0 - 1.0, 0.0001, 1.0);
    vec4 nearP = ivp * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    return normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);
}

vec2 oxProjectToUv(mat4 viewProj, vec3 worldPos, out float deviceDepth) {
    vec4 c = viewProj * vec4(worldPos, 1.0);
    vec3 ndc = c.xyz / c.w;
    deviceDepth = ndc.z;
    return ndc.xy * 0.5 + 0.5;
}

#endif
