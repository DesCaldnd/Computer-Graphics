#version 460
// SelectionMask (render resolution, R8): 1 where EntityID belongs to the selection (sorted id list).
#include <common/bindless.glsl>
OX_READONLY_BUFFER(IdList, { uint ids[]; });
OX_PUSH_CONSTANTS({ IdList selection; uint count; uint entityTex; });
layout(location = 0) in vec2 uv;
layout(location = 0) out float outMask;
void main() {
    uint id = texelFetch(oxUTextures2D[nonuniformEXT(pc.entityTex)], ivec2(gl_FragCoord.xy), 0).r;
    outMask = 0.0;
    if (id == 0u) return;
    uint lo = 0u, hi = pc.count;
    while (lo < hi) {
        uint mid = (lo + hi) / 2u;
        uint v = pc.selection.ids[mid];
        if (v == id) { outMask = 1.0; return; }
        if (v < id) lo = mid + 1u; else hi = mid;
    }
}
