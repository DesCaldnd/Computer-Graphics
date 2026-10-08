// OxwaldEngine render: metallic-roughness PBR (GGX, height-correlated Smith, Schlick), light attenuation, SH9.
#ifndef OX_RENDER_PBR_GLSL
#define OX_RENDER_PBR_GLSL

#include "math.glsl"

const float OX_MIN_PERCEPTUAL_ROUGHNESS = 0.045;

struct OxSurface {
    vec3 position;       // world
    vec3 normal;         // world, normalised (shading normal)
    vec3 geometricNormal;
    vec3 view;           // world, surface → camera, normalised
    vec3 baseColor;
    float alpha;
    float metallic;
    float perceptualRoughness;
    float roughness;     // alpha = perceptual²
    float occlusion;     // material AO
    vec3 emissive;
    vec3 diffuseColor;
    vec3 f0;
    float NdotV;
};

void oxSurfaceFinalize(inout OxSurface s) {
    s.perceptualRoughness = clamp(s.perceptualRoughness, OX_MIN_PERCEPTUAL_ROUGHNESS, 1.0);
    s.roughness = s.perceptualRoughness * s.perceptualRoughness;
    s.diffuseColor = s.baseColor * (1.0 - s.metallic);
    s.f0 = mix(vec3(0.04), s.baseColor, s.metallic);
    s.NdotV = max(dot(s.normal, s.view), 1e-4);
}

float oxDGGX(float NdotH, float a) {
    float a2 = a * a;
    float f = (NdotH * a2 - NdotH) * NdotH + 1.0;
    return a2 / (OX_PI * f * f);
}

// Height-correlated Smith visibility V = G / (4 NdotL NdotV).
float oxVSmithGGXCorrelated(float NdotV, float NdotL, float a) {
    float a2 = a * a;
    float lv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float ll = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(lv + ll, 1e-5);
}

vec3 oxFSchlick(float VdotH, vec3 f0) { return f0 + (vec3(1.0) - f0) * oxPow5(1.0 - VdotH); }
vec3 oxFSchlickRoughness(float NdotV, vec3 f0, float perceptualRoughness) {
    return f0 + (max(vec3(1.0 - perceptualRoughness), f0) - f0) * oxPow5(1.0 - NdotV);
}

// Direct lighting BRDF × NdotL for a light from direction L (surface → light). energyComp = multi-scatter factor.
vec3 oxBrdfDirect(OxSurface s, vec3 L, vec3 energyComp) {
    vec3 H = normalize(s.view + L);
    float NdotL = clamp(dot(s.normal, L), 0.0, 1.0);
    if (NdotL <= 0.0) return vec3(0.0);
    float NdotH = clamp(dot(s.normal, H), 0.0, 1.0);
    float LdotH = clamp(dot(L, H), 0.0, 1.0);
    float D = oxDGGX(NdotH, s.roughness);
    float V = oxVSmithGGXCorrelated(s.NdotV, NdotL, s.roughness);
    vec3 F = oxFSchlick(LdotH, s.f0);
    vec3 specular = D * V * F * energyComp;
    vec3 diffuse = s.diffuseColor * OX_INV_PI;
    return (diffuse + specular) * NdotL;
}

// Smooth inverse-square falloff windowed to zero at `range` (Karis 2013).
float oxDistanceAttenuation(float distanceSq, float range) {
    float invRange = 1.0 / max(range, 1e-4);
    float factor = distanceSq * invRange * invRange;
    float window = clamp(1.0 - factor * factor, 0.0, 1.0);
    return window * window / max(distanceSq, 1e-4);
}

float oxSpotAttenuation(vec3 L, vec3 spotDirection, float spotScale, float spotOffset) {
    float cd = dot(-L, spotDirection);
    float a = clamp(cd * spotScale + spotOffset, 0.0, 1.0);
    return a * a;
}

// --- spherical harmonics (order 2, 9 coefficients, rgb in vec4.xyz) ---
// Coefficients are pre-convolved with the clamped cosine lobe and divided by π: the result is the diffuse radiance
// of a white Lambertian surface (irradiance / π).
vec3 oxEvalSH9(vec4 sh[9], vec3 n) {
    vec3 r = sh[0].xyz * 0.282095;
    r += sh[1].xyz * 0.488603 * n.y;
    r += sh[2].xyz * 0.488603 * n.z;
    r += sh[3].xyz * 0.488603 * n.x;
    r += sh[4].xyz * 1.092548 * n.x * n.y;
    r += sh[5].xyz * 1.092548 * n.y * n.z;
    r += sh[6].xyz * 0.315392 * (3.0 * n.z * n.z - 1.0);
    r += sh[7].xyz * 1.092548 * n.x * n.z;
    r += sh[8].xyz * 0.546274 * (n.x * n.x - n.y * n.y);
    return max(r, vec3(0.0));
}

// Lagarde 2014 specular occlusion from ambient occlusion.
float oxSpecularOcclusion(float NdotV, float ao, float roughness) {
    return clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

// GGX importance sampling (for IBL prefiltering).
vec3 oxImportanceSampleGGX(vec2 xi, float a, vec3 n) {
    float phi = OX_TWO_PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3 h = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    vec3 t, b;
    oxBasis(n, t, b);
    return normalize(t * h.x + b * h.y + n * h.z);
}

#endif
