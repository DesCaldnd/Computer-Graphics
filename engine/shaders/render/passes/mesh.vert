#version 460
// Shared vertex shader of the depth prepass and the forward passes (identical code → invariant positions, so the
// forward pass can use depth EQUAL against the prepass).
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
    uint instanceId = pc.drawIds.id[gl_InstanceIndex];
    OxVertex v = oxFetchVertex(pc.scene, instanceId, uint(gl_VertexIndex));
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
