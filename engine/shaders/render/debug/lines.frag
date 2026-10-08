#version 460
#include <render/common/view.glsl>
struct DebugVertex { vec3 position; uint color; };
OX_READONLY_BUFFER(DebugVertices, { DebugVertex v[]; });
OX_PUSH_CONSTANTS({ ViewBuffer view; DebugVertices vertices; uint depth; uint depthTest; vec2 depthScale; });
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() {
    if (pc.depthTest != 0u) {
        // Manual depth test against the render-resolution depth (lines are drawn at output resolution).
        float scene = OX_FETCH_2D(pc.depth, ivec2(gl_FragCoord.xy * pc.depthScale), 0).r;
        if (scene > gl_FragCoord.z * 1.001 + 1e-6) discard;
    }
    outColor = vColor;
}
