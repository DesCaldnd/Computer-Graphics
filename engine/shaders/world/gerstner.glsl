// Sum-of-Gerstner-waves water displacement. Mirrors ox::world::GerstnerWaves (engine/world/src/water.cpp)
// exactly — keep both in sync so CPU buoyancy matches what is rendered.
//
// Data layout (std140/std430, 528 bytes) = ox::world::GerstnerParamsGpu:
//   OxGerstnerWave waves[OX_GERSTNER_MAX_WAVES];   dirK = (dir.x, dir.z, k, omega), amp = (A, qa, phase, 0)
//   vec4 info;                                     (count, baseHeight, time, 0)
// Usage:
//   layout(std140, set = 0, binding = 3) uniform WaterUbo { OxGerstnerParams gerstner; };
//   vec3 p  = vec3(x0.x, gerstner.info.y, x0.y) + oxGerstnerDisplacement(gerstner, x0, gerstner.info.z);
//   vec3 n  = oxGerstnerNormal(gerstner, x0, gerstner.info.z);
#ifndef OX_WORLD_GERSTNER_GLSL
#define OX_WORLD_GERSTNER_GLSL

#define OX_GERSTNER_MAX_WAVES 16

struct OxGerstnerWave {
    vec4 dirK;
    vec4 amp;
};

struct OxGerstnerParams {
    OxGerstnerWave waves[OX_GERSTNER_MAX_WAVES];
    vec4 info;
};

// Lagrangian displacement of rest point x0 (world XZ): returns (dx, height above base, dz).
vec3 oxGerstnerDisplacement(OxGerstnerParams g, vec2 x0, float t) {
    vec3 p = vec3(0.0);
    int count = min(int(g.info.x), OX_GERSTNER_MAX_WAVES);
    for (int i = 0; i < count; ++i) {
        OxGerstnerWave w = g.waves[i];
        vec2 d = w.dirK.xy;
        float theta = w.dirK.z * dot(d, x0) - w.dirK.w * t + w.amp.z;
        float c = cos(theta);
        float s = sin(theta);
        p.x += w.amp.y * d.x * c;
        p.z += w.amp.y * d.y * c;
        p.y += w.amp.x * s;
    }
    return p;
}

// Normal of the displaced surface at rest point x0 (cross product of the parametric tangents).
vec3 oxGerstnerNormal(OxGerstnerParams g, vec2 x0, float t) {
    vec3 tx = vec3(1.0, 0.0, 0.0);
    vec3 tz = vec3(0.0, 0.0, 1.0);
    int count = min(int(g.info.x), OX_GERSTNER_MAX_WAVES);
    for (int i = 0; i < count; ++i) {
        OxGerstnerWave w = g.waves[i];
        vec2 d = w.dirK.xy;
        float k = w.dirK.z;
        float theta = k * dot(d, x0) - w.dirK.w * t + w.amp.z;
        float c = cos(theta);
        float s = sin(theta);
        float qks = w.amp.y * k * s;
        float akc = w.amp.x * k * c;
        tx += vec3(-qks * d.x * d.x, akc * d.x, -qks * d.x * d.y);
        tz += vec3(-qks * d.x * d.y, akc * d.y, -qks * d.y * d.y);
    }
    return normalize(cross(tz, tx));
}

// Eulerian height at world XZ (fixed-point inversion of the horizontal displacement), e.g. for
// screen-space water depth or particles. Matches GerstnerWaves::heightAt with the same iterations.
vec2 oxGerstnerRestPoint(OxGerstnerParams g, vec2 xz, float t, int iterations) {
    vec2 x0 = xz;
    for (int i = 0; i < iterations; ++i) {
        vec3 d = oxGerstnerDisplacement(g, x0, t);
        x0 = xz - d.xz;
    }
    return x0;
}

float oxGerstnerHeight(OxGerstnerParams g, vec2 xz, float t, int iterations) {
    return g.info.y + oxGerstnerDisplacement(g, oxGerstnerRestPoint(g, xz, t, iterations), t).y;
}

#endif
