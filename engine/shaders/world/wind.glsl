// Global wind with travelling gusts. Mirrors ox::world::WindField (engine/world/src/weather.cpp).
// Data = ox::world::WindGpu (std140, 32 bytes):
//   vec4 dirSpeedTime;  xy = direction (XZ, normalised), z = speed m/s, w = time s
//   vec4 gust;          x = gustStrength, y = gustWavelength, z = turbulence, w = turbulenceFrequency
#ifndef OX_WORLD_WIND_GLSL
#define OX_WORLD_WIND_GLSL

struct OxWind {
    vec4 dirSpeedTime;
    vec4 gust;
};

#define OX_TWO_PI 6.28318530718

float oxWindGustFactor(OxWind w, vec3 p) {
    vec2 dir = w.dirSpeedTime.xy;
    vec2 perp = vec2(-dir.y, dir.x);
    float lambda = max(w.gust.y, 1e-3);
    float along = (dot(p.xz, dir) - w.dirSpeedTime.z * w.dirSpeedTime.w) / lambda;
    float across = dot(p.xz, perp) / lambda;
    float g = 0.5 * sin(OX_TWO_PI * along + 0.7 * sin(OX_TWO_PI * across * 0.31)) +
              0.3 * sin(OX_TWO_PI * along * 2.71 + 1.7) +
              0.2 * sin(OX_TWO_PI * (along * 6.13 + across * 1.37) + 4.1);
    return 1.0 + w.gust.x * g;
}

// Horizontal wind velocity (m/s) at world position p.
vec3 oxWindSample(OxWind w, vec3 p) {
    vec2 dir = w.dirSpeedTime.xy;
    vec2 perp = vec2(-dir.y, dir.x);
    float lambda = max(w.gust.y, 1e-3);
    float across = dot(p.xz, perp) / lambda;
    float sway = sin(OX_TWO_PI * (w.gust.w * w.dirSpeedTime.w + across * 2.3));
    vec2 v = dir * (w.dirSpeedTime.z * oxWindGustFactor(w, p)) + perp * (w.dirSpeedTime.z * w.gust.z * sway);
    return vec3(v.x, 0.0, v.y);
}

#endif
