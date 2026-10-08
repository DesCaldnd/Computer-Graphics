#version 460
// Shadow depth vertex shader. OX_LAYERED: one instanced draw renders all 6 point light faces
// (gl_InstanceIndex = instance × 6 + face, gl_Layer = layerBase + face; needs shaderOutputLayer).
#ifdef OX_LAYERED
#extension GL_ARB_shader_viewport_layer_array : require
#endif
#include <render/common/scene.glsl>

OX_READONLY_BUFFER(MatrixBuffer, { mat4 m[]; });
OX_RENDER_DRAW_PUSH(MatrixBuffer matrices; uint layerBase; uint pad;);

layout(location = 0) out vec2 vUv0;
layout(location = 1) flat out uint vInstance;
layout(location = 2) out float vAlpha;

void main() {
#ifdef OX_LAYERED
    uint face = uint(gl_InstanceIndex) % 6u;
    uint instanceId = pc.drawIds.id[uint(gl_InstanceIndex) / 6u];
    gl_Layer = int(pc.layerBase + face);
#else
    uint face = 0u;
    uint instanceId = pc.drawIds.id[gl_InstanceIndex];
#endif
    OxVertex v = oxFetchVertex(pc.scene, instanceId, uint(gl_VertexIndex));
    vUv0 = v.uv0;
    vInstance = instanceId;
    vAlpha = v.color.a;
    gl_Position = pc.matrices.m[face] * vec4(v.position, 1.0);
}
