#version 460
// Vegetation depth prepass (and OX_VEG_SHADOW cascades): alpha test, dithered LOD fade, two-sided normals, velocity.
#include "vegetation_common.glsl"
#include <render/common/material.glsl>

OX_RENDER_PUSH(VegInstanceBuffer instances; VegRecordBuffer records; VegFrameBuffer frame; VegProtoBuffer protos; OxWorldMatrixBuffer matrices; uint proto; uint material; uint entityId; uint unused; uint inputs[4];);

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv0;
layout(location = 4) in vec4 vColor;
layout(location = 5) in vec4 vCurClip;
layout(location = 6) in vec4 vPrevClip;
layout(location = 7) flat in uint vFade;

#ifndef OX_VEG_SHADOW
layout(location = 0) out vec4 outNormals;
layout(location = 1) out vec2 outVelocity;
#ifdef OX_ENTITY_ID
layout(location = 2) out uint outEntity;
#endif
#endif

void main() {
#ifndef OX_VEG_SHADOW
    if (!oxWorldDitherKeep(gl_FragCoord.xy, vFade)) discard;
#endif
    Material m = SCENE.materials.m[pc.material];
#ifdef OX_ALPHA_TEST
    if (oxMaterialAlpha(m, vUv0, vColor, VIEW.mipBias) < m.alphaCutoff) discard;
#endif
#ifndef OX_VEG_SHADOW
    vec3 V = (VIEW.flags & OX_VIEW_ORTHOGRAPHIC) != 0u ? normalize(VIEW.invView[2].xyz) : normalize(VIEW.cameraPosition.xyz - vWorldPos);
    vec3 n = oxFoliageNormal(vNormal, normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos))), V, oxSunDirection(pc.view, pc.scene));
    outNormals = vec4(n, clamp(m.roughness, 0.0, 1.0));
    outVelocity = oxWorldVelocity(vCurClip, vPrevClip);
#ifdef OX_ENTITY_ID
    outEntity = pc.entityId;
#endif
#endif
}
