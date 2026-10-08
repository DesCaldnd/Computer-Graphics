#version 460
// Background of scene captures (fullscreen at depth 0, read-only GREATER_OR_EQUAL test): sky radiance ×
// VIEW.preExposure, alpha = "infinite" distance. flags bit 0: draw the sun disk (planar reflections).
#include <render/common/sky.glsl>

OX_RENDER_PUSH(uint flags;);

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    // Ray from the camera through the pixel's point on the ndc z = 1 plane (the near / oblique clip plane, always in
    // front of the camera; oxViewRay's far point can fall behind the camera with an oblique far plane).
    vec2 uvp = gl_FragCoord.xy * VIEW.renderSize.zw;
    vec4 p = VIEW.invViewProj * vec4(uvp * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w - VIEW.cameraPosition.xyz);
    vec3 c = oxSkyRadiance(pc.view, pc.scene, dir, (pc.flags & 1u) != 0u);
    outColor = vec4(min(c * VIEW.preExposure, vec3(60000.0)), 60000.0);
}
