#pragma once

#include <oxwald/assets/mesh.hpp>

#include <span>
#include <vector>

namespace ox::assets {

struct MeshProcessSettings {
    bool optimizeVertexCache = true; // meshoptimizer vertex cache + overdraw-neutral fetch reorder
    bool generateLods = true;
    std::vector<f32> lodRatios{0.5f, 0.25f, 0.125f}; // target index ratio per extra LOD
    f32 lodMaxError = 0.05f; // relative to the mesh extent (meshopt_simplify target_error)
    bool generateMeshlets = true;
    u32 meshletMaxVertices = 64;
    u32 meshletMaxTriangles = 124;
    f32 meshletConeWeight = 0.25f;
    bool generateCollision = false;
    f32 collisionSimplifyRatio = 0.25f;
    u32 maxHullVertices = 64;
};

// Area/angle-weighted per-vertex tangents with Gram–Schmidt orthogonalisation and handedness in w (same inputs
// and conventions as MikkTSpace: UV0, V down-is-up as stored, B = w·cross(N,T)). Vertices on UV seams must be
// split already (they are after import).
void generateTangents(std::span<const glm::vec3> positions, std::span<VertexAttributes> attributes,
                      std::span<const u32> indices);
// Angle-weighted smooth normals.
void generateNormals(std::span<const glm::vec3> positions, std::span<VertexAttributes> attributes,
                     std::span<const u32> indices);

// Full pipeline on a mesh whose submeshes have lods[0] filled (index ranges): optimisation, LODs, meshlets,
// bounds and optional collision data. Existing LODs > 0 and meshlets are rebuilt.
void processMesh(MeshData& mesh, const MeshProcessSettings& settings);

void optimizeMesh(MeshData& mesh);
void generateLods(MeshData& mesh, const MeshProcessSettings& settings);
void buildMeshlets(MeshData& mesh, const MeshProcessSettings& settings);
void computeBounds(MeshData& mesh);
[[nodiscard]] MeshCollisionData buildCollisionData(const MeshData& mesh, const MeshProcessSettings& settings);

struct ConvexHull {
    std::vector<glm::vec3> vertices;
    std::vector<u32> indices; // triangles, CCW seen from outside
};
// Incremental 3D hull. When the hull has more than maxVertices points, it is rebuilt from the support points
// of maxVertices well-distributed directions (a conservative-ish approximation used for physics).
[[nodiscard]] ConvexHull computeConvexHull(std::span<const glm::vec3> points, u32 maxVertices = 64);

} // namespace ox::assets
