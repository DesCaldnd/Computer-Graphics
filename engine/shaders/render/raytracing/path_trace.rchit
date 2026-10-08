#version 460
// Path tracer closest hit: reports the hit; shading happens in the ray generation shader (path_common.glsl).
#include "path_payload.glsl"

layout(location = 0) rayPayloadInEXT PtPayload payload;
hitAttributeEXT vec2 attribs;

void main() {
    payload.t = gl_HitTEXT;
    payload.instance = gl_InstanceCustomIndexEXT;
    payload.primitive = gl_PrimitiveID;
    payload.bary = attribs;
    payload.hit = 1u;
}
