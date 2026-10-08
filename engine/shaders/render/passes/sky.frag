#version 460
// Sky: fullscreen at depth 0 with a read-only depth test (reversed-Z GREATER_OR_EQUAL) → only background pixels.
#include <render/common/sky.glsl>

OX_RENDER_PUSH(uint unused;);

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 dir = oxViewRay(pc.view, gl_FragCoord.xy * VIEW.renderSize.zw);
    vec3 c = oxSkyRadiance(pc.view, pc.scene, dir, true);
    if (VIEW.debugView != 0u && VIEW.debugView != 7u && VIEW.debugView != 9u) c = vec3(0.0);
    outColor = vec4(c * VIEW.preExposure, 1.0);
}
