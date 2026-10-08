// OxwaldEngine render: helpers shared by the SSR passes (random numbers, GGX VNDF sampling, projections).
#ifndef OX_REFLECTIONS_SSR_COMMON_GLSL
#define OX_REFLECTIONS_SSR_COMMON_GLSL

#include <render/common/pbr.glsl>
#include <render/common/view.glsl>

uint oxHashU(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

vec2 oxRandom2(uvec2 pixel, uint frame) {
    uint h = oxHashU(pixel.x * 1973u + pixel.y * 9277u + frame * 26699u);
    uint h2 = oxHashU(h ^ 0x9e3779b9u);
    return vec2(float(h & 0xffffffu), float(h2 & 0xffffffu)) / 16777216.0;
}

// Heitz 2018: sample a visible GGX normal in tangent space (Ve = view direction in tangent space, z = normal).
vec3 oxSampleGGXVNDF(vec3 Ve, float a, vec2 u) {
    vec3 Vh = normalize(vec3(a * Ve.x, a * Ve.y, Ve.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(Vh, T1);
    float r = sqrt(u.x);
    float phi = OX_TWO_PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(vec3(a * Nh.x, a * Nh.y, max(0.0, Nh.z)));
}

float oxSmithG1(float NdotV, float a) {
    float a2 = a * a;
    return 2.0 * NdotV / (NdotV + sqrt(a2 + (1.0 - a2) * NdotV * NdotV));
}

// pdf of the reflected direction for VNDF sampling: D(H) G1(V) / (4 NdotV).
float oxVndfPdf(float NdotH, float NdotV, float a) {
    return oxDGGX(NdotH, a) * oxSmithG1(NdotV, a) / max(4.0 * NdotV, 1e-5);
}

vec3 oxProjectView(ViewBuffer vb, vec3 viewPos) {
    vec4 c = vb.v.proj * vec4(viewPos, 1.0);
    return vec3(c.xy / c.w * 0.5 + 0.5, c.z / c.w);
}

#endif
