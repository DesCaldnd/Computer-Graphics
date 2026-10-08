#version 460
// Selection outline: edge detection on SelectionMask, blended over SceneColorLDR (output resolution).
#include <common/bindless.glsl>
OX_PUSH_CONSTANTS({ vec4 color; vec2 texel; float thickness; uint mask; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    float center = OX_SAMPLE_2D_LOD(pc.mask, OX_SAMPLER_LINEAR_CLAMP, uv, 0.0).r;
    float m = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            vec2 o = vec2(x, y) * pc.texel * pc.thickness * 0.5;
            m = max(m, OX_SAMPLE_2D_LOD(pc.mask, OX_SAMPLER_LINEAR_CLAMP, uv + o, 0.0).r);
        }
    float edge = clamp(m - center, 0.0, 1.0);
    float a = edge + center * 0.08;
    if (a <= 0.002) discard;
    outColor = vec4(pc.color.rgb, clamp(a, 0.0, 1.0) * pc.color.a);
}
