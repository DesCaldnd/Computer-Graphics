#pragma once

// Engine math: glm conventions are right-handed, Y-up, -Z forward, depth [0,1]
// (GLM_FORCE_DEPTH_ZERO_TO_ONE is set by the core target). The renderer uses reversed-Z
// (near = 1, far = 0), see perspectiveReversedZ / Frustum::fromViewProj.

#include <oxwald/core/types.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace ox {

inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kTwoPi = 2.0f * kPi;
inline constexpr f32 kHalfPi = 0.5f * kPi;
inline constexpr f32 kEpsilon = 1e-6f;
inline constexpr f32 kInfinity = std::numeric_limits<f32>::infinity();

inline constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
inline constexpr glm::vec3 kWorldRight{1.0f, 0.0f, 0.0f};
inline constexpr glm::vec3 kWorldForward{0.0f, 0.0f, -1.0f};

[[nodiscard]] constexpr f32 toRadians(f32 degrees) { return degrees * (kPi / 180.0f); }
[[nodiscard]] constexpr f32 toDegrees(f32 radians) { return radians * (180.0f / kPi); }

[[nodiscard]] inline bool nearlyEqual(f32 a, f32 b, f32 epsilon = 1e-5f) {
    // Absolute tolerance near zero, relative tolerance for large magnitudes.
    const f32 diff = std::abs(a - b);
    return diff <= epsilon || diff <= epsilon * std::max(std::abs(a), std::abs(b));
}
[[nodiscard]] bool nearlyEqual(const glm::vec2& a, const glm::vec2& b, f32 epsilon = 1e-5f);
[[nodiscard]] bool nearlyEqual(const glm::vec3& a, const glm::vec3& b, f32 epsilon = 1e-5f);
[[nodiscard]] bool nearlyEqual(const glm::vec4& a, const glm::vec4& b, f32 epsilon = 1e-5f);
// q and -q describe the same rotation and compare equal.
[[nodiscard]] bool nearlyEqual(const glm::quat& a, const glm::quat& b, f32 epsilon = 1e-5f);
[[nodiscard]] bool nearlyEqual(const glm::mat4& a, const glm::mat4& b, f32 epsilon = 1e-5f);

[[nodiscard]] constexpr f32 saturate(f32 x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
// Maps x from [inMin,inMax] to [outMin,outMax] (not clamped). Degenerate input range returns outMin.
[[nodiscard]] inline f32 remap(f32 x, f32 inMin, f32 inMax, f32 outMin, f32 outMax) {
    const f32 range = inMax - inMin;
    if (std::abs(range) < std::numeric_limits<f32>::min()) {
        return outMin;
    }
    return outMin + (x - inMin) / range * (outMax - outMin);
}
[[nodiscard]] inline f32 remapClamped(f32 x, f32 inMin, f32 inMax, f32 outMin, f32 outMax) {
    const f32 range = inMax - inMin;
    if (std::abs(range) < std::numeric_limits<f32>::min()) {
        return outMin;
    }
    return outMin + saturate((x - inMin) / range) * (outMax - outMin);
}

// ---------------------------------------------------------------------------------------------
// Rotations / projections

// Rotation whose local -Z axis points along `forward` and whose local +Y is as close to `up`
// as possible. Falls back to another up vector when forward is parallel to up.
[[nodiscard]] glm::quat lookRotation(glm::vec3 forward, glm::vec3 up = kWorldUp);
// Shortest-arc rotation taking direction `from` onto `to` (inputs need not be normalised).
[[nodiscard]] glm::quat fromToRotation(glm::vec3 from, glm::vec3 to);

// Reversed-Z projections with depth range [0,1]: near plane maps to 1, far plane to 0.
// Like glm, NDC +Y is up; the Vulkan renderer flips Y with a negative viewport height.
[[nodiscard]] glm::mat4 perspectiveReversedZ(f32 fovY, f32 aspect, f32 zNear, f32 zFar);
// Far plane at infinity (depth -> 0 as distance -> inf).
[[nodiscard]] glm::mat4 perspectiveInfiniteReversedZ(f32 fovY, f32 aspect, f32 zNear);
[[nodiscard]] glm::mat4 orthoReversedZ(f32 left, f32 right, f32 bottom, f32 top, f32 zNear, f32 zFar);

// Splits an affine matrix into T * R * S. A negative determinant (mirroring) is folded into
// scale.x. Returns false (identity rotation, outputs still written) when a scale axis is ~0.
bool decompose(const glm::mat4& m, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale);

// ---------------------------------------------------------------------------------------------
// Transform (translation, rotation, scale; matrix = T * R * S)

struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    [[nodiscard]] glm::mat4 toMatrix() const;
    // Exact for uniform scale; with non-uniform scale + rotation the true inverse has shear and
    // is not representable as TRS (use glm::inverse(toMatrix()) when that matters).
    [[nodiscard]] Transform inverse() const;
    [[nodiscard]] static Transform fromMatrix(const glm::mat4& m);

    // World = parent * child. Same shear caveat as inverse() for non-uniform parent scale.
    friend Transform operator*(const Transform& parent, const Transform& child);

    [[nodiscard]] glm::vec3 transformPoint(glm::vec3 p) const { return position + rotation * (scale * p); }
    // Applies rotation and scale (no translation).
    [[nodiscard]] glm::vec3 transformVector(glm::vec3 v) const { return rotation * (scale * v); }
    // Applies rotation only.
    [[nodiscard]] glm::vec3 transformDirection(glm::vec3 d) const { return rotation * d; }

    [[nodiscard]] glm::vec3 forward() const { return rotation * kWorldForward; }
    [[nodiscard]] glm::vec3 right() const { return rotation * kWorldRight; }
    [[nodiscard]] glm::vec3 up() const { return rotation * kWorldUp; }

    // Linear position/scale, shortest-path slerp for rotation.
    [[nodiscard]] static Transform lerp(const Transform& a, const Transform& b, f32 t);
    [[nodiscard]] static Transform identity() { return {}; }

    bool operator==(const Transform&) const = default;
};

