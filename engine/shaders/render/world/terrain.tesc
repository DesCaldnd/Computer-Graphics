#version 460
// Optional terrain tessellation (r.Terrain.Tessellation, DeviceCaps::tessellationShader): LOD-0 patches close to the
// camera are subdivided by distance; factors fall to 1 at the range so edges shared with untessellated patches match.
#include "terrain_common.glsl"

layout(vertices = 3) out;
OX_RENDER_PUSH(TerrainParamsBuffer params; TerrainPatchBuffer patches; TerrainGridBuffer grid; OxWorldMatrixBuffer matrices; uint inputs[4];);

layout(location = 0) in vec3 vWorldPos[];
layout(location = 0) out vec3 tcWorldPos[];

float edgeFactor(vec3 a, vec3 b) {
    vec4 t = pc.params.p.tess;
    float d = distance(pc.params.p.morphCamera.xyz, (a + b) * 0.5);
    float f = mix(t.y, 1.0, smoothstep(t.x * 0.4, t.x, d));
    return clamp(floor(f + 0.5), 1.0, 64.0);
}

void main() {
    tcWorldPos[gl_InvocationID] = vWorldPos[gl_InvocationID];
    if (gl_InvocationID == 0) {
        vec3 p0 = vWorldPos[0], p1 = vWorldPos[1], p2 = vWorldPos[2];
        float e0 = edgeFactor(p1, p2), e1 = edgeFactor(p2, p0), e2 = edgeFactor(p0, p1);
        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelInner[0] = max(e0, max(e1, e2));
    }
}
