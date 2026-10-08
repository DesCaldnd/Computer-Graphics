#version 460
// Copies display-encoded SceneColorLDR into the view's target. OX_SRGB_TARGET: the target format is *_SRGB, so
// decode first (the hardware re-encodes on store).
#include <render/common/color.glsl>
#include <common/bindless.glsl>
OX_PUSH_CONSTANTS({ uint src; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 c = OX_SAMPLE_2D_LOD(pc.src, OX_SAMPLER_LINEAR_CLAMP, uv, 0.0);
#ifdef OX_SRGB_TARGET
    c.rgb = oxSrgbToLinear(c.rgb);
#endif
    outColor = vec4(c.rgb, 1.0);
}