[[nodiscard]] Transform compose(const Transform& parent, const Transform& child);
[[nodiscard]] bool nearlyEqual(const Transform& a, const Transform& b, f32 epsilon = 1e-5f);

// ---------------------------------------------------------------------------------------------
// Bounding volumes

struct AABB {
    glm::vec3 min{kInfinity};
    glm::vec3 max{-kInfinity};

    [[nodiscard]] static AABB fromCenterExtents(glm::vec3 center, glm::vec3 halfExtents) {
        return {center - halfExtents, center + halfExtents};
    }
    [[nodiscard]] static AABB fromMinMax(glm::vec3 mn, glm::vec3 mx) { return {mn, mx}; }

    // A default-constructed box is empty/invalid until something is expanded into it.
    [[nodiscard]] bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    [[nodiscard]] glm::vec3 center() const { return (min + max) * 0.5f; }
    // Half size.
    [[nodiscard]] glm::vec3 extents() const { return (max - min) * 0.5f; }
    [[nodiscard]] glm::vec3 size() const { return max - min; }
    [[nodiscard]] f32 surfaceArea() const;
    [[nodiscard]] f32 volume() const;

    void expand(glm::vec3 point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }
    void expand(const AABB& other) {
        if (other.valid()) {
            min = glm::min(min, other.min);
            max = glm::max(max, other.max);
        }
    }
    void reset() { *this = AABB{}; }

    [[nodiscard]] bool contains(glm::vec3 p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
    [[nodiscard]] bool contains(const AABB& o) const {
        return o.valid() && contains(o.min) && contains(o.max);
    }
    [[nodiscard]] bool intersects(const AABB& o) const {
        return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y && min.z <= o.max.z &&
               max.z >= o.min.z;
    }
    [[nodiscard]] glm::vec3 closestPoint(glm::vec3 p) const { return glm::clamp(p, min, max); }
    // Bounding box of this box transformed by an affine matrix (Arvo's method).
    [[nodiscard]] AABB transformed(const glm::mat4& m) const;

    bool operator==(const AABB&) const = default;
};

struct Sphere {
    glm::vec3 center{0.0f};
    f32 radius = 0.0f;

    [[nodiscard]] bool contains(glm::vec3 p) const;
    [[nodiscard]] bool intersects(const Sphere& o) const;
    [[nodiscard]] bool intersects(const AABB& box) const;
    bool operator==(const Sphere&) const = default;
};

// Plane: dot(normal, p) + d = 0. Positive signed distance is on the side the normal points to.
struct Plane {
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    f32 d = 0.0f;

