// UI overlay (ImGui + RmlUi): vertex pulling of ox::ui::UiVertex through buffer device addresses, index pulling
// (no index buffer binding), optional per-command transform (RmlUi `transform`), premultiplied-alpha output.
#ifndef OX_UI_COMMON_GLSL
#define OX_UI_COMMON_GLSL
#include <common/bindless.glsl>

struct UiVertex { vec2 pos; vec2 uv; uint color; };
OX_READONLY_BUFFER(UiVertices, { UiVertex v[]; });
OX_READONLY_BUFFER(UiIndices, { uint i[]; });
OX_READONLY_BUFFER(UiTransforms, { mat4 m[]; });

// Mirrors UiPush in engine/ui/src/ui_render_feature.cpp.
OX_PUSH_CONSTANTS({
    UiVertices vertices;
    UiIndices indices;
    UiTransforms transforms;
    vec4 xform;      // frame pixel = pos * xform.xy + xform.zw
    vec2 ndcScale;   // 2 / frame size
    uint firstIndex;
    int vertexOffset;
    int transform;   // -1 = none
    uint texture;    // bindless sampled index
    uint flags;      // bit 0: premultiplied input
    uint pad;
});

const uint OX_UI_PREMULTIPLIED = 1u;
#endif
