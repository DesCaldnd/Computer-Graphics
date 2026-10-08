// OxwaldEngine post-processing helpers (colour spaces, reversible tonemap, hashing).
#ifndef OX_PP_COMMON_GLSL
#define OX_PP_COMMON_GLSL

#include <render/common/math.glsl>
#include <render/common/view.glsl>

float ppMax3(vec3 c) { return max(c.r, max(c.g, c.b)); }
float ppMin3(vec3 c) { return min(c.r, min(c.g, c.b)); }
float ppLuma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 ppRgbToYCoCg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}
vec3 ppYCoCgToRgb(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Reversible "max channel" Reinhard (Karis): compresses HDR outliers before filtering, exact inverse afterwards.
vec3 ppTonemap(vec3 c) { return c / (1.0 + ppMax3(c)); }
vec3 ppTonemapInverse(vec3 c) { return c / max(1.0 - ppMax3(c), 1.0 / 4096.0); }

vec3 ppSanitize(vec3 c) {
    if (any(isnan(c)) || any(isinf(c))) return vec3(0.0);
    return max(c, vec3(0.0));
}

// Integer hash → [0, 1) (film grain, sample rotation).
float ppHash(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return float(v.x & 0x00FFFFFFu) / 16777216.0;
}

#endif
