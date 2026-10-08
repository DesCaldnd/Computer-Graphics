#version 460
// Terrain depth prepass: Normals (heightmap normal, roughness), Velocity, EntityID (editor); holes discard.
// OX_TERRAIN_SHADOW: shadow cascades (holes only).
#include "terrain_common.glsl"

OX_RENDER_PUSH(TerrainParamsBuffer params; TerrainPatchBuffer patches; TerrainGridBuffer grid; OxWorldMatrixBuffer matrices; uint inputs[4];);

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec4 vCurClip;
layout(location = 2) in vec4 vPrevClip;

#ifndef OX_TERRAIN_SHADOW
layout(location = 0) out vec4 outNormals;
layout(location = 1) out vec2 outVelocity;
#ifdef OX_ENTITY_ID
layout(location = 2) out uint outEntity;
#endif
#endif

void main() {
    if (oxTerrainIsHole(pc.params, vWorldPos.xz)) discard;
#ifndef OX_TERRAIN_SHADOW
    outNormals = vec4(oxTerrainMacroNormal(pc.params, vWorldPos.xz), 0.85);
    outVelocity = oxWorldVelocity(vCurClip, vPrevClip);
#ifdef OX_ENTITY_ID
    outEntity = pc.params.p.entityId;
#endif
#endif
}
