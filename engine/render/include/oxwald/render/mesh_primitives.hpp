#pragma once

// Procedural meshes in the assets::MeshData layout (positions + 48-byte attributes + u32 indices, one submesh,
// tangents with handedness). Used by tests, the editor (Create → Shape) and as placeholders.

#include <oxwald/assets/mesh.hpp>
#include <oxwald/core/uuid.hpp>

namespace ox::render {

enum class Primitive : u8 { Cube, Sphere, Plane, Cylinder, Capsule, Cone, Torus, Count };
const char* primitiveName(Primitive p);

struct PrimitiveParams {
    f32 size = 1.0f;         // cube edge, sphere/cylinder/cone/capsule diameter, plane edge, torus outer diameter
    f32 height = 1.0f;       // cylinder, cone, capsule (total)
    f32 minorRadius = 0.125f; // torus tube
    u32 segments = 32;
    u32 rings = 16;
    f32 uvScale = 1.0f;
};

[[nodiscard]] assets::MeshData makePrimitive(Primitive p, const PrimitiveParams& params = {});

// Stable asset ids of the built-in primitives (registered by GpuResourceCache at startup with default params).
[[nodiscard]] Uuid primitiveUuid(Primitive p);

// Recomputes the bounding box / sphere of the mesh and of its submeshes from the positions.
void computeBounds(assets::MeshData& mesh);
// Generates per-vertex tangents (MikkTSpace-like accumulation) from positions/normals/uv0.
void computeTangents(assets::MeshData& mesh);

} // namespace ox::render
