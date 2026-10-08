#pragma once

// CPU mesh data in the exact layout the renderer uploads (vertex pulling, see docs/dev/modules/assets.md).
//
//   positions          vec3 f32                       12 B / vertex  (depth, shadows, culling, meshlets)
//   attributes         VertexAttributes               48 B / vertex  (normal, tangent+sign, uv0, uv1, color)
//   skin (optional)    SkinVertex                     16 B / vertex  (4 × u16 joints, 4 × unorm16 weights)
//   indices            u32, absolute (already include the submesh vertex offset), triangle lists
//   meshlets           Meshlet                        64 B           (bounds + cone + ranges)
//   meshletVertices    u32, absolute vertex indices   (meshlet.vertexOffset .. +vertexCount)
//   meshletTriangles   u8 × 3 per triangle, local     (meshlet.triangleOffset .. +3·triangleCount, padded to 4)
//
// Every submesh owns a contiguous vertex range and one MeshLod per LOD level (LOD 0 = full detail); all LODs
// share the vertex streams, only indices/meshlets differ.

#include <oxwald/assets/asset_types.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/core/result.hpp>

#include <span>
#include <string>
#include <vector>

namespace ox::assets {

struct VertexAttributes {
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f}; // xyz tangent, w = bitangent sign (B = w · cross(N, T))
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    u32 color = 0xffffffffu; // RGBA8 unorm, R in the lowest byte, linear
};
static_assert(sizeof(VertexAttributes) == 48);

struct SkinVertex {
    u16 joints[4] = {0, 0, 0, 0};
    u16 weights[4] = {65535, 0, 0, 0}; // unorm16, sum = 65535
};
static_assert(sizeof(SkinVertex) == 16);

struct Meshlet {
    glm::vec3 center{0.0f}; // bounding sphere
    f32 radius = 0.0f;
    glm::vec3 coneAxis{0.0f, 0.0f, 1.0f}; // backface cone: reject if dot(normalize(center - camPos), axis) >= cutoff
    f32 coneCutoff = 1.0f;                // (1 = cone test disabled)
    glm::vec3 coneApex{0.0f};
    u32 submesh = 0;
    u32 vertexOffset = 0;   // into meshletVertices
    u32 triangleOffset = 0; // byte offset into meshletTriangles
    u32 vertexCount = 0;    // <= 64 (MeshProcessSettings::meshletMaxVertices)
    u32 triangleCount = 0;  // <= 124
};
static_assert(sizeof(Meshlet) == 64);

struct MeshLod {
    u32 indexOffset = 0;
    u32 indexCount = 0;
    u32 meshletOffset = 0;
    u32 meshletCount = 0;
    f32 error = 0.0f; // simplification error in mesh units (0 for LOD 0) — for screen-space LOD selection
};

struct Submesh {
    std::string name;
    u32 materialSlot = 0;
    u32 vertexOffset = 0;
    u32 vertexCount = 0;
    AABB bounds;
    std::vector<MeshLod> lods; // lods[0] always present
};

struct MaterialSlot {
    std::string name;
    Uuid material; // nil = default material
};

// Physics-ready collision geometry (meters, mesh space).
struct MeshCollisionData {
    std::vector<glm::vec3> hullVertices; // convex hull points
    std::vector<u32> hullIndices;        // hull triangles (CCW, outward)
    std::vector<glm::vec3> vertices;     // simplified, welded triangle mesh
    std::vector<u32> indices;
    [[nodiscard]] bool empty() const { return hullVertices.empty() && vertices.empty(); }
};

struct MeshData {
    static constexpr AssetType kAssetType = AssetType::Mesh;

    std::string name;
    std::vector<glm::vec3> positions;
    std::vector<VertexAttributes> attributes;
    std::vector<SkinVertex> skin;
    std::vector<u32> indices;
    std::vector<Meshlet> meshlets;
    std::vector<u32> meshletVertices;
    std::vector<u8> meshletTriangles;
    std::vector<Submesh> submeshes;
    std::vector<MaterialSlot> materials;
    AABB bounds;
    Sphere boundingSphere;
    MeshCollisionData collision;
    Uuid skeleton; // Skeleton asset for skinned meshes

    [[nodiscard]] u32 vertexCount() const { return static_cast<u32>(positions.size()); }
    [[nodiscard]] u32 lodCount() const;
    [[nodiscard]] u64 triangleCount(u32 lod = 0) const;
    [[nodiscard]] bool skinned() const { return !skin.empty(); }
    [[nodiscard]] usize memoryUsage() const;
};

[[nodiscard]] std::vector<std::byte> serializeMesh(const MeshData& mesh);
[[nodiscard]] Result<MeshData> deserializeMesh(std::span<const std::byte> data);

// Axis-aligned cube centred at the origin, 24 vertices / 12 triangles, one submesh, tangents, meshlets.
[[nodiscard]] MeshData makeCubeMesh(f32 halfExtent = 0.5f);

[[nodiscard]] inline u32 packColor(glm::vec4 c) {
    auto q = [](f32 v) { return static_cast<u32>(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (q(c.a) << 24);
}
[[nodiscard]] inline glm::vec4 unpackColor(u32 c) {
    return glm::vec4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, (c >> 24) & 0xff) / 255.0f;
}

} // namespace ox::assets
