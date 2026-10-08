#version 460
// Terrain tessellation evaluation: barycentric position + detail displacement from the dominant splat layer
// (TerrainRenderComponent::tessellationHeight), faded out towards the tessellation range.
#include "terrain_common.glsl"

layout(triangles, equal_spacing, ccw) in;
OX_RENDER_PUSH(TerrainParamsBuffer params; TerrainPatchBuffer patches; TerrainGridBuffer grid; OxWorldMatrixBuffer matrices; uint inputs[4];);

layout(location = 0) in vec3 tcWorldPos[];
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec4 vCurClip;
layout(location = 2) out vec4 vPrevClip;

invariant gl_Position;

void main() {
    precise vec3 p = tcWorldPos[0] * gl_TessCoord.x + tcWorldPos[1] * gl_TessCoord.y + tcWorldPos[2] * gl_TessCoord.z;
    vec4 t = pc.params.p.tess;
    float d = distance(pc.params.p.morphCamera.xyz, p);
    float fade = 1.0 - smoothstep(t.x * 0.4, t.x, d);
    if (fade > 0.0) p.y += (oxTerrainDetailHeight(pc.params, pc.scene, p.xz, 1.0) - 0.5) * t.z * fade;
    vWorldPos = p;
    vCurClip = VIEW.unjitteredViewProj * vec4(p, 1.0);
    vPrevClip = VIEW.prevUnjitteredViewProj * vec4(p, 1.0);
    gl_Position = VIEW.viewProj * vec4(p, 1.0);
}
