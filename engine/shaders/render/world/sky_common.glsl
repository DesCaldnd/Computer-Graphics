// OxwaldEngine render / world: physically scaled sky (cd/m²) — Preetham daylight with twilight fade, night sky,
// moonlit sky, procedural star field rotated by the celestial frame, sun disc with limb darkening, moon disc whose
// phase follows from the sun direction. Mirrors WorldSkyGpu in features/world/world_sky.cpp.
#ifndef OX_RENDER_WORLD_SKY_COMMON_GLSL
#define OX_RENDER_WORLD_SKY_COMMON_GLSL

#include <world/preetham.glsl>
#include "world_common.glsl"

const uint OX_SKY_SUN_DISC = 1u;
const uint OX_SKY_MOON = 2u;
const uint OX_SKY_STARS = 4u;

struct WorldSky {
    vec4 preetham[8];
    vec4 sunDir;    // xyz towards the sun, w angular radius (rad)
    vec4 sunColor;  // rgb = colour × illuminance (lux) × disc intensity, w = sun elevation (deg)
    vec4 moonDir;   // xyz towards the moon, w angular radius (rad)
    vec4 moonColor; // rgb = colour × illuminance (lux) × intensity, w = illuminated fraction
    vec4 starsRot;  // quaternion (xyzw) celestial → world
    vec4 params;    // sky intensity, stars intensity, time (s), daylight weight
    vec4 night;     // rgb night sky radiance (cd/m²), w = moonlit sky radiance per lux
    vec4 aerial;    // rayleigh scale, height falloff (1/m), haze (mie) density, enabled
    uint flags;
    uint skyCube;   // radiance cube of this sky (IBL source, aerial perspective)
    uint pad0;
    uint pad1;
};
OX_READONLY_BUFFER(WorldSkyBuffer, { WorldSky s; });

vec3 oxQuatRotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

vec3 oxWorldSkyPreetham(WorldSkyBuffer sb, vec3 dir) {
    OxPreetham p;
    p.A = sb.s.preetham[0]; p.B = sb.s.preetham[1]; p.C = sb.s.preetham[2]; p.D = sb.s.preetham[3];
    p.E = sb.s.preetham[4]; p.zenith = sb.s.preetham[5]; p.normalization = sb.s.preetham[6];
    p.sunDirection = sb.s.preetham[7];
    return oxPreethamRadiance(p, dir);
}

// Diffuse sky dome without discs and stars (IBL source, aerial perspective in-scatter).
vec3 oxWorldSkyDome(WorldSkyBuffer sb, vec3 dir) {
    vec3 up = vec3(dir.x, max(dir.y, 0.0005), dir.z);
    float dayW = sb.s.params.w;
    vec3 c = vec3(0.0);
    if (dayW > 0.0) c += oxWorldSkyPreetham(sb, normalize(up)) * dayW;
    // Night sky (airglow, brighter towards the horizon) and moonlit Rayleigh sky.
    float horizon = 1.0 - clamp(up.y, 0.0, 1.0);
    c += sb.s.night.rgb * (0.7 + 0.6 * horizon);
    float moonUp = smoothstep(-0.05, 0.1, sb.s.moonDir.y);
    float cm = dot(normalize(up), sb.s.moonDir.xyz);
    c += sb.s.moonColor.rgb * sb.s.night.w * vec3(0.55, 0.7, 1.0) * (0.75 + 0.25 * cm * cm + 0.3 * horizon) * moonUp;
    if (dir.y < 0.0) {
        // Ground: darker, slightly warm reflection of the horizon sky.
        c *= mix(0.3, 0.15, clamp(-dir.y * 3.0, 0.0, 1.0)) * vec3(1.0, 0.95, 0.88);
    }
    return c * sb.s.params.x;
}

