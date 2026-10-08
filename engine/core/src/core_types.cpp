#include <oxwald/core/math.hpp>
#include <oxwald/core/reflect.hpp>

namespace ox::reflect {

void registerCoreReflection() {
    OX_REFLECT_TYPE(Transform, "ox.Transform")
        .field("position", &Transform::position)
        .field("rotation", &Transform::rotation)
        .field("scale", &Transform::scale);
    OX_REFLECT_TYPE(AABB, "ox.AABB").field("min", &AABB::min).field("max", &AABB::max);
    OX_REFLECT_TYPE(Sphere, "ox.Sphere").field("center", &Sphere::center).field("radius", &Sphere::radius);
}

} // namespace ox::reflect
