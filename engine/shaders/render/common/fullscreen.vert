#version 460
// Fullscreen triangle. uv (0,0) = top-left; depth 0 (= far plane with reversed-Z).
layout(location = 0) out vec2 outUv;
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    outUv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
