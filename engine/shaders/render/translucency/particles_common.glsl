// GPU particles: shared structs (mirror ox::render ParticleGpu / EmitterGpu in particles_feature.cpp, scalar layout),
// buffers, random numbers and curl noise.
#ifndef OX_TRANSLUCENCY_PARTICLES_COMMON_GLSL
#define OX_TRANSLUCENCY_PARTICLES_COMMON_GLSL

#include <render/common/math.glsl>
#include <render/common/scene.glsl>

struct Particle {
    vec3 position;
    float age;
    vec3 velocity;
    float lifetime;   // <= 0: dead slot
    float size;
    float rotation;
    float rotationSpeed;
    float frameOffset;
    uint seed;
    uint pad0;
    float pad1;
    float pad2;
};

struct Emitter {
    mat4 world;
    vec4 shape;      // x shape (0 point, 1 sphere, 2 cone, 3 box, 4 mesh), y radius, z cone half angle (rad), w shell
    vec4 box;        // xyz half extents
    vec4 lifetime;   // min, max lifetime, min, max speed
    vec4 velocity;   // xyz initial velocity (world), w drag
    vec4 gravity;    // xyz acceleration (world), w turbulence strength
    vec4 turbulence; // frequency, speed, time, dt
    vec4 size;       // min, max start size, min, max start rotation (rad)
    vec4 rotation;   // min, max rotation speed (rad/s), stretch, soft distance
    vec4 color;      // base colour (linear rgb, a)
    vec4 flipbook;   // columns, rows, fps (0 = over lifetime), random start frame
    vec4 collision;  // x mode (0 none, 1 bounce, 2 kill), y bounce, z friction, w emissive
    uint maxParticles;
    uint emitCount;
    uint seed;       // per frame
    uint current;    // alive list read this frame (0/1)
    uint meshFirstIndex;
    uint meshTriangles;
    int meshVertexOffset;
    uint flags;      // bit 0 lit, bits 1-2 blend (0 additive, 1 alpha, 2 premultiplied), bits 3-4 render mode, bit 5 sort
    uint gradient;   // 64×2 RGBA16F: row 0 colour over life, row 1 r = size over life
    uint texture;    // sprite / flipbook atlas
    uint meshIndexCount;
    uint pad;
};

const uint OX_PARTICLE_LIT = 1u;
const uint OX_PARTICLE_SORT = 32u;
uint oxParticleBlend(Emitter e) { return (e.flags >> 1) & 3u; }
uint oxParticleRenderMode(Emitter e) { return (e.flags >> 3) & 3u; }

OX_BUFFER(ParticleBuffer, { Particle p[]; });
OX_READONLY_BUFFER(EmitterBuffer, { Emitter e; });
// lists: dead[max], alive0[max], alive1[max]; sort pairs follow when sorting (key, index) × pow2(max)
OX_BUFFER(ParticleLists, { uint v[]; });
// counters: dead, alive0, alive1, pad, VkDrawIndirectCommand, VkDrawIndexedIndirectCommand, pad × 3
OX_BUFFER(ParticleCounters, { int c[16]; });
OX_READONLY_BUFFER(IndexBufferRef, { uint i[]; });

uint oxPcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float oxRandom(inout uint state) {
    state = oxPcg(state);
    return float(state >> 8) * (1.0 / 16777216.0);
}
vec3 oxRandomUnitVector(inout uint state) {
    float z = oxRandom(state) * 2.0 - 1.0;
    float a = oxRandom(state) * OX_TWO_PI;
    float r = sqrt(max(1.0 - z * z, 0.0));
    return vec3(r * cos(a), r * sin(a), z);
}

// Divergence-free curl of a trigonometric vector potential (two octaves), roughly unit magnitude.
vec3 oxCurl(vec3 p, float t) {
    vec3 c = vec3(0.0);
    float amp = 1.0;
    for (int o = 0; o < 2; ++o) {
        vec3 q = p;
        c.x += amp * (-sin(q.x + t) * sin(q.y) - cos(q.z + t) * cos(q.x));
        c.y += amp * (-sin(q.y + t) * sin(q.z) - cos(q.x + t) * cos(q.y));
        c.z += amp * (-sin(q.z + t) * sin(q.x) - cos(q.y + t) * cos(q.z));
        p = mat3(0.8, 0.6, 0.0, -0.48, 0.64, 0.6, 0.36, -0.48, 0.8) * p * 2.03 + 1.7;
        amp *= 0.5;
    }
    return c * 0.5;
}

#endif
