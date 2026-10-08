// OxwaldEngine volumetrics: froxel grid mapping, phase functions and analytic height fog. Shared by the fog
// passes, the composite and fog_sample.glsl (translucency). Grid parameters live in the view constants
// (VIEW.volumetricFogGrid / Depth / Lighting, written by the Volumetrics feature; grid.w == 0 → no froxel fog).
//
// Depth distribution (mirrors ox::render::volumetrics::FroxelGrid):
//   viewDepth(s) = (2^(s · log2(1 + far · scale)) - 1) / scale,   s = slice / gridZ ∈ [0, 1]
#ifndef OX_VOLUMETRICS_FOG_COMMON_GLSL
#define OX_VOLUMETRICS_FOG_COMMON_GLSL

#include <render/common/math.glsl>
#include <render/common/scene.glsl>

const float OX_FOG_SKY_DEPTH = 65000.0; // stands in for "infinitely far" (sky pixels)

bool oxFroxelFogActive(ViewBuffer vb) { return vb.v.volumetricFogGrid.w > 0.0; }

float oxFroxelSliceToDepth(ViewBuffer vb, float slice01) {
    vec4 d = vb.v.volumetricFogDepth;
    return (exp2(slice01 * d.y) - 1.0) / d.x;
}

float oxFroxelDepthToSlice(ViewBuffer vb, float viewDepth) {
    vec4 d = vb.v.volumetricFogDepth;
    return log2(1.0 + max(viewDepth, 0.0) * d.x) / d.y;
}

// Camera forward (world) and the world position at a positive view depth along the ray through `uv`
// (render-resolution uv, top-left origin). Works for perspective and orthographic views.
vec3 oxFogCameraForward(ViewBuffer vb) { return -normalize(vb.v.invView[2].xyz); }

vec3 oxFogWorldPosition(ViewBuffer vb, vec2 uv, float viewDepth) {
    mat4 ivp = vb.v.invViewProj;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 n = ivp * vec4(ndc, 1.0, 1.0); // reversed-Z: device depth 1 = near plane
    vec4 f = ivp * vec4(ndc, 0.5, 1.0);
    vec3 pn = n.xyz / n.w;
    vec3 dir = f.xyz / f.w - pn;
    vec3 fwd = oxFogCameraForward(vb);
    float nearDepth = dot(pn - vb.v.cameraPosition.xyz, fwd);
    return pn + dir * ((viewDepth - nearDepth) / dot(dir, fwd));
}

// Distance along the view ray per metre of view depth (1 / cos of the angle to the camera axis).
float oxFogRayScale(ViewBuffer vb, vec2 uv) {
    vec3 a = oxFogWorldPosition(vb, uv, 1.0), b = oxFogWorldPosition(vb, uv, 2.0);
    return length(b - a);
}

// Continuous froxel coordinate (x, y in froxels, z in slices; centres at integer + 0.5) → world position.
vec3 oxFroxelWorldPosition(ViewBuffer vb, vec3 froxel, out float viewDepth) {
    vec3 grid = vb.v.volumetricFogGrid.xyz;
    viewDepth = oxFroxelSliceToDepth(vb, froxel.z / grid.z);
    return oxFogWorldPosition(vb, froxel.xy / grid.xy, viewDepth);
}

// --- phase functions ---

float oxPhaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * OX_PI * denom * sqrt(denom));
}

// Mean sky radiance (DC term of the irradiance SH; SH stores irradiance / π) × ambient intensity.
vec3 oxFogAmbientRadiance(ViewBuffer vb) {
    OxSHBuffer sh = vb.v.irradianceSH;
    return max(sh.c[0].xyz * 0.282095, vec3(0.0)) * vb.v.iblIntensity;
}

// --- exponential height fog (EnvironmentComponent: density at y = 0, height falloff, start distance) ---

float oxHeightFogDensity(ViewBuffer vb, vec3 worldPos, float distanceFromCamera) {
    vec4 fp = vb.v.fogParams;
    if (fp.x <= 0.0 || distanceFromCamera < fp.z) return 0.0;
    return fp.x * exp(-fp.y * clamp(worldPos.y, -50.0 / max(fp.y, 1e-4), 1e6));
}

// Optical depth of the height fog along origin + dir · t for t ∈ [a, b].
float oxHeightFogOpticalDepth(ViewBuffer vb, vec3 origin, vec3 dir, float a, float b) {
    vec4 fp = vb.v.fogParams;
    a = max(a, fp.z);
    if (fp.x <= 0.0 || b <= a) return 0.0;
    float f = max(fp.y, 1e-5);
    float k = f * dir.y;
    float ya = origin.y + dir.y * a;
    if (abs(k) < 1e-5) return fp.x * exp(-f * clamp(ya, -80.0 / f, 80.0 / f)) * (b - a);
    float yb = origin.y + dir.y * b;
    float ea = exp(-clamp(f * ya, -80.0, 80.0));
    float eb = exp(-clamp(f * yb, -80.0, 80.0));
    return fp.x * (ea - eb) / k;
}

// Radiance scattered towards `toCamera` by a unit-albedo medium lit by the sun (unshadowed) and the sky.
vec3 oxFogUnshadowedInscatter(ViewBuffer vb, SceneBuffer sb, vec3 toCamera, float g) {
    vec3 L = oxFogAmbientRadiance(vb) * vb.v.volumetricFogLighting.x;
    int sun = vb.v.sunLight;
    if (sun >= 0) {
        Light l = sb.s.lights.l[sun];
        L += l.color * oxPhaseHG(dot(l.direction, toCamera), g) * vb.v.volumetricFogLighting.y;
    }
    return L;
}

// Analytic height fog between distances a and b along the view ray (beyond the froxel grid / sky):
// rgb = in-scatter × preExposure, a = transmittance.
vec4 oxAnalyticHeightFog(ViewBuffer vb, SceneBuffer sb, vec3 dir, float a, float b) {
    if (vb.v.fogParams.x <= 0.0 || b <= a) return vec4(0.0, 0.0, 0.0, 1.0);
    vec3 cam = vb.v.cameraPosition.xyz;
    float tau = oxHeightFogOpticalDepth(vb, cam, dir, a, b);
    float T = exp(-tau);
    vec3 L = vb.v.fogColor.rgb * oxFogUnshadowedInscatter(vb, sb, -dir, vb.v.volumetricFogLighting.z);
    return vec4(L * (1.0 - T) * vb.v.preExposure, T);
}

#endif
