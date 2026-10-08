#version 460
// Particle fragment: flipbook sample (two frames blended), soft particles against the scene depth, blend mode →
// premultiplied output (additive = alpha 0), fog. Target blend: ONE, ONE_MINUS_SRC_ALPHA.
#include "particle_push.glsl"
#include <render/common/view.glsl>

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec4 vUv;
layout(location = 2) in vec4 vFog;
layout(location = 3) flat in vec4 vParams;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = vec4(1.0);
    if (EM.texture != OX_INVALID_INDEX) {
        vec4 a = OX_SAMPLE_2D(EM.texture, OX_SAMPLER_LINEAR_CLAMP, vUv.xy);
        vec4 b = OX_SAMPLE_2D(EM.texture, OX_SAMPLER_LINEAR_CLAMP, vUv.zw);
        tex = mix(a, b, vParams.z);
    }
    vec4 c = vColor * tex;
    float fade = 1.0;
    if (pc.depth != OX_INVALID_INDEX) {
        float d = OX_FETCH_2D(pc.depth, ivec2(gl_FragCoord.xy), 0).r;
        if (d > 0.0) {
            float sceneLin = oxLinearDepth(pc.view, d);
            float diff = sceneLin - vParams.x;
            fade = vParams.y > 0.0 ? clamp(diff / vParams.y, 0.0, 1.0) : (diff >= 0.0 ? 1.0 : 0.0);
        }
    }
    uint blend = uint(vParams.w + 0.5);
    float alpha = clamp(c.a * fade, 0.0, 1.0);
    vec3 rgb;
    if (blend == 2u) rgb = c.rgb * fade;      // premultiplied texture / colour
    else rgb = c.rgb * alpha;                 // additive and alpha
    rgb = rgb * vFog.a + vFog.rgb * alpha;
    if (alpha <= 0.0 && dot(rgb, rgb) <= 0.0) discard;
    outColor = vec4(rgb * VIEW.preExposure, blend == 0u ? 0.0 : alpha);
}
