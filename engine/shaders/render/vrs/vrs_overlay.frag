#version 460
// Variable rate shading debug overlay (r.VRS.Mode DebugOverlay / r.VRS.Overlay): tints SceneColorLDR by the shading
// rate of each tile. 1×1 untouched, 1×2 / 2×1 blue, 2×2 green, 2×4 / 4×2 yellow, 4×4 red; tile borders darkened.
#include <render/vrs/vrs_common.glsl>

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

OX_PUSH_CONSTANTS({
    uint rate;       // sampled R8_UINT
    uint tileSize;
    vec2 renderSize;
    float opacity;
});

void main() {
    vec2 px = uv * pc.renderSize;
    ivec2 tile = ivec2(px / float(pc.tileSize));
    uint r = texelFetch(oxUTextures2D[nonuniformEXT(pc.rate)], tile, 0).r;
    uint lw = (r >> 2u) & 3u, lh = r & 3u;
    uint sum = lw + lh;
    vec3 c = vec3(0.0);
    float a = 0.0;
    if (sum == 1u) { c = vec3(0.15, 0.45, 1.0); a = 1.0; }
    else if (sum == 2u) { c = vec3(0.1, 0.9, 0.2); a = 1.0; }
    else if (sum == 3u) { c = vec3(1.0, 0.85, 0.1); a = 1.0; }
    else if (sum >= 4u) { c = vec3(1.0, 0.15, 0.1); a = 1.0; }
    vec2 f = fract(px / float(pc.tileSize)) * float(pc.tileSize);
    bool border = min(f.x, f.y) < 1.0;
    if (border) { c = vec3(0.0); a = max(a, 0.6); }
    outColor = vec4(c, a * pc.opacity);
}
