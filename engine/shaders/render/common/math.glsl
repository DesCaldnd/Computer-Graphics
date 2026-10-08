// OxwaldEngine render: small math helpers (noise, sampling sequences, encodings).
#ifndef OX_RENDER_MATH_GLSL
#define OX_RENDER_MATH_GLSL

const float OX_PI = 3.14159265358979;
const float OX_TWO_PI = 6.28318530717959;
const float OX_INV_PI = 0.31830988618379;

float oxSaturate(float x) { return clamp(x, 0.0, 1.0); }
vec3 oxSaturate(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }
float oxPow5(float x) { float x2 = x * x; return x2 * x2 * x; }
float oxLuminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Jimenez 2014: interleaved gradient noise in [0,1).
float oxInterleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float oxRadicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 oxHammersley(uint i, uint n) { return vec2(float(i) / float(n), oxRadicalInverseVdC(i)); }

// Orthonormal basis around n (Duff et al. 2017).
void oxBasis(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

// Octahedral encoding of unit vectors.
vec2 oxOctWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 oxOctEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    n.xy = n.z >= 0.0 ? n.xy : oxOctWrap(n.xy);
    return n.xy * 0.5 + 0.5;
}
vec3 oxOctDecode(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// Cube map face direction for a texel of face `face` (Vulkan order +X,-X,+Y,-Y,+Z,-Z), uv in [0,1].
vec3 oxCubeDirection(uint face, vec2 uv) {
    vec2 p = uv * 2.0 - 1.0;
    switch (face) {
    case 0u: return normalize(vec3(1.0, -p.y, -p.x));
    case 1u: return normalize(vec3(-1.0, -p.y, p.x));
    case 2u: return normalize(vec3(p.x, 1.0, p.y));
    case 3u: return normalize(vec3(p.x, -1.0, -p.y));
    case 4u: return normalize(vec3(p.x, -p.y, 1.0));
    default: return normalize(vec3(-p.x, -p.y, -1.0));
    }
}

// 16-tap Poisson disk (unit radius).
const vec2 OX_POISSON16[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2(0.34495938, 0.29387760), vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379), vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790));

mat2 oxRotation2D(float angle) {
    float s = sin(angle), c = cos(angle);
    return mat2(c, s, -s, c);
}

#endif
