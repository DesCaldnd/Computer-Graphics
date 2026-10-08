#pragma once

#include <oxwald/physics/types.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::physics {

enum class ShapeType : u8 {
    Box,
    Sphere,
    Capsule,      // along local Y; halfHeight = half height of the cylindrical part
    Cylinder,     // along local Y
    ConvexHull,   // from points
    TriangleMesh, // static/kinematic only
    HeightField,  // static only (terrain)
    Compound,
    // Only reported by ShapeRef::type() for decorated/foreign shapes:
    Scaled,
    OffsetCenterOfMass,
    RotatedTranslated,
    Other,
};

struct CompoundChild;

// Plain, hashable shape description. Build with the static helpers or fill the fields directly.
struct ShapeDesc {
    ShapeType type = ShapeType::Box;

    glm::vec3 halfExtents{0.5f}; // Box
    f32 radius = 0.5f;           // Sphere, Capsule, Cylinder
    f32 halfHeight = 0.5f;       // Capsule (cylinder part), Cylinder
    f32 convexRadius = -1.f;     // < 0 = automatic (Jolt default, clamped to the shape size)
    f32 density = 1000.f;        // kg/m^3, used when the body does not override its mass

    std::vector<glm::vec3> points; // ConvexHull points / TriangleMesh vertices
    std::vector<u32> indices;      // TriangleMesh, 3 per triangle (CCW = front face)

    // HeightField: sampleCount*sampleCount heights, row-major (z rows, x columns).
    // World position of sample (x, z) = offset + scale * (x, heights[z * sampleCount + x], z).
    // Use kHeightFieldHole for holes.
    std::vector<f32> heights;
    u32 sampleCount = 0;
    glm::vec3 heightFieldOffset{0.f};
    glm::vec3 heightFieldScale{1.f};

    std::vector<CompoundChild> children; // Compound

    // Decorations applied on top of any type (Jolt ScaledShape / OffsetCenterOfMassShape).
    glm::vec3 scale{1.f};
    glm::vec3 centerOfMassOffset{0.f};

    static ShapeDesc box(glm::vec3 halfExtents);
    static ShapeDesc sphere(f32 radius);
    static ShapeDesc capsule(f32 halfHeight, f32 radius);
    static ShapeDesc cylinder(f32 halfHeight, f32 radius);
    static ShapeDesc convexHull(std::vector<glm::vec3> points);
    static ShapeDesc triangleMesh(std::vector<glm::vec3> vertices, std::vector<u32> indices);
    static ShapeDesc heightField(std::vector<f32> heights, u32 sampleCount, glm::vec3 offset, glm::vec3 scale);
    static ShapeDesc compound(std::vector<CompoundChild> children);

    bool operator==(const ShapeDesc& other) const;
};

struct CompoundChild {
    ShapeDesc shape;
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    bool operator==(const CompoundChild& other) const;
};

inline constexpr f32 kHeightFieldHole = 3.402823466e+38f; // == JPH::HeightFieldShapeConstants::cNoCollisionValue

// Intrusively ref-counted reference to an immutable Jolt shape. Cheap to copy, thread-safe
// ref counting, can be shared by any number of bodies (and worlds).
class ShapeRef {
public:
    ShapeRef() = default;
    ShapeRef(const ShapeRef& other);
    ShapeRef(ShapeRef&& other) noexcept;
    ShapeRef& operator=(const ShapeRef& other);
    ShapeRef& operator=(ShapeRef&& other) noexcept;
    ~ShapeRef();

    [[nodiscard]] bool valid() const { return m_shape != nullptr; }
    explicit operator bool() const { return valid(); }
    bool operator==(const ShapeRef& other) const { return m_shape == other.m_shape; }

    [[nodiscard]] ShapeType type() const;
    [[nodiscard]] glm::vec3 centerOfMass() const; // relative to the shape origin
    [[nodiscard]] Aabb localBounds() const;
    [[nodiscard]] f32 volume() const;
    [[nodiscard]] f32 mass() const; // from density; 0 for mesh/height field
    [[nodiscard]] u32 refCount() const;

    // Escape hatch: `const JPH::Shape*`. Adopting adds a reference.
    [[nodiscard]] const void* native() const { return m_shape; }
    static ShapeRef fromNative(const void* joltShape);

private:
    const void* m_shape = nullptr;
};

// Builds a shape without caching. Returns an invalid ref (and fills `error`) on failure.
ShapeRef createShape(const ShapeDesc& desc, std::string* error = nullptr);

// Wraps existing shapes.
ShapeRef makeScaled(const ShapeRef& shape, glm::vec3 scale);
ShapeRef makeRotatedTranslated(const ShapeRef& shape, glm::vec3 position, glm::quat rotation);
ShapeRef makeOffsetCenterOfMass(const ShapeRef& shape, glm::vec3 offset);

// Stable 64-bit hash of every field of the description (used by ShapeCache).
u64 hashShapeDesc(const ShapeDesc& desc);

// Deduplicates shapes by description: identical descriptions return the same ShapeRef.
// Thread-safe.
class ShapeCache {
public:
    ShapeRef getOrCreate(const ShapeDesc& desc, std::string* error = nullptr);
    // Drops entries only referenced by the cache.
    usize collectGarbage();
    void clear();
    [[nodiscard]] usize size() const;

private:
    struct Entry {
        ShapeDesc desc;
        ShapeRef shape;
    };
    mutable std::mutex m_mutex;
    std::unordered_multimap<u64, Entry> m_entries;
};

} // namespace ox::physics
