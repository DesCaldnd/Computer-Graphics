#version 460
// Vertex shader of the meshlet path (r.GpuDriven.Meshlets): the compacted index buffer written by meshlet_cull.comp
// holds (visible meshlet slot << 7 | local vertex); pc.drawIds points at the visible meshlet table
// (uvec2: instance, u32 offset of the meshlet's vertex list). Same outputs and math as passes/mesh.vert, so the
// forward pass can depth-test EQUAL against the prepass.
#include <render/common/scene.glsl>

OX_RENDER_DRAW_PUSH(uint inputs[6];);

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUv0;
layout(location = 4) out vec4 vColor;
layout(location = 5) out vec4 vCurClip;
layout(location = 6) out vec4 vPrevClip;
layout(location = 7) flat out uint vInstance;

invariant gl_Position;

void main() {
    uint packed = uint(gl_VertexIndex);
    uint slot = packed >> 7;
    uint local = packed & 127u;
    uint instanceId = pc.drawIds.id[slot * 2u];
    uint vertexList = pc.drawIds.id[slot * 2u + 1u];
    uint meshIndex = SCENE.instances.i[instanceId].meshIndex;
    uint vertexIndex = uint(SCENE.meshes.m[meshIndex].vertexOffset) + SCENE.meshlets.data[vertexList + local];
    OxVertex v = oxFetchVertex(pc.scene, instanceId, vertexIndex);
    vWorldPos = v.position;
    vNormal = v.normal;
    vTangent = v.tangent;
    vUv0 = v.uv0;
    vColor = v.color;
    vInstance = instanceId;
    vCurClip = VIEW.unjitteredViewProj * vec4(v.position, 1.0);
    vPrevClip = VIEW.prevUnjitteredViewProj * vec4(v.prevPosition, 1.0);
    gl_Position = VIEW.viewProj * vec4(v.position, 1.0);
}
