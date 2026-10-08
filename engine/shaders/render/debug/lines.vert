#version 460
// core DebugDraw lines (ox::DebugVertex: vec3 position + RGBA8 colour), vertex pulling.
#include <render/common/view.glsl>
struct DebugVertex { vec3 position; uint color; };
OX_READONLY_BUFFER(DebugVertices, { DebugVertex v[]; });
OX_PUSH_CONSTANTS({ ViewBuffer view; DebugVertices vertices; uint depth; uint depthTest; vec2 depthScale; });
layout(location = 0) out vec4 vColor;
void main() {
    DebugVertex dv = pc.vertices.v[gl_VertexIndex];
    uint c = dv.color;
    vColor = vec4(float(c & 0xffu), float((c >> 8) & 0xffu), float((c >> 16) & 0xffu), float(c >> 24)) / 255.0;
    gl_Position = pc.view.v.unjitteredViewProj * vec4(dv.position, 1.0);
}
