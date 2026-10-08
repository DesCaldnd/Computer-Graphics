#version 460
// Impostor bake: albedo + coverage (RGBA8 sRGB) and object-space normal (RGBA8) of one atlas frame.
#include <render/common/material.glsl>

layout(push_constant, scalar) uniform OxPushConstants {
    SceneBuffer scene;
    mat4 viewProj;
    uint material;
    uint pad0;
} pc;

layout(location = 0) in vec2 vUv0;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vColor;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;

void main() {
    Material m = SCENE.materials.m[pc.material];
    vec4 vc = vec4(1.0, 1.0, 1.0, vColor.a); // vertex colour rgb holds wind masks, alpha is opacity
    float alpha = oxMaterialAlpha(m, vUv0, vc, 0.0);
    if (oxMaterialBlend(m) == OX_BLEND_ALPHA_TEST && alpha < m.alphaCutoff) discard;
    OxMaterialSample ms = oxSampleMaterial(m, vUv0, vc, vNormal, vec4(0.0), gl_FrontFacing, 0.0);
    vec3 n = normalize(vNormal); // canopy-outward normals, not flipped on back faces (see oxFoliageNormal)
    outAlbedo = vec4(ms.baseColor.rgb, 1.0);
    outNormal = vec4(n * 0.5 + 0.5, 1.0);
}
