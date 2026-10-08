// OxwaldEngine ray tracing: payloads and push block of the RT pipeline path tracer (path_trace.rgen / .rchit / .rahit /
// .rmiss / path_shadow.rmiss). SBT layout (ox::render::rt::RtHitGroup × RtRayType): hit group = instance SBT offset
// (hitGroup × 2) + ray type (0 radiance, 1 shadow), stride 2; miss 0 = radiance, miss 1 = shadow.
#ifndef OX_RT_PATH_PAYLOAD_GLSL
#define OX_RT_PATH_PAYLOAD_GLSL

#extension GL_EXT_ray_tracing : require

#include "rt_common.glsl"

struct PtPayload {
    float t;
    uint instance;
    uint primitive;
    vec2 bary;
    uint hit;
};

OX_RENDER_PUSH(RtSceneBuffer rt; uint accum; uint sceneColor; uint width; uint height; uint frame; uint maxBounces;
               uint samples; uint reset;);

#endif
