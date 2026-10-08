#version 460
// Buffer visualisations written over SceneColorLDR (Debug stage): velocity, depth, shadow mask, overdraw.
#include <render/common/color.glsl>
#include <render/common/view.glsl>
OX_PUSH_CONSTANTS({ ViewBuffer view; uint mode; uint tex; float scale; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 v = OX_SAMPLE_2D_LOD(pc.tex, OX_SAMPLER_NEAREST_CLAMP, uv, 0.0);
    vec3 c = vec3(0.0);
    if (pc.mode == 11u) c = vec3(0.5 + v.xy * pc.scale, 0.5);                      // velocity
    else if (pc.mode == 12u) c = vec3(pow(clamp(v.r, 0.0, 1.0), 0.25));            // depth (reversed-Z)
    else if (pc.mode == 13u) c = vec3(v.r);                                        // shadow mask
    else if (pc.mode == 8u) c = oxHeatmap(v.r / 8.0);                              // overdraw (layers)
    outColor = vec4(oxLinearToSrgb(c), 1.0);
}
