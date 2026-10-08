#version 460
// Vertex pulling for transparent / refractive meshes and their back-face depth prepass.
#include <render/common/scene.glsl>
#include "translucent_push.glsl"

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUv0;
layout(location = 4) out vec4 vColor;
layout(location = 5) flat out uint vInstance;

void main() {
    uint instanceId = pc.drawIds.id[gl_InstanceIndex];
    OxVertex v = oxFetchVertex(pc.scene, instanceId, uint(gl_VertexIndex));
    vWorldPos = v.position;
    vNormal = v.normal;
    vTangent = v.tangent;
    vUv0 = v.uv0;
    vColor = v.color;
    vInstance = instanceId;
    gl_Position = VIEW.viewProj * vec4(v.position, 1.0);
}
