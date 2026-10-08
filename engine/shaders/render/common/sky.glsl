// OxwaldEngine render: sky radiance (cd/m²) for the sky pass and IBL capture. Modes: 0 procedural gradient driven
// by the sun, 1 environment cubemap (HDRI × skyIntensity), 2 Preetham (world::PreethamSky::Gpu coefficients).
#ifndef OX_RENDER_SKY_GLSL
#define OX_RENDER_SKY_GLSL

#include <world/preetham.glsl>
#include "math.glsl"
#include "scene.glsl"

// Analytic sky: zenith/horizon gradient scaled from the sun illuminance (clear sky ≈ 10% of the sun's lux as
// zenith luminance), ground bounce below the horizon. sunLux = 0 → a neutral 5000 cd/m² sky.
vec3 oxProceduralSky(vec3 dir, vec3 sunDir, float sunLux) {
    float base = sunLux > 0.0 ? sunLux * 0.1 : 5000.0;
    float sunHeight = sunLux > 0.0 ? clamp(sunDir.y, -0.2, 1.0) : 0.6;
    float dayFactor = smoothstep(-0.15, 0.25, sunHeight);
    vec3 zenith = vec3(0.22, 0.42, 0.85);
    vec3 horizon = vec3(0.75, 0.82, 0.95);
    vec3 sunset = vec3(1.0, 0.55, 0.3);
    float h = dir.y;
    vec3 sky;
    if (h >= 0.0) {
        float t = pow(1.0 - h, 3.0);
        sky = mix(zenith, horizon, t);
        float sunAmount = sunLux > 0.0 ? max(dot(dir, sunDir), 0.0) : 0.0;
        sky = mix(sky, sunset, (1.0 - dayFactor) * t * 0.8);
        sky += vec3(1.0, 0.9, 0.7) * pow(sunAmount, 8.0) * 0.6;
    } else {
        sky = mix(horizon * 0.45, vec3(0.18, 0.16, 0.14), clamp(-h * 4.0, 0.0, 1.0));
    }
    return sky * base * mix(0.05, 1.0, dayFactor);
}

vec3 oxSkyRadiance(ViewBuffer vb, SceneBuffer sb, vec3 dir, bool sunDisk) {
    uint mode = vb.v.skyMode;
    vec3 sunDir = vec3(0.0, 1.0, 0.0);
    vec3 sunColor = vec3(0.0);
    float sunRadius = 0.0;
    int sun = vb.v.sunLight;
    if (sun >= 0) {
        Light l = sb.s.lights.l[sun];
        sunDir = -l.direction;
        sunColor = l.color;
        sunRadius = max(vb.v.sunAngularRadius, 0.0045);
    }
    vec3 c;
    if (mode == 1u && vb.v.skyCube != OX_INVALID_INDEX) {
        c = textureLod(samplerCube(OX_TEXCUBE(vb.v.skyCube), OX_SAMPLER(OX_SAMPLER_LINEAR_CLAMP)), dir, 0.0).rgb;
    } else if (mode == 2u) {
        OxPreetham p;
        p.A = vb.v.preetham[0]; p.B = vb.v.preetham[1]; p.C = vb.v.preetham[2]; p.D = vb.v.preetham[3];
        p.E = vb.v.preetham[4]; p.zenith = vb.v.preetham[5]; p.normalization = vb.v.preetham[6];
        p.sunDirection = vb.v.preetham[7];
        c = dir.y >= 0.0 ? oxPreethamRadiance(p, dir) : oxPreethamRadiance(p, vec3(dir.x, 0.001, dir.z)) * 0.3;
    } else {
        c = oxProceduralSky(dir, sunDir, oxLuminance(sunColor));
    }
    c *= vb.v.skyIntensity;
    if (sunDisk && sun >= 0) {
        float cosR = cos(sunRadius);
        float cd = dot(dir, sunDir);
        if (cd > cosR) {
            // Disk radiance L = E / (π r²), limb darkened; clamped to stay inside fp16 after pre-exposure.
            float edge = smoothstep(cosR, mix(cosR, 1.0, 0.3), cd);
            vec3 L = sunColor / (OX_PI * sunRadius * sunRadius);
            c += min(L * edge, vec3(30000.0 / max(vb.v.preExposure, 1e-12)));
        }
    }
    return c;
}

#endif
