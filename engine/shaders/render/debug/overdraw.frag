#version 460
// Overdraw: every rasterised fragment adds 1 (additive blending, no depth test).
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(1.0, 0.0, 0.0, 0.0); }
