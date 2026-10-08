#version 460
// Path tracer radiance miss (miss index 0): the integrator evaluates the sky itself.
#include "path_payload.glsl"

layout(location = 0) rayPayloadInEXT PtPayload payload;

void main() { payload.hit = 0u; }
