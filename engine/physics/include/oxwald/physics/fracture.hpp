#pragma once

// Offline / load-time Voronoi fracture of a closed mesh into convex-ish chunks (destruction).
//
//   ox::physics::FractureMesh box = ox::physics::makeBoxFractureMesh({1.f, 0.5f, 0.25f});
//   ox::physics::FractureSettings s;
//   s.chunkCount = 16;
//   s.seed = 42;                                         // same mesh + settings + seed -> identical result
//   ox::physics::FractureResult r = ox::physics::voronoiFracture(box, s);
//   for (const auto& c : r.chunks) { /* c.positions/normals/uvs/indices relative to c.centroid, c.hullPoints */ }
//
// Algorithm: `chunkCount` seed points are sampled inside the mesh (deterministic generator, winding-number inside
// test); the Voronoi cell of every seed is obtained by clipping the source mesh with the bisector planes to every
// other seed. Each clip caps the cut with interior faces (planar UVs scaled by `interiorUvScale`, flagged as
// interior so they can use their own material). Cells of a convex source are convex; for a non-convex closed source
// the chunk geometry is exact but its collision hull is the convex hull of the chunk.
// Pure CPU code without Jolt; the result is plain data (serializeFracture / deserializeFracture for caching).

#include <oxwald/core/types.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <optional>
#include <span>
#include <vector>

namespace ox::physics {

// Closed triangle mesh (CCW = outward). `normals` / `uvs` are optional per-vertex attributes (empty: flat normals,
// box-projected UVs). Vertices of neighbouring triangles must share exact positions (welded), otherwise caps can't be
// built across the seam.
struct FractureMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<u32> indices;
};

struct FractureSettings {
    u32 chunkCount = 12;
    u64 seed = 1;
    f32 interiorUvScale = 1.f; // UV units per mesh unit on cut faces
    // Seeds closer than this fraction of the bounds diagonal to an existing seed are rejected (avoids slivers).
    f32 minSeedSpacing = 0.02f;
    // Thin sources (walls, slabs, beams): when the smallest bounds extent is below planarRatio x the middle one, the
    // seeds lie on the mid plane of that axis, so cells are prisms through the thickness. Pieces knocked out of a
    // wall can then leave it instead of being keyed into their neighbours by slanted faces. 0 = always volumetric.
    f32 planarRatio = 0.6f;
    // Optional seed concentration around a point (mesh space): fraction of seeds sampled within `focusRadius`.
    std::optional<glm::vec3> focus;
    f32 focusRadius = 0.25f;
    f32 focusFraction = 0.5f;
};

struct FractureChunk {
    // Render geometry relative to `centroid`. Indices [0, interiorIndexStart) are the original surface,
    // [interiorIndexStart, size) the cut (interior) faces.
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<u32> indices;
    u32 interiorIndexStart = 0;
    // Unique vertex positions relative to `centroid` (convex hull collision input).
    std::vector<glm::vec3> hullPoints;
    glm::vec3 centroid{0.f}; // centre of mass in source mesh space (uniform density)
    glm::vec3 boundsMin{0.f}; // source mesh space
    glm::vec3 boundsMax{0.f};
    glm::vec3 seed{0.f};
    f32 volume = 0.f;
};

// Two chunks sharing a cut face.
struct FractureLink {
    u32 a = 0;
    u32 b = 0;
    f32 area = 0.f; // shared face area
};

struct FractureResult {
    std::vector<FractureChunk> chunks;
    std::vector<FractureLink> links; // a < b, sorted
    f32 sourceVolume = 0.f;
    glm::vec3 sourceCentroid{0.f};
    glm::vec3 boundsMin{0.f};
    glm::vec3 boundsMax{0.f};
    u64 seed = 0;

    [[nodiscard]] bool empty() const { return chunks.empty(); }
    // Neighbour lists derived from `links`.
    [[nodiscard]] std::vector<std::vector<u32>> adjacency() const;
};

[[nodiscard]] FractureResult voronoiFracture(const FractureMesh& mesh, const FractureSettings& settings);

// ---- helpers --------------------------------------------------------------------------------------------------

// Axis-aligned box centred at the origin: 24 vertices (per-face normals, 0..1 UVs per face).
[[nodiscard]] FractureMesh makeBoxFractureMesh(const glm::vec3& halfExtents);
// Convex hull of `points` as a closed mesh (flat normals, box-projected UVs). Empty when the points are degenerate.
[[nodiscard]] FractureMesh makeConvexHullFractureMesh(std::span<const glm::vec3> points);
// Signed volume and centroid of a closed mesh (divergence theorem).
[[nodiscard]] f32 meshVolume(std::span<const glm::vec3> positions, std::span<const u32> indices);
[[nodiscard]] glm::vec3 meshCentroid(std::span<const glm::vec3> positions, std::span<const u32> indices);
// True when every edge (by exact position) is shared by exactly two triangles with opposite orientation.
[[nodiscard]] bool isClosedMesh(std::span<const glm::vec3> positions, std::span<const u32> indices);
// True when every point lies on the inner side of every triangle plane (within `tolerance`).
[[nodiscard]] bool isConvexMesh(std::span<const glm::vec3> positions, std::span<const u32> indices, f32 tolerance = 1e-4f);
// Generalised winding number test (closed meshes).
[[nodiscard]] bool pointInsideMesh(const glm::vec3& p, std::span<const glm::vec3> positions, std::span<const u32> indices);

// Stable 64-bit key of a mesh + settings (cache key / deterministic chunk asset ids).
[[nodiscard]] u64 fractureKey(const FractureMesh& mesh, const FractureSettings& settings);

// Compact binary form ("OXFR", versioned, CRC-checked). deserialize returns nullopt on corrupt data.
[[nodiscard]] std::vector<u8> serializeFracture(const FractureResult& result);
[[nodiscard]] std::optional<FractureResult> deserializeFracture(std::span<const u8> bytes);

} // namespace ox::physics
