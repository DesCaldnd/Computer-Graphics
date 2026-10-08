// OxwaldEngine GPU-driven rendering: shared structures of the culling passes (mirrors gpu_driven.cpp, scalar layout).
#ifndef OX_GPU_DRIVEN_CULL_COMMON_GLSL
#define OX_GPU_DRIVEN_CULL_COMMON_GLSL

#include <render/common/scene.glsl>

struct MeshLod {
    uint firstIndex;
    uint indexCount;
    float error;
    uint meshletFirst;       // u32 offset of the first Meshlet header (16 words) in the meshlet arena
    uint meshletCount;
    uint meshletVertexBase;  // u32 offset of meshletVertices
    uint meshletTriangleBase; // u32 offset of the packed u8 triangle indices
    uint pad;
};
const uint OX_MAX_MESH_LODS = 8u;
OX_READONLY_BUFFER(MeshLodBuffer, { MeshLod l[]; });

struct DrawCommand { // VkDrawIndexedIndirectCommand
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
OX_BUFFER(DrawCommandBuffer, { DrawCommand c[]; });
OX_BUFFER(UintBuffer, { uint v[]; });
OX_READONLY_BUFFER(CullItemBuffer, { uvec2 i[]; }); // x = instance, y = batch

struct CullBatch {
    uint firstCommand; // lodCount consecutive commands (one per LOD)
    uint idBase;       // first instance-id slot (capacity slots shared by the LODs)
    uint lodCount;
    uint meshIndex;
    uint capacity;
    uint run;          // DrawIndirectRun index (compaction)
    uint runFirst;     // first command of the run
    uint pad;
};
OX_READONLY_BUFFER(CullBatchBuffer, { CullBatch b[]; });

const uint OX_CULL_FRUSTUM = 1u;
const uint OX_CULL_SPHERE = 2u;
const uint OX_CULL_EARLY = 4u;     // two-phase occlusion, phase 1: emit only instances visible last frame
const uint OX_CULL_LATE = 8u;      // phase 2: HiZ test, emit newly visible, update visibility
const uint OX_CULL_COMPACT = 16u;  // also write compacted commands + per-run counts
const uint OX_CULL_DISTANCE = 32u;
const uint OX_CULL_MESHLETS = 64u; // instances with meshlets go to the meshlet list instead of instanced draws
const uint OX_CULL_RESET_VISIBILITY = 128u;

struct CullJob {
    vec4 planes[6];
    vec4 sphere;        // xyz centre, w radius
    vec4 lodCamera;     // xyz camera position, w = pixels per unit at distance 1 (0 = LOD 0)
    float lodThreshold; // pixels
    float drawDistance;
    uint flags;
    uint multiplier;    // instances per culled instance (layered point shadows: 6)
    mat4 viewProj;      // occlusion test
    CullItemBuffer items;
    CullBatchBuffer batches;
    DrawCommandBuffer commands;
    UintBuffer ids;
    UintBuffer scratch;    // uvec2 per item: (command, slot) or ~0
    UintBuffer visibility; // per item: drawn last frame (two-phase occlusion)
    UintBuffer counters;
    DrawCommandBuffer compacted;
    UintBuffer runCounts;
    UintBuffer meshletInstances; // [0] = count, then (instance, lod) pairs (meshlet path)
    uint itemCount;
    uint batchCount;
    uint hiz;      // sampled index of the HiZ pyramid (late phase)
    uint hizMips;
    vec2 hizSize;
    uint lodOrtho;
    uint meshletCapacity;
    uint runCount;
    uint pad;
};
OX_READONLY_BUFFER(CullJobBuffer, { CullJob j; });

// Counter slots (u32, per kind: main view = 0, shadow/custom = 16).
const uint OX_COUNTER_TESTED = 0u;
const uint OX_COUNTER_FRUSTUM_CULLED = 1u;
const uint OX_COUNTER_OCCLUDED = 2u;
const uint OX_COUNTER_VISIBLE = 3u;
const uint OX_COUNTER_TRIANGLES = 4u;
const uint OX_COUNTER_DRAWS = 5u;
const uint OX_COUNTER_MESHLETS_TESTED = 6u;
const uint OX_COUNTER_MESHLETS_VISIBLE = 7u;
const uint OX_COUNTER_MESHLET_TRIANGLES = 8u;
const uint OX_COUNTER_LOD_SUM = 9u;

// Mirror of ox::render::selectLod (gpu_types.hpp).
uint oxSelectLod(MeshLodBuffer lods, uint meshIndex, uint lodCount, float scale, float dist, vec4 lodCamera,
                 float threshold, bool ortho) {
    if (lodCamera.w <= 0.0 || lodCount <= 1u) return 0u;
    float perUnit = ortho ? lodCamera.w : lodCamera.w / max(dist, 1e-3);
    uint lod = 0u;
    for (uint l = 1u; l < lodCount && l < OX_MAX_MESH_LODS; ++l) {
        MeshLod m = lods.l[meshIndex * OX_MAX_MESH_LODS + l];
        if (m.indexCount == 0u || m.error * scale * perUnit > threshold) break;
        lod = l;
    }
    return lod;
}

// Conservative HiZ occlusion test of a world-space sphere (reversed-Z, HiZ = farthest depth per texel).
// hizSize = full depth resolution; the pyramid starts at half resolution (gpu_driven/hiz_early.comp): level L of the
// full-resolution pyramid is mip L-1, hizMips counts the stored mips.
bool oxOccludedByHiZ(mat4 viewProj, vec3 c, float r, uint hiz, uint hizMips, vec2 hizSize) {
    vec2 mn = vec2(1.0), mx = vec2(0.0);
    float closest = 0.0;
    for (uint k = 0u; k < 8u; ++k) {
        vec3 corner = c + r * vec3((k & 1u) != 0u ? 1.0 : -1.0, (k & 2u) != 0u ? 1.0 : -1.0, (k & 4u) != 0u ? 1.0 : -1.0);
        vec4 clip = viewProj * vec4(corner, 1.0);
        if (clip.w <= 1e-4) return false; // crosses the camera plane
        vec3 ndc = clip.xyz / clip.w;
        vec2 uv = ndc.xy * 0.5 + 0.5;
        mn = min(mn, uv);
        mx = max(mx, uv);
        closest = max(closest, ndc.z);
    }
    if (closest >= 1.0) return false; // reaches the near plane
    mn = clamp(mn, vec2(0.0), vec2(1.0));
    mx = clamp(mx, vec2(0.0), vec2(1.0));
    vec2 pmin = mn * hizSize, pmax = mx * hizSize;
    vec2 size = max(pmax - pmin, vec2(1.0));
    int level = clamp(int(ceil(log2(max(size.x, size.y)))), 1, int(hizMips)); // pyramid level (≥ 1)
    ivec2 p0, p1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        ivec2 levelSize = max(ivec2(hizSize) >> level, ivec2(1));
        p0 = clamp(ivec2(pmin) >> level, ivec2(0), levelSize - 1);
        p1 = clamp(ivec2(pmax) >> level, ivec2(0), levelSize - 1);
        if (all(lessThanEqual(p1 - p0, ivec2(1))) || level >= int(hizMips)) break;
        level += 1;
    }
    float farthest = 1.0;
    for (int y = p0.y; y <= min(p1.y, p0.y + 1); ++y)
        for (int x = p0.x; x <= min(p1.x, p0.x + 1); ++x) farthest = min(farthest, OX_FETCH_2D(hiz, ivec2(x, y), level - 1).r);
    // Wider than 2×2 even at the last level: be conservative.
    if (any(greaterThan(p1 - p0, ivec2(1)))) return false;
    return closest < farthest;
}

#endif
