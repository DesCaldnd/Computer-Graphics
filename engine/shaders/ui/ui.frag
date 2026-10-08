#version 460
#include "ui_common.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

// The target (SceneColorLDR) is UNORM and already display-encoded, so UI colours (authored in sRGB) are written and
// blended as is, which is what ImGui/RmlUi expect. Output is premultiplied (blend ONE, ONE_MINUS_SRC_ALPHA).
void main() {
    vec4 c = vColor * OX_SAMPLE_2D(pc.texture, OX_SAMPLER_LINEAR_CLAMP, vUv);
    if ((pc.flags & OX_UI_PREMULTIPLIED) == 0u) c.rgb *= c.a;
    outColor = c;
}