    [[nodiscard]] f32 signedDistance(glm::vec3 p) const { return glm::dot(normal, p) + d; }
    // Degenerate planes (|normal| ~ 0) normalise to {0, 1}: every point is on the positive side.
    [[nodiscard]] Plane normalized() const;
    [[nodiscard]] glm::vec3 project(glm::vec3 p) const { return p - signedDistance(p) * normal; }
    [[nodiscard]] static Plane fromPointNormal(glm::vec3 point, glm::vec3 normal);
    // Counter-clockwise a,b,c (seen from the front) gives a normal pointing towards the viewer.
    [[nodiscard]] static Plane fromPoints(glm::vec3 a, glm::vec3 b, glm::vec3 c);
    bool operator==(const Plane&) const = default;
};

struct OBB {
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{0.5f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};

    [[nodiscard]] AABB bounds() const;
    [[nodiscard]] bool contains(glm::vec3 p) const;
    [[nodiscard]] std::array<glm::vec3, 8> corners() const;
    bool operator==(const OBB&) const = default;
};

// ---------------------------------------------------------------------------------------------
// Frustum

struct Frustum {
    enum PlaneIndex : u32 { Left = 0, Right, Bottom, Top, Near, Far, Count };

    // Normalised planes with normals pointing inwards. A degenerate plane (infinite far plane of
    // an infinite reversed-Z projection) is stored as {0, 1} and always passes.
    std::array<Plane, 6> planes{};

    // Gribb/Hartmann extraction from a [0,1]-depth projection * view matrix.
    [[nodiscard]] static Frustum fromViewProj(const glm::mat4& viewProj, bool reversedZ = true);
    // World-space corners: [0..3] near plane, [4..7] far plane, each (-1,-1),(1,-1),(1,1),(-1,1) in NDC xy.
    // Infinite projections have no far corners - use a finite far plane for visualisation.
    [[nodiscard]] static std::array<glm::vec3, 8> corners(const glm::mat4& invViewProj, bool reversedZ = true);

    [[nodiscard]] bool contains(glm::vec3 p) const;
    // Conservative (may report intersection for boxes just outside a frustum corner).
    [[nodiscard]] bool intersects(const AABB& box) const;
    [[nodiscard]] bool intersects(const Sphere& sphere) const;
};

// ---------------------------------------------------------------------------------------------
// Rays

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};

    [[nodiscard]] glm::vec3 at(f32 t) const { return origin + direction * t; }
};

struct RayHit {
    f32 t = 0.0f;
    glm::vec3 normal{0.0f}; // geometric, normalised, cross(b-a, c-a)
    f32 u = 0.0f;           // barycentrics: p = (1-u-v)*a + u*b + v*c
    f32 v = 0.0f;
    bool frontFace = true;
};

// All intersection functions return the smallest t >= 0 (in units of ray.direction's length).
// A ray starting inside a box returns 0; starting inside a sphere returns the exit distance.
[[nodiscard]] std::optional<f32> intersectRayAABB(const Ray& ray, const AABB& box);
[[nodiscard]] std::optional<f32> intersectRaySphere(const Ray& ray, const Sphere& sphere);
[[nodiscard]] std::optional<f32> intersectRayPlane(const Ray& ray, const Plane& plane);
// Moller-Trumbore. Front faces are counter-clockwise when seen from the ray origin.
[[nodiscard]] std::optional<RayHit> intersectRayTriangle(const Ray& ray, glm::vec3 a, glm::vec3 b, glm::vec3 c,
                                                         bool cullBackFace = false);

// ---------------------------------------------------------------------------------------------
// Deterministic random numbers (xoshiro256**, seeded through splitmix64). Same seed -> same
// sequence on every platform. Not thread-safe; use one instance per thread.

class Random {
public:
    explicit Random(u64 seed = 0x0A5F1D2C3B4E5F60ull);

    void seed(u64 seed);
    u64 nextU64();
    u32 nextU32() { return static_cast<u32>(nextU64() >> 32); }
    // Uniform in [0, 1).
    f32 nextFloat();
    f64 nextDouble();
    // Uniform in [a, b).
    f32 range(f32 a, f32 b) { return a + (b - a) * nextFloat(); }
    // Uniform integer in [a, b] (inclusive, unbiased). Order of a and b does not matter.
    i32 rangeInt(i32 a, i32 b);
    bool chance(f32 probability) { return nextFloat() < probability; }
    glm::vec3 unitVector();
    glm::vec3 inUnitSphere();
    glm::vec2 inUnitDisk();
    // Uniformly distributed rotation.
    glm::quat rotation();

private:
    u64 m_state[4]{};
};

} // namespace ox
