#version 460
// CDLOD terrain vertex shader (depth prepass, forward, shadow cascades with OX_TERRAIN_SHADOW). Instances are
// TerrainPatchGpu, vertices the shared grid mesh (vertex pulling, index buffer = grid indices).
#include "terrain_common.glsl"

OX_RENDER_PUSH(TerrainParamsBuffer params; TerrainPatchBuffer patches; TerrainGridBuffer grid; OxWorldMatrixBuffer matrices; uint inputs[4];);

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec4 vCurClip;
layout(location = 2) out vec4 vPrevClip;

invariant gl_Position;

void main() {
    TerrainPatch inst = pc.patches.p[gl_InstanceIndex];
    TerrainGridVertex g = pc.grid.v[gl_VertexIndex];
    vec3 wp = oxTerrainVertex(pc.params, inst, g);
    vWorldPos = wp;
#ifdef OX_TERRAIN_SHADOW
    vCurClip = vec4(0.0);
    vPrevClip = vec4(0.0);
    gl_Position = pc.matrices.m[0] * vec4(wp, 1.0);
#else
    vCurClip = VIEW.unjitteredViewProj * vec4(wp, 1.0);
    vPrevClip = VIEW.prevUnjitteredViewProj * vec4(wp, 1.0);
    gl_Position = VIEW.viewProj * vec4(wp, 1.0);
#endif
}
