// OxwaldEngine ray tracing: shared helpers of the SVGF-style denoiser (denoise_*.comp). Works on any device.
#ifndef OX_RT_DENOISE_COMMON_GLSL
#define OX_RT_DENOISE_COMMON_GLSL

#include <render/common/scene.glsl>

// G-buffer pixel of a signal pixel (signal at render / scale).
ivec2 oxDnGbufferPixel(ivec2 p, uint scale, ivec2 renderSize) {
    return clamp(p * int(scale) + int(scale / 2u), ivec2(0), renderSize - 1);
}

// Linear view depth (0 = sky / no geometry) and world normal at a G-buffer pixel.
void oxDnGeometry(ViewBuffer vb, uint depthTex, uint normalTex, ivec2 gp, out float z, out vec3 n) {
    float d = OX_FETCH_2D(depthTex, gp, 0).r;
    n = OX_FETCH_2D(normalTex, gp, 0).xyz;
    z = d > 0.0 ? oxLinearDepth(vb, d) : 0.0;
    float len = length(n);
    n = len > 1e-4 ? n / len : vec3(0.0, 1.0, 0.0);
}

float oxDnLuma(vec4 c, vec4 w) { return dot(c, w); }

// Signal-resolution geometry written by the temporal pass (xyz normal, w linear depth; 0 = sky).
void oxDnGeo(uint geoTex, ivec2 p, out float z, out vec3 n) {
    vec4 g = OX_FETCH_2D(geoTex, p, 0);
    z = g.w;
    n = g.xyz;
}

// Edge-stopping test used by reprojection (relative depth + normal similarity).
bool oxDnSimilar(float z, vec3 n, float pz, vec3 pn) {
    if (pz <= 0.0 || z <= 0.0) return false;
    return abs(z - pz) / max(z, 1e-3) < 0.1 && dot(n, pn) > 0.9;
}

#endif