// Procedural stars in the celestial frame (rotated with the sky). Returns cd/m².
vec3 oxWorldStars(WorldSkyBuffer sb, vec3 dir, float pixelAngle) {
    vec4 q = sb.s.starsRot;
    vec3 cel = oxQuatRotate(vec4(-q.xyz, q.w), dir); // inverse rotation: world → celestial
    const float cells = 220.0;
    vec3 p = cel * cells;
    vec3 base = floor(p - 0.5);
    vec3 acc = vec3(0.0);
    float t = sb.s.params.z;
    for (int k = 0; k < 8; ++k) {
        vec3 c = base + vec3(float(k & 1), float((k >> 1) & 1), float(k >> 2));
        vec3 h = oxWorldHash33(c + 17.0);
        if (h.x > 0.012) continue; // ~1 % of the cells hold a star (a few thousand over the sky)
        vec3 starPos = c + 0.15 + 0.7 * oxWorldHash33(c * 1.7 + 3.1);
        vec3 starDir = normalize(starPos);
        float ang = acos(clamp(dot(starDir, cel), -1.0, 1.0));
        float size = max(pixelAngle * 0.45, 1e-4);
        float g = exp(-ang * ang / (size * size));
        // Magnitude distribution: many faint, few bright stars (cd/m² of a star-covered pixel).
        float brightness = 0.004 + 0.5 * pow(h.y, 10.0) + 0.04 * h.y * h.y;
        // Colour from a rough temperature (blue-white to orange).
        vec3 tint = mix(vec3(1.0, 0.75, 0.55), vec3(0.75, 0.85, 1.0), h.z);
        float twinkle = 1.0 + 0.45 * sin(t * (2.5 + 9.0 * h.z) + h.y * 40.0) * (1.0 - clamp(dir.y, 0.0, 1.0));
        acc += tint * brightness * g * twinkle;
    }
    // Extinction close to the horizon.
    return acc * smoothstep(-0.02, 0.2, dir.y);
}

// Full sky radiance (cd/m², before pre-exposure) for the sky pass. pixelAngle = angular size of a pixel (rad).
vec3 oxWorldSkyRadiance(WorldSkyBuffer sb, vec3 dir, float pixelAngle, float maxRadiance) {
    vec3 c = oxWorldSkyDome(sb, dir);
    uint flags = sb.s.flags;
    if (dir.y < -0.01) return c;
    bool occluded = false;
    // Moon disc (geometry gives the phase: the sphere is lit from the sun direction; faint earthshine).
    if ((flags & OX_SKY_MOON) != 0u) {
        vec3 md = sb.s.moonDir.xyz;
        float r = sb.s.moonDir.w;
        float cd = dot(dir, md);
        if (cd > cos(r)) {
            vec3 right = normalize(cross(abs(md.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0), md));
            vec3 up = cross(md, right);
            vec2 xy = vec2(dot(dir, right), dot(dir, up)) / sin(r);
            float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
            vec3 n = normalize(right * xy.x + up * xy.y + md * z);
            float lit = max(dot(n, sb.s.sunDir.xyz), 0.0);
            float maria = 0.72 + 0.28 * oxWorldFbm(xy * 3.1 + 4.0);
            float frac = max(sb.s.moonColor.w, 0.02);
            vec3 L = sb.s.moonColor.rgb / frac / (OX_PI * r * r) * 1.5 * (lit + 0.002) * maria;
            float edge = smoothstep(1.0, 0.96, length(xy));
            c = mix(c, c * 0.2 + L, edge * smoothstep(-0.02, 0.02, md.y));
            occluded = edge > 0.5;
        }
    }
    if (!occluded && (flags & OX_SKY_STARS) != 0u && sb.s.params.y > 0.0) {
        c += oxWorldStars(sb, dir, pixelAngle) * sb.s.params.y;
    }
    // Sun disc with limb darkening I(μ) = 1 - u(1 - μ), normalised so the disc integrates to the illuminance.
    if ((flags & OX_SKY_SUN_DISC) != 0u) {
        vec3 sd = sb.s.sunDir.xyz;
        float r = sb.s.sunDir.w;
        float cd = dot(dir, sd);
        float cosR = cos(r);
        if (cd > cosR) {
            float rr = clamp(acos(clamp(cd, -1.0, 1.0)) / r, 0.0, 1.0);
            float mu = sqrt(1.0 - rr * rr);
            const float u = 0.6;
            float limb = (1.0 - u * (1.0 - mu)) / (1.0 - u / 3.0);
            vec3 L = sb.s.sunColor.rgb / (OX_PI * r * r) * limb;
            float edge = smoothstep(1.0, 0.97, rr);
            c += min(L * edge, vec3(maxRadiance)) * smoothstep(-0.01, 0.005, sd.y);
        }
    }
    return c;
}

#endif
