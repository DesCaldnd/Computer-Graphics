#version 460
#include "ui_common.glsl"

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main() {
    uint idx = pc.indices.i[pc.firstIndex + uint(gl_VertexIndex)];
    UiVertex v = pc.vertices.v[uint(pc.vertexOffset) + idx];
    vec4 p = vec4(v.pos * pc.xform.xy + pc.xform.zw, 0.0, 1.0);
    if (pc.transform >= 0) p = pc.transforms.m[pc.transform] * p;
    // Frame pixels (origin top-left, y down) -> Vulkan clip space (y down as well).
    gl_Position = vec4(p.xy * pc.ndcScale - p.w, 0.0, p.w);
    vUv = v.uv;
    vColor = unpackUnorm4x8(v.color);
}
