#version 460
// Alpha-tested shadow casters.
#include <render/common/material.glsl>

OX_READONLY_BUFFER(MatrixBuffer, { mat4 m[]; });
OX_RENDER_DRAW_PUSH(MatrixBuffer matrices; uint layerBase; uint pad;);

layout(location = 0) in vec2 vUv0;
layout(location = 1) flat in uint vInstance;
layout(location = 2) in float vAlpha;

void main() {
    uint materialIndex = SCENE.instances.i[vInstance].materialIndex;
    Material m = SCENE.materials.m[materialIndex];
    if (oxMaterialAlpha(m, vUv0, vec4(1.0, 1.0, 1.0, vAlpha), 0.0) < m.alphaCutoff) discard;
}
