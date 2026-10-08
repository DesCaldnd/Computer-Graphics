#version 460
// Path tracer shadow miss (miss index 1): nothing in the way.
#include "path_payload.glsl"

layout(location = 1) rayPayloadInEXT uint shadowVisible;

void main() { shadowVisible = 1u; }
