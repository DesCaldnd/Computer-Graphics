// Water surface parameters (mirrors ox::render WaterGpu in water_feature.cpp, scalar layout) + Gerstner helpers.
#ifndef OX_TRANSLUCENCY_WATER_COMMON_GLSL
#define OX_TRANSLUCENCY_WATER_COMMON_GLSL

#include <render/common/scene.glsl>
#include <world/gerstner.glsl>

struct WaterParams {
    OxGerstnerParams gerstner; // info = (count, base height, time, 0)
    vec4 rect;                 // centre xz, half size xz (<= 0 = unbounded)
    vec4 absorption;           // rgb 1/m, w = refraction strength
    vec4 scatter;              // rgb in-scatter colour, w = roughness
    vec4 detail;               // normal strength, tiles per metre, scroll speed, amplitude sum
    vec4 foam;                 // shore distance, crest amount, intensity, w = surface height above the camera XZ (CPU)
    vec4 caustics;             // intensity, tiles per metre, falloff per metre, unused
    vec4 underwater;           // rgb fog colour, w = density
    vec4 grid;                 // centre xz (snapped camera), half extent, alpha (near cell size factor)
    uint normalMap;
    uint causticsMap;
    uint flags;                // bit 0 unbounded
    uint pad;
};

OX_READONLY_BUFFER(WaterBuffer, { WaterParams w; });

float oxWaterTime(WaterBuffer wb) { return wb.w.gerstner.info.z; }
float oxWaterBase(WaterBuffer wb) { return wb.w.gerstner.info.y; }

bool oxWaterInside(WaterBuffer wb, vec2 xz) {
    vec4 r = wb.w.rect;
    if (r.z <= 0.0 || r.w <= 0.0) return true;
    vec2 d = abs(xz - r.xy);
    return d.x <= r.z && d.y <= r.w;
}

// Local copy of the active waves only (the world gerstner.glsl functions take the 528-byte struct by value: copying
// it from device memory per call would read all 16 waves; loops never read past info.x).
OxGerstnerParams oxWaterLoadGerstner(WaterBuffer wb) {
    OxGerstnerParams g;
    g.info = wb.w.gerstner.info;
    int count = min(int(g.info.x), OX_GERSTNER_MAX_WAVES);
    for (int i = 0; i < count; ++i) g.waves[i] = wb.w.gerstner.waves[i];
    return g;
}

// Per-pixel variants reading the waves straight from the buffer (uniform addresses, cached): the same math as
// oxGerstnerDisplacement / oxGerstnerNormal / oxGerstnerHeight in world/gerstner.glsl, without the local copy of the
// wave array, which Metal keeps in (slow) stack memory when it is indexed dynamically.
vec3 oxWaterDisplacement(WaterBuffer wb, vec2 x0, float t) {
    vec3 p = vec3(0.0);
    int count = min(int(wb.w.gerstner.info.x), OX_GERSTNER_MAX_WAVES);
    for (int i = 0; i < count; ++i) {
        vec4 dirK = wb.w.gerstner.waves[i].dirK;
        vec4 amp = wb.w.gerstner.waves[i].amp;
        float theta = dirK.z * dot(dirK.xy, x0) - dirK.w * t + amp.z;
        float c = cos(theta);
        p.x += amp.y * dirK.x * c;
        p.z += amp.y * dirK.y * c;
        p.y += amp.x * sin(theta);
    }
    return p;
}

vec3 oxWaterNormal(WaterBuffer wb, vec2 x0, float t) {
    vec3 tx = vec3(1.0, 0.0, 0.0);
    vec3 tz = vec3(0.0, 0.0, 1.0);
    int count = min(int(wb.w.gerstner.info.x), OX_GERSTNER_MAX_WAVES);
    for (int i = 0; i < count; ++i) {
        vec4 dirK = wb.w.gerstner.waves[i].dirK;
        vec4 amp = wb.w.gerstner.waves[i].amp;
        vec2 d = dirK.xy;
        float k = dirK.z;
        float theta = k * dot(d, x0) - dirK.w * t + amp.z;
        float qks = amp.y * k * sin(theta);
        float akc = amp.x * k * cos(theta);
        tx += vec3(-qks * d.x * d.x, akc * d.x, -qks * d.x * d.y);
        tz += vec3(-qks * d.x * d.y, akc * d.y, -qks * d.y * d.y);
    }
    return normalize(cross(tz, tx));
}

// Eulerian height of the displaced surface at a world XZ (fixed-point inversion, see oxGerstnerRestPoint).
float oxWaterHeight(WaterBuffer wb, vec2 xz, int iterations) {
    float t = wb.w.gerstner.info.z;
    vec2 x0 = xz;
    for (int i = 0; i < iterations; ++i) x0 = xz - oxWaterDisplacement(wb, x0, t).xz;
    return wb.w.gerstner.info.y + oxWaterDisplacement(wb, x0, t).y;
}

#endif
