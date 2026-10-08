#version 460
// SceneColorHDR (pre-exposed) → display-encoded SceneColorLDR: exposure, tonemapper, sRGB OETF, dither.
#include <render/common/color.glsl>
#include <render/common/math.glsl>
#include <render/common/view.glsl>

OX_PUSH_CONSTANTS({ ViewBuffer view; uint hdr; uint tonemapper; uint exposureTex; uint debugBypass; });

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 hdr = OX_SAMPLE_2D_LOD(pc.hdr, OX_SAMPLER_LINEAR_CLAMP, uv, 0.0).rgb;
    float preExposure = max(pc.view.v.preExposure, 1e-12);
    vec3 display;
    if (pc.debugBypass != 0u) {
        display = clamp(hdr / preExposure, 0.0, 1.0);
    } else {
        float exposure = pc.view.v.exposure;
        if (pc.exposureTex != OX_INVALID_INDEX) exposure = OX_FETCH_2D(pc.exposureTex, ivec2(0), 0).r;
        display = oxTonemap(hdr * (exposure / preExposure), pc.tonemapper);
    }
    vec3 encoded = oxLinearToSrgb(display);
    float noise = oxInterleavedGradientNoise(gl_FragCoord.xy) - 0.5;
    outColor = vec4(encoded + noise / 255.0, 1.0);
}
