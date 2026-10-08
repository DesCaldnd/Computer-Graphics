#version 460
// World sky background: fullscreen at depth 0 with a read-only GREATER_OR_EQUAL test (only empty pixels).
#include "sky_common.glsl"

OX_RENDER_PUSH(WorldSkyBuffer sky; float pixelAngle; uint unused;);

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 dir = oxViewRay(pc.view, gl_FragCoord.xy * VIEW.renderSize.zw);
    float maxRadiance = 30000.0 / max(VIEW.preExposure, 1e-12);
    vec3 c = oxWorldSkyRadiance(pc.sky, dir, pc.pixelAngle, maxRadiance) * VIEW.skyIntensity;
    if (VIEW.debugView != 0u && VIEW.debugView != 7u && VIEW.debugView != 9u) c = vec3(0.0);
    outColor = vec4(c * VIEW.preExposure, 1.0);
}
