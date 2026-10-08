#include "jolt_common.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/physics/shape.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace ox::physics {
namespace {

using detail::toGlm;
using detail::toJolt;

const JPH::Shape* asJolt(const void* p) { return static_cast<const JPH::Shape*>(p); }

// --- Hashing (FNV-1a 64) ----------------------------------------------------------------------
struct Hasher {
    u64 h = 1469598103934665603ull;
    void bytes(const void* data, usize size) {
        const auto* p = static_cast<const u8*>(data);
        for (usize i = 0; i < size; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    void f(f32 v) {
        if (v == 0.f) {
            v = 0.f; // fold -0 into +0
        }
        u32 bits = std::bit_cast<u32>(v);
        bytes(&bits, sizeof(bits));
    }
    void u(u64 v) { bytes(&v, sizeof(v)); }
    void v3(const glm::vec3& v) {
        f(v.x);
        f(v.y);
        f(v.z);
    }
    void q(const glm::quat& v) {
        f(v.x);
        f(v.y);
        f(v.z);
        f(v.w);
    }
};

void hashInto(Hasher& h, const ShapeDesc& d) {
    h.u(static_cast<u64>(d.type));
    h.v3(d.scale);
    h.v3(d.centerOfMassOffset);
    h.f(d.density);
    h.f(d.convexRadius);
    switch (d.type) {
    case ShapeType::Box: h.v3(d.halfExtents); break;
    case ShapeType::Sphere: h.f(d.radius); break;
    case ShapeType::Capsule:
    case ShapeType::Cylinder:
        h.f(d.radius);
        h.f(d.halfHeight);
        break;
    case ShapeType::ConvexHull:
    case ShapeType::TriangleMesh:
        h.u(d.points.size());
        for (const auto& p : d.points) {
            h.v3(p);
        }
        h.u(d.indices.size());
        if (!d.indices.empty()) {
            h.bytes(d.indices.data(), d.indices.size() * sizeof(u32));
        }
        break;
    case ShapeType::HeightField:
        h.u(d.sampleCount);
        h.v3(d.heightFieldOffset);
        h.v3(d.heightFieldScale);
        h.u(d.heights.size());
        for (f32 v : d.heights) {
            h.f(v);
        }
        break;
    case ShapeType::Compound:
        h.u(d.children.size());
        for (const auto& c : d.children) {
            hashInto(h, c.shape);
            h.v3(c.position);
            h.q(c.rotation);
        }
        break;
    default: break;
    }
}

f32 convexRadiusFor(const ShapeDesc& d, f32 maxAllowed) {
    f32 r = d.convexRadius < 0.f ? JPH::cDefaultConvexRadius : d.convexRadius;
    return std::clamp(r, 0.f, std::max(0.f, maxAllowed));
}

bool fail(std::string* error, std::string message) {
    OX_LOG_ERROR("physics", "createShape: {}", message);
    if (error) {
        *error = std::move(message);
    }
    return false;
}

JPH::RefConst<JPH::Shape> build(const ShapeDesc& d, std::string* error);

JPH::RefConst<JPH::Shape> finish(JPH::ShapeSettings::ShapeResult result, std::string* error) {
    if (result.HasError()) {
        fail(error, std::string(result.GetError().c_str()));
        return nullptr;
    }
    return result.Get();
}

JPH::RefConst<JPH::Shape> buildBase(const ShapeDesc& d, std::string* error) {
    switch (d.type) {
    case ShapeType::Box: {
        f32 minHalf = std::min({d.halfExtents.x, d.halfExtents.y, d.halfExtents.z});
        if (minHalf <= 0.f) {
            fail(error, "box half extents must be > 0");
            return nullptr;
        }
        JPH::Ref<JPH::BoxShapeSettings> s = new JPH::BoxShapeSettings(toJolt(d.halfExtents), convexRadiusFor(d, minHalf));
        s->SetDensity(d.density);
        return finish(s->Create(), error);
    }
    case ShapeType::Sphere: {
        if (d.radius <= 0.f) {
            fail(error, "sphere radius must be > 0");
            return nullptr;
        }
        JPH::Ref<JPH::SphereShapeSettings> s = new JPH::SphereShapeSettings(d.radius);
        s->SetDensity(d.density);
        return finish(s->Create(), error);
    }
    case ShapeType::Capsule: {
        if (d.radius <= 0.f || d.halfHeight < 0.f) {
            fail(error, "capsule needs radius > 0 and halfHeight >= 0");
            return nullptr;
        }
        if (d.halfHeight == 0.f) {
            JPH::Ref<JPH::SphereShapeSettings> s = new JPH::SphereShapeSettings(d.radius);
            s->SetDensity(d.density);
            return finish(s->Create(), error);
        }
        JPH::Ref<JPH::CapsuleShapeSettings> s = new JPH::CapsuleShapeSettings(d.halfHeight, d.radius);
        s->SetDensity(d.density);
        return finish(s->Create(), error);
    }
    case ShapeType::Cylinder: {
        if (d.radius <= 0.f || d.halfHeight <= 0.f) {
            fail(error, "cylinder needs radius > 0 and halfHeight > 0");
            return nullptr;
        }
        JPH::Ref<JPH::CylinderShapeSettings> s = new JPH::CylinderShapeSettings(
            d.halfHeight, d.radius, convexRadiusFor(d, std::min(d.halfHeight, d.radius)));
        s->SetDensity(d.density);
        return finish(s->Create(), error);
    }
    case ShapeType::ConvexHull: {
        if (d.points.size() < 4) {
            fail(error, "convex hull needs at least 4 points");
            return nullptr;
        }
        JPH::Array<JPH::Vec3> pts;
        pts.reserve(d.points.size());
        for (const auto& p : d.points) {
            pts.push_back(toJolt(p));
        }
        JPH::Ref<JPH::ConvexHullShapeSettings> s =
            new JPH::ConvexHullShapeSettings(pts, d.convexRadius < 0.f ? JPH::cDefaultConvexRadius : d.convexRadius);
        s->SetDensity(d.density);
        return finish(s->Create(), error);
    }
    case ShapeType::TriangleMesh: {
        if (d.indices.empty() || d.indices.size() % 3 != 0) {
            fail(error, "triangle mesh needs 3*n indices");
            return nullptr;
        }
        JPH::VertexList verts;
        verts.reserve(d.points.size());
        for (const auto& p : d.points) {
            verts.push_back(JPH::Float3(p.x, p.y, p.z));
        }
        JPH::IndexedTriangleList tris;
        tris.reserve(d.indices.size() / 3);
        for (usize i = 0; i < d.indices.size(); i += 3) {
            if (std::max({d.indices[i], d.indices[i + 1], d.indices[i + 2]}) >= d.points.size()) {
                fail(error, "triangle mesh index out of range");
                return nullptr;
            }
            tris.push_back(JPH::IndexedTriangle(d.indices[i], d.indices[i + 1], d.indices[i + 2]));
        }
        JPH::Ref<JPH::MeshShapeSettings> s = new JPH::MeshShapeSettings(std::move(verts), std::move(tris));
        return finish(s->Create(), error);
    }
    case ShapeType::HeightField: {
        if (d.sampleCount < 2 || d.heights.size() != usize(d.sampleCount) * d.sampleCount) {
            fail(error, "height field needs sampleCount >= 2 and sampleCount^2 heights");
            return nullptr;
        }
        JPH::Ref<JPH::HeightFieldShapeSettings> s = new JPH::HeightFieldShapeSettings(
            d.heights.data(), toJolt(d.heightFieldOffset), toJolt(d.heightFieldScale), d.sampleCount);
        return finish(s->Create(), error);
    }
    case ShapeType::Compound: {
        if (d.children.empty()) {
            fail(error, "compound needs at least one child");
            return nullptr;
        }
        JPH::Ref<JPH::StaticCompoundShapeSettings> s = new JPH::StaticCompoundShapeSettings();
        for (const auto& child : d.children) {
            JPH::RefConst<JPH::Shape> cs = build(child.shape, error);
            if (!cs) {
                return nullptr;
            }
            s->AddShape(toJolt(child.position), toJolt(glm::normalize(child.rotation)), cs);
        }
        return finish(s->Create(), error);
    }
    default: fail(error, "shape type cannot be created from a ShapeDesc"); return nullptr;
    }
}

JPH::RefConst<JPH::Shape> build(const ShapeDesc& d, std::string* error) {
    JPH::RefConst<JPH::Shape> shape = buildBase(d, error);
    if (!shape) {
        return nullptr;
    }
    if (d.scale != glm::vec3(1.f)) {
        if (!shape->IsValidScale(toJolt(d.scale))) {
            fail(error, "invalid scale for this shape type (spheres/capsules need uniform scale)");
            return nullptr;
        }
        shape = finish(JPH::ScaledShapeSettings(shape, toJolt(d.scale)).Create(), error);
        if (!shape) {
            return nullptr;
        }
    }
    if (d.centerOfMassOffset != glm::vec3(0.f)) {
        shape = finish(JPH::OffsetCenterOfMassShapeSettings(toJolt(d.centerOfMassOffset), shape).Create(), error);
    }
    return shape;
}

ShapeRef adopt(const JPH::Shape* s) { return s ? ShapeRef::fromNative(s) : ShapeRef{}; }

} // namespace

// --- ShapeDesc --------------------------------------------------------------------------------
ShapeDesc ShapeDesc::box(glm::vec3 halfExtents) {
    ShapeDesc d;
    d.type = ShapeType::Box;
    d.halfExtents = halfExtents;
    return d;
}
ShapeDesc ShapeDesc::sphere(f32 radius) {
    ShapeDesc d;
    d.type = ShapeType::Sphere;
    d.radius = radius;
    return d;
}
ShapeDesc ShapeDesc::capsule(f32 halfHeight, f32 radius) {
    ShapeDesc d;
    d.type = ShapeType::Capsule;
    d.halfHeight = halfHeight;
    d.radius = radius;
    return d;
}
ShapeDesc ShapeDesc::cylinder(f32 halfHeight, f32 radius) {
    ShapeDesc d;
    d.type = ShapeType::Cylinder;
    d.halfHeight = halfHeight;
    d.radius = radius;
    return d;
}
ShapeDesc ShapeDesc::convexHull(std::vector<glm::vec3> points) {
    ShapeDesc d;
    d.type = ShapeType::ConvexHull;
    d.points = std::move(points);
    return d;
}
ShapeDesc ShapeDesc::triangleMesh(std::vector<glm::vec3> vertices, std::vector<u32> indices) {
    ShapeDesc d;
    d.type = ShapeType::TriangleMesh;
    d.points = std::move(vertices);
    d.indices = std::move(indices);
    return d;
}
ShapeDesc ShapeDesc::heightField(std::vector<f32> heights, u32 sampleCount, glm::vec3 offset, glm::vec3 scale) {
    ShapeDesc d;
    d.type = ShapeType::HeightField;
    d.heights = std::move(heights);
    d.sampleCount = sampleCount;
    d.heightFieldOffset = offset;
    d.heightFieldScale = scale;
    return d;
}
ShapeDesc ShapeDesc::compound(std::vector<CompoundChild> children) {
    ShapeDesc d;
    d.type = ShapeType::Compound;
    d.children = std::move(children);
    return d;
}

bool ShapeDesc::operator==(const ShapeDesc& o) const {
    return type == o.type && halfExtents == o.halfExtents && radius == o.radius && halfHeight == o.halfHeight &&
           convexRadius == o.convexRadius && density == o.density && points == o.points && indices == o.indices &&
           heights == o.heights && sampleCount == o.sampleCount && heightFieldOffset == o.heightFieldOffset &&
           heightFieldScale == o.heightFieldScale && children == o.children && scale == o.scale &&
           centerOfMassOffset == o.centerOfMassOffset;
}

bool CompoundChild::operator==(const CompoundChild& o) const {
    return shape == o.shape && position == o.position && rotation == o.rotation;
}

u64 hashShapeDesc(const ShapeDesc& desc) {
    Hasher h;
    hashInto(h, desc);
    return h.h;
}

// --- ShapeRef ---------------------------------------------------------------------------------
ShapeRef::ShapeRef(const ShapeRef& other) : m_shape(other.m_shape) {
    if (m_shape) {
        asJolt(m_shape)->AddRef();
    }
}
ShapeRef::ShapeRef(ShapeRef&& other) noexcept : m_shape(other.m_shape) { other.m_shape = nullptr; }
ShapeRef& ShapeRef::operator=(const ShapeRef& other) {
    if (this != &other) {
        ShapeRef tmp(other);
        std::swap(m_shape, tmp.m_shape);
    }
    return *this;
}
ShapeRef& ShapeRef::operator=(ShapeRef&& other) noexcept {
    if (this != &other) {
        ShapeRef tmp(std::move(other));
        std::swap(m_shape, tmp.m_shape);
    }
    return *this;
}
ShapeRef::~ShapeRef() {
    if (m_shape) {
        asJolt(m_shape)->Release();
    }
}

ShapeRef ShapeRef::fromNative(const void* joltShape) {
    ShapeRef r;
    r.m_shape = joltShape;
    if (joltShape) {
        asJolt(joltShape)->AddRef();
    }
    return r;
}

ShapeType ShapeRef::type() const {
    if (!m_shape) {
        return ShapeType::Other;
    }
    switch (asJolt(m_shape)->GetSubType()) {
    case JPH::EShapeSubType::Box: return ShapeType::Box;
    case JPH::EShapeSubType::Sphere: return ShapeType::Sphere;
    case JPH::EShapeSubType::Capsule: return ShapeType::Capsule;
    case JPH::EShapeSubType::Cylinder: return ShapeType::Cylinder;
    case JPH::EShapeSubType::ConvexHull: return ShapeType::ConvexHull;
    case JPH::EShapeSubType::Mesh: return ShapeType::TriangleMesh;
    case JPH::EShapeSubType::HeightField: return ShapeType::HeightField;
    case JPH::EShapeSubType::StaticCompound:
    case JPH::EShapeSubType::MutableCompound: return ShapeType::Compound;
    case JPH::EShapeSubType::Scaled: return ShapeType::Scaled;
    case JPH::EShapeSubType::OffsetCenterOfMass: return ShapeType::OffsetCenterOfMass;
    case JPH::EShapeSubType::RotatedTranslated: return ShapeType::RotatedTranslated;
    default: return ShapeType::Other;
    }
}

glm::vec3 ShapeRef::centerOfMass() const { return m_shape ? toGlm(asJolt(m_shape)->GetCenterOfMass()) : glm::vec3(0.f); }

Aabb ShapeRef::localBounds() const {
    if (!m_shape) {
        return {};
    }
    JPH::AABox b = asJolt(m_shape)->GetLocalBounds();
    // Jolt local bounds are relative to the center of mass.
    glm::vec3 com = centerOfMass();
    return {toGlm(b.mMin) + com, toGlm(b.mMax) + com};
}

f32 ShapeRef::volume() const { return m_shape ? asJolt(m_shape)->GetVolume() : 0.f; }

f32 ShapeRef::mass() const {
    if (!m_shape) {
        return 0.f;
    }
    auto sub = asJolt(m_shape)->GetSubType();
    if (sub == JPH::EShapeSubType::Mesh || sub == JPH::EShapeSubType::HeightField) {
        return 0.f;
    }
    return asJolt(m_shape)->GetMassProperties().mMass;
}

u32 ShapeRef::refCount() const { return m_shape ? asJolt(m_shape)->GetRefCount() : 0; }

ShapeRef createShape(const ShapeDesc& desc, std::string* error) {
    detail::ensureJoltInitialized();
    return adopt(build(desc, error).GetPtr());
}

ShapeRef makeScaled(const ShapeRef& shape, glm::vec3 scale) {
    if (!shape) {
        return {};
    }
    detail::ensureJoltInitialized();
    return adopt(finish(JPH::ScaledShapeSettings(asJolt(shape.native()), toJolt(scale)).Create(), nullptr).GetPtr());
}

ShapeRef makeRotatedTranslated(const ShapeRef& shape, glm::vec3 position, glm::quat rotation) {
    if (!shape) {
        return {};
    }
    detail::ensureJoltInitialized();
    return adopt(finish(JPH::RotatedTranslatedShapeSettings(toJolt(position), toJolt(glm::normalize(rotation)),
                                                            asJolt(shape.native()))
                            .Create(),
                        nullptr)
                     .GetPtr());
}

ShapeRef makeOffsetCenterOfMass(const ShapeRef& shape, glm::vec3 offset) {
    if (!shape) {
        return {};
    }
    detail::ensureJoltInitialized();
    return adopt(
        finish(JPH::OffsetCenterOfMassShapeSettings(toJolt(offset), asJolt(shape.native())).Create(), nullptr).GetPtr());
}

// --- ShapeCache -------------------------------------------------------------------------------
ShapeRef ShapeCache::getOrCreate(const ShapeDesc& desc, std::string* error) {
    u64 key = hashShapeDesc(desc);
    {
        std::lock_guard lock(m_mutex);
        auto [begin, end] = m_entries.equal_range(key);
        for (auto it = begin; it != end; ++it) {
            if (it->second.desc == desc) {
                return it->second.shape;
            }
        }
    }
    // Build outside the lock (mesh/hull cooking can be slow); a concurrent duplicate build is harmless.
    ShapeRef shape = createShape(desc, error);
    if (!shape) {
        return {};
    }
    std::lock_guard lock(m_mutex);
    auto [begin, end] = m_entries.equal_range(key);
    for (auto it = begin; it != end; ++it) {
        if (it->second.desc == desc) {
            return it->second.shape;
        }
    }
    m_entries.emplace(key, Entry{desc, shape});
    return shape;
}

usize ShapeCache::collectGarbage() {
    std::lock_guard lock(m_mutex);
    usize removed = 0;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->second.shape.refCount() <= 1) {
            it = m_entries.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void ShapeCache::clear() {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
}

usize ShapeCache::size() const {
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

} // namespace ox::physics
