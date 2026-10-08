#version 460
// Default "upscaler": bilinear resample of SceneColorHDR from render to output resolution.
#include <common/bindless.glsl>
OX_PUSH_CONSTANTS({ uint src; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() { outColor = OX_SAMPLE_2D_LOD(pc.src, OX_SAMPLER_LINEAR_CLAMP, uv, 0.0); }
