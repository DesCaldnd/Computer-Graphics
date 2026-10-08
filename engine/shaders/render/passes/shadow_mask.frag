#version 460
// ShadowMask: screen-space sun visibility from the cascaded shadow maps (render resolution, R8).
#include <render/common/shadows.glsl>

OX_RENDER_PUSH(uint depth; uint normals;);

layout(location = 0) in vec2 uv;
layout(location = 0) out float outMask;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = OX_FETCH_2D(pc.depth, p, 0).r;
    if (d <= 0.0) { outMask = 1.0; return; }
    vec3 N = OX_FETCH_2D(pc.normals, p, 0).xyz;
    vec2 suv = gl_FragCoord.xy * VIEW.renderSize.zw;
    vec3 worldPos = oxWorldPositionFromDepth(pc.view, suv, d);
    float viewDepth = oxLinearDepth(pc.view, d);
    outMask = oxSunShadow(pc.view, worldPos, normalize(N), viewDepth, gl_FragCoord.xy);
}
