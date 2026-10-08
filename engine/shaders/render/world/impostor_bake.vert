#version 460
// Impostor bake: renders a prototype mesh (scene arenas, gl_VertexIndex includes the base vertex) into one frame of
// the atlas with an orthographic matrix looking at the object along a hemi-octahedral direction.
#include <render/common/scene.glsl>

layout(push_constant, scalar) uniform OxPushConstants {
    SceneBuffer scene;
    mat4 viewProj;
    uint material;
    uint pad0;
} pc;

layout(location = 0) out vec2 vUv0;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vColor;

void main() {
    vec3 p = SCENE.positions.p[gl_VertexIndex];
    VertexAttributes a = SCENE.attributes.a[gl_VertexIndex];
    vUv0 = a.uv0;
    vNormal = a.normal;
    vColor = oxUnpackColor(a.color);
    gl_Position = pc.viewProj * vec4(p, 1.0);
}
