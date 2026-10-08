#version 460
// Depth prepass: Normals (world normal + roughness), Velocity (uv motion), EntityID (editor), alpha test.
#include <render/common/material.glsl>

OX_RENDER_DRAW_PUSH(uint inputs[6];);

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv0;
layout(location = 4) in vec4 vColor;
layout(location = 5) in vec4 vCurClip;
layout(location = 6) in vec4 vPrevClip;
layout(location = 7) flat in uint vInstance;

layout(location = 0) out vec4 outNormals;
layout(location = 1) out vec2 outVelocity;
#ifdef OX_ENTITY_ID
layout(location = 2) out uint outEntity;
#endif

void main() {
    InstanceBuffer instances = SCENE.instances;
    uint materialIndex = instances.i[vInstance].materialIndex;
    Material m = SCENE.materials.m[materialIndex];
#ifdef OX_ALPHA_TEST
    if (oxMaterialAlpha(m, vUv0, vColor, VIEW.mipBias) < m.alphaCutoff) discard;
#endif
    OxMaterialSample ms = oxSampleMaterial(m, vUv0, vColor, vNormal, vTangent, gl_FrontFacing, VIEW.mipBias);
    outNormals = vec4(ms.normal, clamp(ms.perceptualRoughness, 0.0, 1.0));
    vec2 cur = vCurClip.xy / vCurClip.w;
    vec2 prev = vPrevClip.xy / vPrevClip.w;
    outVelocity = (cur - prev) * 0.5;
#ifdef OX_ENTITY_ID
    outEntity = instances.i[vInstance].entityId;
#endif
}
