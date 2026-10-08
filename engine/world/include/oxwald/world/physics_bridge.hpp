#pragma once

// Header-only adapter from world collider data to ox::physics shapes. The world library itself does
// not link physics — include this only from code that links Oxwald::physics (gameplay/integration).

#include <oxwald/physics/shape.hpp>
#include <oxwald/world/terrain_lod.hpp>
#include <oxwald/world/vegetation.hpp>

namespace ox::world {

static_assert(kPhysicsHeightHole == ox::physics::kHeightFieldHole, "hole marker must match physics");

inline ox::physics::ShapeDesc toShapeDesc(const PhysicsHeightfieldTile& t) {
    return ox::physics::ShapeDesc::heightField(t.heights, t.sampleCount, t.offset, t.scale);
}

// Collider shape in the instance's local space (body position/rotation = VegetationCollider pose).
inline ox::physics::ShapeDesc toShapeDesc(const VegetationCollider& c) {
    switch (c.shape) {
    case VegetationColliderShape::Capsule: return ox::physics::ShapeDesc::capsule(c.halfHeight, c.radius);
    case VegetationColliderShape::Cylinder: return ox::physics::ShapeDesc::cylinder(c.halfHeight, c.radius);
    case VegetationColliderShape::Box: return ox::physics::ShapeDesc::box({c.radius, c.halfHeight, c.radius});
    }
    return ox::physics::ShapeDesc::capsule(c.halfHeight, c.radius);
}

} // namespace ox::world
