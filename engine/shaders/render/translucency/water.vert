#version 460
// Water surface: dense grid (rectangle, or camera-centred with cells growing with distance when unbounded)
// displaced by the Gerstner waves of engine/shaders/world/gerstner.glsl (same math as the CPU buoyancy).
#include "water_push.glsl"

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vRest;  // undisplaced XZ (rest point x0)
layout(location = 2) out float vCrest; // 0..1 height above the base relative to the amplitude sum

void main() {
    uint cells = pc.gridCells;
    uint quad = uint(gl_VertexIndex) / 6u;
    uint corner = uint(gl_VertexIndex) % 6u;
    const uvec2 kCorner[6] = uvec2[](uvec2(0, 0), uvec2(0, 1), uvec2(1, 1), uvec2(0, 0), uvec2(1, 1), uvec2(1, 0));
    uvec2 cell = uvec2(quad % cells, quad / cells) + kCorner[corner];
    vec2 t = vec2(cell) / float(cells) * 2.0 - 1.0; // -1..1
    // Camera-centred grid, cells growing with the distance (s(t) = t·(α + (1-α)|t|): near cells = grid.w metres);
    // bounded water clamps the grid to its rectangle (border vertices collapse onto the edge).
    float alpha = WATER.grid.w;
    vec2 s = t * (alpha + (1.0 - alpha) * abs(t));
    vec2 x0 = WATER.grid.xy + s * WATER.grid.z;
    vec4 r = WATER.rect;
    if ((WATER.flags & 1u) == 0u) x0 = clamp(x0, r.xy - r.zw, r.xy + r.zw);
    vec3 d = oxWaterDisplacement(pc.water, x0, oxWaterTime(pc.water));
    vec3 p = vec3(x0.x, oxWaterBase(pc.water), x0.y) + d;
    vWorldPos = p;
    vRest = x0;
    vCrest = WATER.detail.w > 0.0 ? clamp(d.y / WATER.detail.w, 0.0, 1.0) : 0.0;
    gl_Position = VIEW.viewProj * vec4(p, 1.0);
}
