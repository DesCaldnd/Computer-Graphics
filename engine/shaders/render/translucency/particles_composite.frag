#version 460
// Depth-aware (bilateral) upsampling of the low-resolution particle buffer: the four low-res texels around the pixel
// are weighted bilinearly × depth similarity to the full-resolution scene depth, so particles do not bleed over
// foreground edges. Output premultiplied; blend ONE, ONE_MINUS_SRC_ALPHA into SceneColorHDR.
#include <render/common/view.glsl>

layout(push_constant, scalar) uniform OxPushConstants {
    ViewBuffer view;
    uint particles;
    uint lowDepth;
    uint fullDepth;
    uint pad;
    vec2 lowSize;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 suv = gl_FragCoord.xy * pc.view.v.renderSize.zw;
    float d = OX_FETCH_2D(pc.fullDepth, ivec2(gl_FragCoord.xy), 0).r;
    float lin = d > 0.0 ? oxLinearDepth(pc.view, d) : 1e6;
    vec2 pos = suv * pc.lowSize - 0.5;
    ivec2 base = ivec2(floor(pos));
    vec2 f = pos - vec2(base);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    vec4 nearest = vec4(0.0);
    float best = 1e30;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            ivec2 q = clamp(base + ivec2(x, y), ivec2(0), ivec2(pc.lowSize) - 1);
            float ld = OX_FETCH_2D(pc.lowDepth, q, 0).r;
            float llin = ld > 0.0 ? oxLinearDepth(pc.view, ld) : 1e6;
            float bw = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
            float rel = abs(llin - lin) / max(lin, 1e-3);
            float w = bw / (1e-3 + rel * 50.0);
            vec4 c = OX_FETCH_2D(pc.particles, q, 0);
            sum += c * w;
            wsum += w;
            if (rel < best) {
                best = rel;
                nearest = c;
            }
        }
    }
    vec4 c = wsum > 1e-6 ? sum / wsum : nearest;
    if (c.a <= 0.0 && dot(c.rgb, c.rgb) <= 0.0) discard;
    outColor = c;
}
