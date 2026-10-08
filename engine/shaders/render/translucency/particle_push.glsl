// Push constants of the particle render passes. The alive list is read as lists.v[listOffset + i × listStride + listSlot]
// (stride 2 / slot 1 for the sorted (key, slot) pairs).
#include "particles_common.glsl"
layout(push_constant, scalar) uniform OxPushConstants {
    ViewBuffer view;
    SceneBuffer scene;
    EmitterBuffer emitter;
    ParticleBuffer particles;
    ParticleLists lists;
    uint listOffset;
    uint listStride;
    uint listSlot;
    uint depth;       // depth used for soft particles: low-res closest depth (lowRes) or SceneDepthCopy
    uint fog;         // VolumetricFog
    uint lowRes;      // 1 = rendering into the low-resolution particle buffer
    vec2 targetSize;  // size of the render target of this pass
} pc;
#define EM pc.emitter.e
