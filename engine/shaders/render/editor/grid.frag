#version 460
// Editor ground grid (y = 0), anti-aliased lines with distance fade, X (red) / Z (blue) axes. Drawn in the Overlay
// stage (output resolution, display-encoded colour) with a manual depth test against the render-resolution Depth.
#include <render/common/view.glsl>
OX_PUSH_CONSTANTS({ ViewBuffer view; uint depth; float cellSize; float fadeDistance; float opacity; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

float gridLine(vec2 coord, float lineWidth) {
    vec2 d = fwidth(coord);
    vec2 g = abs(fract(coord - 0.5) - 0.5) / max(d, vec2(1e-6));
    return 1.0 - min(min(g.x, g.y) / lineWidth, 1.0);
}

void main() {
    ViewBuffer vb = pc.view;
    vec3 cam = vb.v.cameraPosition.xyz;
    mat4 ivp = vb.v.invViewProj;
    vec4 n = ivp * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec4 f = ivp * vec4(uv * 2.0 - 1.0, 0.0001, 1.0);
    vec3 p0 = n.xyz / n.w;
    vec3 dir = normalize(f.xyz / f.w - p0);
    if (abs(dir.y) < 1e-6) discard;
    float t = -p0.y / dir.y;
    if (t <= 0.0) discard;
    vec3 P = p0 + dir * t;
    vec4 clip = vb.v.viewProj * vec4(P, 1.0);
    float gridDepth = clip.z / clip.w;
    float sceneDepth = textureLod(sampler2D(OX_TEX2D(pc.depth), OX_SAMPLER(OX_SAMPLER_NEAREST_CLAMP)), uv, 0.0).r;
    if (sceneDepth > gridDepth * 1.0005 + 1e-7) discard;
    vec2 c = P.xz / pc.cellSize;
    float minor = gridLine(c, 1.0) * 0.35;
    float major = gridLine(c / 10.0, 1.5) * 0.7;
    float a = max(minor, major);
    vec3 color = vec3(0.55);
    vec2 dAxis = fwidth(P.xz);
    if (abs(P.z) < dAxis.y * 1.5) { color = vec3(0.9, 0.25, 0.25); a = 0.9; }
    if (abs(P.x) < dAxis.x * 1.5) { color = vec3(0.25, 0.45, 0.95); a = 0.9; }
    float dist = length(P - cam);
    a *= clamp(1.0 - dist / pc.fadeDistance, 0.0, 1.0) * pc.opacity;
    if (a <= 0.002) discard;
    outColor = vec4(color, a);
}
