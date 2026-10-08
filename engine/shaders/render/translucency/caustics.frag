#version 460
// Underwater light on opaque geometry below a water surface: Beer-Lambert attenuation of the light along the water
// column + caustics: animated projected texture (two scrolling layers, min-combined),
// refracted along the sun direction, attenuated with depth, sun shadow (ShadowMask) and fading in near the surface.
// Multiplicative blend (DST_COLOR, ZERO): SceneColorHDR *= 1 + caustics, so the lit albedo of the floor is modulated
// without a G-buffer.
#include "water_push.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 suv = gl_FragCoord.xy * VIEW.renderSize.zw;
    float d = OX_FETCH_2D(pc.sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (d <= 0.0 || VIEW.sunLight < 0) discard;
    vec3 P = oxWorldPositionFromDepth(pc.view, suv, d);
    if (!oxWaterInside(pc.water, P.xz)) discard;
    float surface = oxWaterHeight(pc.water, P.xz, 2);
    float depthBelow = surface - P.y;
    if (depthBelow <= 0.0) discard;

    Light sun = SCENE.lights.l[VIEW.sunLight];
    vec3 L = -sun.direction;
    if (L.y <= 0.0) discard;
    vec3 Lw = refract(-L, vec3(0.0, 1.0, 0.0), 1.0 / 1.333); // light direction under water (pointing down)
    vec2 entry = P.xz - Lw.xz * (depthBelow / max(-Lw.y, 1e-3));

    float time = oxWaterTime(pc.water);
    float scale = WATER.caustics.y;
    vec2 uvA = entry * scale + vec2(0.031, 0.017) * time;
    vec2 uvB = entry * scale * 1.27 + vec2(-0.023, 0.029) * time + 0.37;
    // Chromatic split: slightly different scale per channel.
    vec3 c;
    // Deeper floors see a blurrier pattern (wider light cones): mip by depth.
    float lod = clamp(depthBelow * 0.6, 0.0, 4.0);
    for (int ch = 0; ch < 3; ++ch) {
        float o = (float(ch) - 1.0) * 0.004 * depthBelow;
        float a = OX_SAMPLE_2D_LOD(WATER.causticsMap, OX_SAMPLER_LINEAR_REPEAT, uvA + o, lod).r;
        float b = OX_SAMPLE_2D_LOD(WATER.causticsMap, OX_SAMPLER_LINEAR_REPEAT, uvB - o, lod).r;
        c[ch] = min(a, b);
    }
    float atten = exp(-depthBelow * WATER.caustics.z) * smoothstep(0.0, 0.35, depthBelow) * L.y;
    float vis = pc.shadowMask != OX_INVALID_INDEX ? OX_SAMPLE_2D_LOD(pc.shadowMask, OX_SAMPLER_LINEAR_CLAMP, suv, 0.0).r : 1.0;
    // Light reaching the floor also crossed the water column above it (Beer-Lambert along the refracted sun ray).
    vec3 column = exp(-WATER.absorption.rgb * depthBelow / max(-Lw.y, 0.2));
    vec3 factor = column * (vec3(1.0) + c * 1.6 * WATER.caustics.x * atten * vis);
    outColor = vec4(factor, 1.0);
}
