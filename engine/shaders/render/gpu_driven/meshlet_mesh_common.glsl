// Shared declarations of the VK_EXT_mesh_shader meshlet path (meshlet.task / meshlet.mesh). Gated by
// DeviceCaps::meshShader (+ taskShader) and r.GpuDriven.MeshShaders; compile-tested everywhere.
#ifndef OX_GPU_DRIVEN_MESHLET_MESH_COMMON_GLSL
#define OX_GPU_DRIVEN_MESHLET_MESH_COMMON_GLSL

#extension GL_EXT_mesh_shader : require
#include "cull_common.glsl"

// Visible meshlets of one instance (one task workgroup per meshlet instance; instances with more visible meshlets
// than this are truncated, see docs/dev/perf.md).
const uint OX_TASK_MAX_MESHLETS = 1024u;

struct OxTaskPayload {
    uint instanceId;
    uint lod;
    uint meshlets[OX_TASK_MAX_MESHLETS];
};

// DrawPush prefix (view, scene, drawIds, inputs[6]) shared with the fragment shaders, then the meshlet data.
// drawIds = meshlet instance list written by cull.comp ([0] = count, then (instance, lod) pairs). One draw per
// pipeline variant: task workgroups of instances with another variant emit nothing.
OX_RENDER_DRAW_PUSH(uint inputs[6]; MeshLodBuffer lods; uint hiz; uint hizMips; vec2 hizSize; uint late; uint variant;);

struct OxMeshletHeader {
    vec3 center;
    float radius;
    vec3 coneAxis;
    float coneCutoff;
    vec3 coneApex;
    uint submesh;
    uint vertexOffset;
    uint triangleOffset;
    uint vertexCount;
    uint triangleCount;
};

OxMeshletHeader oxLoadMeshletHeader(uint word) {
    MeshletBuffer m = SCENE.meshlets;
    OxMeshletHeader h;
    h.center = uintBitsToFloat(uvec3(m.data[word], m.data[word + 1u], m.data[word + 2u]));
    h.radius = uintBitsToFloat(m.data[word + 3u]);
    h.coneAxis = uintBitsToFloat(uvec3(m.data[word + 4u], m.data[word + 5u], m.data[word + 6u]));
    h.coneCutoff = uintBitsToFloat(m.data[word + 7u]);
    h.coneApex = uintBitsToFloat(uvec3(m.data[word + 8u], m.data[word + 9u], m.data[word + 10u]));
    h.submesh = m.data[word + 11u];
    h.vertexOffset = m.data[word + 12u];
    h.triangleOffset = m.data[word + 13u];
    h.vertexCount = m.data[word + 14u];
    h.triangleCount = m.data[word + 15u];
    return h;
}

#endif
