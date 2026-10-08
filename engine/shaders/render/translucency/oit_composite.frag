#version 460
// Weighted Blended OIT resolve: SceneColorHDR = average colour × (1 - revealage) + background × revealage
// (blend SRC_ALPHA / ONE_MINUS_SRC_ALPHA).
#include <common/bindless.glsl>

OX_PUSH_CONSTANTS({ uint accum; uint reveal; });

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float reveal = OX_FETCH_2D(pc.reveal, p, 0).r;
    if (reveal >= 0.9999) discard;
    vec4 accum = OX_FETCH_2D(pc.accum, p, 0);
    vec3 average = accum.rgb / clamp(accum.a, 1e-5, 5e4);
    outColor = vec4(average, 1.0 - reveal);
}
