#include <oxwald/core/math.hpp>

#include <algorithm>

namespace ox {

bool nearlyEqual(const glm::vec2& a, const glm::vec2& b, f32 epsilon) {
    return nearlyEqual(a.x, b.x, epsilon) && nearlyEqual(a.y, b.y, epsilon);
}

bool nearlyEqual(const glm::vec3& a, const glm::vec3& b, f32 epsilon) {
    return nearlyEqual(a.x, b.x, epsilon) && nearlyEqual(a.y, b.y, epsilon) && nearlyEqual(a.z, b.z, epsilon);
}

bool nearlyEqual(const glm::vec4& a, const glm::vec4& b, f32 epsilon) {
    return nearlyEqual(glm::vec3(a), glm::vec3(b), epsilon) && nearlyEqual(a.w, b.w, epsilon);
}

bool nearlyEqual(const glm::quat& a, const glm::quat& b, f32 epsilon) {
    const glm::vec4 va{a.x, a.y, a.z, a.w};
    const glm::vec4 vb{b.x, b.y, b.z, b.w};
    return nearlyEqual(va, vb, epsilon) || nearlyEqual(va, -vb, epsilon);
}

bool nearlyEqual(const glm::mat4& a, const glm::mat4& b, f32 epsilon) {
    for (int c = 0; c < 4; ++c) {
        if (!nearlyEqual(a[c], b[c], epsilon)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------

glm::quat lookRotation(glm::vec3 forward, glm::vec3 up) {
    const f32 len = glm::length(forward);
    if (len < kEpsilon) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    const glm::vec3 z = -forward / len; // local +Z points backwards
    glm::vec3 x = glm::cross(up, z);
    if (glm::dot(x, x) < 1e-10f) {
        // forward parallel to up: pick any up that is not parallel.
        const glm::vec3 alt = std::abs(z.y) < 0.9f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{0.0f, 0.0f, 1.0f};
        x = glm::cross(alt, z);
    }
    x = glm::normalize(x);
    const glm::vec3 y = glm::cross(z, x);
    return glm::normalize(glm::quat_cast(glm::mat3{x, y, z}));
}

glm::quat fromToRotation(glm::vec3 from, glm::vec3 to) {
    const f32 lf = glm::length(from);
    const f32 lt = glm::length(to);
    if (lf < kEpsilon || lt < kEpsilon) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    from /= lf;
    to /= lt;
    const f32 cosTheta = glm::dot(from, to);
    if (cosTheta < -1.0f + 1e-6f) {
        // Opposite: rotate 180 degrees about any axis perpendicular to `from`.
        glm::vec3 axis = glm::cross(glm::vec3{1.0f, 0.0f, 0.0f}, from);
        if (glm::dot(axis, axis) < 1e-6f) {
            axis = glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, from);
        }
        return glm::angleAxis(kPi, glm::normalize(axis));
    }
    const glm::vec3 axis = glm::cross(from, to);
    const f32 s = std::sqrt((1.0f + cosTheta) * 2.0f);
    const f32 invs = 1.0f / s;
    return glm::normalize(glm::quat{s * 0.5f, axis.x * invs, axis.y * invs, axis.z * invs});
}

glm::mat4 perspectiveReversedZ(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    // Swapping near/far in a [0,1] right-handed projection yields reversed-Z.
    return glm::perspectiveRH_ZO(fovY, aspect, zFar, zNear);
}

glm::mat4 perspectiveInfiniteReversedZ(f32 fovY, f32 aspect, f32 zNear) {
    const f32 f = 1.0f / std::tan(fovY * 0.5f);
    glm::mat4 m{0.0f};
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][2] = 0.0f;
    m[2][3] = -1.0f;
    m[3][2] = zNear;
    return m;
}

glm::mat4 orthoReversedZ(f32 left, f32 right, f32 bottom, f32 top, f32 zNear, f32 zFar) {
    return glm::orthoRH_ZO(left, right, bottom, top, zFar, zNear);
}

bool decompose(const glm::mat4& m, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) {
    translation = glm::vec3(m[3]);
    glm::vec3 c0{m[0]};
    glm::vec3 c1{m[1]};
    glm::vec3 c2{m[2]};
    scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    if (scale.x < kEpsilon || scale.y < kEpsilon || scale.z < kEpsilon) {
        rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        return false;
    }
    if (glm::dot(glm::cross(c0, c1), c2) < 0.0f) {
        scale.x = -scale.x;
    }
    c0 /= scale.x;
    c1 /= scale.y;
    c2 /= scale.z;
    rotation = glm::normalize(glm::quat_cast(glm::mat3{c0, c1, c2}));
    return true;
}

// ---------------------------------------------------------------------------------------------

glm::mat4 Transform::toMatrix() const {
    glm::mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4{position, 1.0f};
    return m;
}

Transform Transform::inverse() const {
    Transform r;
    r.rotation = glm::conjugate(rotation);
    r.scale = {std::abs(scale.x) > kEpsilon ? 1.0f / scale.x : 0.0f, std::abs(scale.y) > kEpsilon ? 1.0f / scale.y : 0.0f,
               std::abs(scale.z) > kEpsilon ? 1.0f / scale.z : 0.0f};
    r.position = r.scale * (r.rotation * -position);
    return r;
}

Transform Transform::fromMatrix(const glm::mat4& m) {
    Transform t;
    decompose(m, t.position, t.rotation, t.scale);
    return t;
}

Transform operator*(const Transform& parent, const Transform& child) {
    Transform r;
    r.position = parent.transformPoint(child.position);
    r.rotation = glm::normalize(parent.rotation * child.rotation);
    r.scale = parent.scale * child.scale;
    return r;
}

Transform compose(const Transform& parent, const Transform& child) { return parent * child; }

Transform Transform::lerp(const Transform& a, const Transform& b, f32 t) {
    Transform r;
    r.position = glm::mix(a.position, b.position, t);
    glm::quat qb = b.rotation;
    if (glm::dot(a.rotation, qb) < 0.0f) {
        qb = -qb;
    }
    r.rotation = glm::normalize(glm::slerp(a.rotation, qb, t));
    r.scale = glm::mix(a.scale, b.scale, t);
    return r;
}

bool nearlyEqual(const Transform& a, const Transform& b, f32 epsilon) {
    return nearlyEqual(a.position, b.position, epsilon) && nearlyEqual(a.rotation, b.rotation, epsilon) &&
           nearlyEqual(a.scale, b.scale, epsilon);
}

// ---------------------------------------------------------------------------------------------

f32 AABB::surfaceArea() const {
    if (!valid()) {
        return 0.0f;
    }
    const glm::vec3 s = size();
    return 2.0f * (s.x * s.y + s.y * s.z + s.z * s.x);
}

f32 AABB::volume() const {
    if (!valid()) {
        return 0.0f;
    }
    const glm::vec3 s = size();
    return s.x * s.y * s.z;
}

AABB AABB::transformed(const glm::mat4& m) const {
    if (!valid()) {
        return {};
    }
    const glm::vec3 c = center();
    const glm::vec3 e = extents();
    const glm::vec3 nc = glm::vec3(m * glm::vec4(c, 1.0f));
    const glm::mat3 a{glm::abs(glm::vec3(m[0])), glm::abs(glm::vec3(m[1])), glm::abs(glm::vec3(m[2]))};
    const glm::vec3 ne = a * e;
    return {nc - ne, nc + ne};
}

bool Sphere::contains(glm::vec3 p) const {
    const glm::vec3 d = p - center;
    return glm::dot(d, d) <= radius * radius;
}

bool Sphere::intersects(const Sphere& o) const {
    const glm::vec3 d = o.center - center;
    const f32 r = radius + o.radius;
    return glm::dot(d, d) <= r * r;
}

bool Sphere::intersects(const AABB& box) const {
    if (!box.valid()) {
        return false;
    }
    return contains(box.closestPoint(center));
}

Plane Plane::normalized() const {
    const f32 len = glm::length(normal);
    if (len < kEpsilon) {
        return Plane{glm::vec3{0.0f}, 1.0f};
    }
    return Plane{normal / len, d / len};
}

Plane Plane::fromPointNormal(glm::vec3 point, glm::vec3 n) {
    const f32 len = glm::length(n);
    const glm::vec3 nn = len > kEpsilon ? n / len : glm::vec3{0.0f, 1.0f, 0.0f};
    return Plane{nn, -glm::dot(nn, point)};
}

Plane Plane::fromPoints(glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    return fromPointNormal(a, glm::cross(b - a, c - a));
}

AABB OBB::bounds() const {
    const glm::mat3 r = glm::mat3_cast(rotation);
    const glm::mat3 absR{glm::abs(r[0]), glm::abs(r[1]), glm::abs(r[2])};
    const glm::vec3 e = absR * halfExtents;
    return {center - e, center + e};
}

bool OBB::contains(glm::vec3 p) const {
    const glm::vec3 local = glm::conjugate(rotation) * (p - center);
    return std::abs(local.x) <= halfExtents.x && std::abs(local.y) <= halfExtents.y &&
           std::abs(local.z) <= halfExtents.z;
}

std::array<glm::vec3, 8> OBB::corners() const {
    std::array<glm::vec3, 8> out{};
    for (u32 i = 0; i < 8; ++i) {
        const glm::vec3 sign{(i & 1u) ? 1.0f : -1.0f, (i & 2u) ? 1.0f : -1.0f, (i & 4u) ? 1.0f : -1.0f};
        out[i] = center + rotation * (sign * halfExtents);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------

Frustum Frustum::fromViewProj(const glm::mat4& m, bool reversedZ) {
    const glm::vec4 row0{m[0][0], m[1][0], m[2][0], m[3][0]};
    const glm::vec4 row1{m[0][1], m[1][1], m[2][1], m[3][1]};
    const glm::vec4 row2{m[0][2], m[1][2], m[2][2], m[3][2]};
    const glm::vec4 row3{m[0][3], m[1][3], m[2][3], m[3][3]};

    // Clip space: -w <= x,y <= w and 0 <= z <= w. For reversed-Z, z = w is the near plane.
    const glm::vec4 zGreaterZero = row2;
    const glm::vec4 zLessW = row3 - row2;

    const std::array<glm::vec4, 6> raw{
        row3 + row0, row3 - row0, row3 + row1, row3 - row1,
        reversedZ ? zLessW : zGreaterZero,
        reversedZ ? zGreaterZero : zLessW,
    };
    Frustum f;
    for (usize i = 0; i < raw.size(); ++i) {
        f.planes[i] = Plane{glm::vec3(raw[i]), raw[i].w}.normalized();
    }
    return f;
}

std::array<glm::vec3, 8> Frustum::corners(const glm::mat4& invViewProj, bool reversedZ) {
    const f32 zNear = reversedZ ? 1.0f : 0.0f;
    const f32 zFar = reversedZ ? 0.0f : 1.0f;
    constexpr std::array<glm::vec2, 4> kXY{glm::vec2{-1, -1}, glm::vec2{1, -1}, glm::vec2{1, 1}, glm::vec2{-1, 1}};
    std::array<glm::vec3, 8> out{};
    for (u32 i = 0; i < 8; ++i) {
        const glm::vec2 xy = kXY[i % 4];
        glm::vec4 p = invViewProj * glm::vec4{xy, i < 4 ? zNear : zFar, 1.0f};
        const f32 w = std::abs(p.w) > 1e-12f ? p.w : (p.w < 0.0f ? -1e-12f : 1e-12f);
        out[i] = glm::vec3(p) / w;
    }
    return out;
}

bool Frustum::contains(glm::vec3 p) const {
    for (const Plane& plane : planes) {
        if (plane.signedDistance(p) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersects(const AABB& box) const {
    if (!box.valid()) {
        return false;
    }
    for (const Plane& plane : planes) {
        // "Positive vertex": the box corner furthest along the plane normal.
        const glm::vec3 p{plane.normal.x >= 0.0f ? box.max.x : box.min.x, plane.normal.y >= 0.0f ? box.max.y : box.min.y,
                          plane.normal.z >= 0.0f ? box.max.z : box.min.z};
        if (plane.signedDistance(p) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersects(const Sphere& sphere) const {
    for (const Plane& plane : planes) {
        if (plane.signedDistance(sphere.center) < -sphere.radius) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------

std::optional<f32> intersectRayAABB(const Ray& ray, const AABB& box) {
    if (!box.valid()) {
        return std::nullopt;
    }
    f32 tMin = 0.0f;
    f32 tMax = kInfinity;
    for (int axis = 0; axis < 3; ++axis) {
        const f32 o = ray.origin[axis];
        const f32 d = ray.direction[axis];
        if (std::abs(d) < 1e-12f) {
            if (o < box.min[axis] || o > box.max[axis]) {
                return std::nullopt;
            }
            continue;
        }
        const f32 inv = 1.0f / d;
        f32 t0 = (box.min[axis] - o) * inv;
        f32 t1 = (box.max[axis] - o) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return std::nullopt;
        }
    }
    return tMin;
}

std::optional<f32> intersectRaySphere(const Ray& ray, const Sphere& sphere) {
    const glm::vec3 oc = ray.origin - sphere.center;
    const f32 a = glm::dot(ray.direction, ray.direction);
    if (a < 1e-20f) {
        return std::nullopt;
    }
    const f32 halfB = glm::dot(oc, ray.direction);
    const f32 c = glm::dot(oc, oc) - sphere.radius * sphere.radius;
    const f32 disc = halfB * halfB - a * c;
    if (disc < 0.0f) {
        return std::nullopt;
    }
    const f32 sq = std::sqrt(disc);
    f32 t = (-halfB - sq) / a;
    if (t < 0.0f) {
        t = (-halfB + sq) / a;
        if (t < 0.0f) {
            return std::nullopt;
        }
    }
    return t;
}

std::optional<f32> intersectRayPlane(const Ray& ray, const Plane& plane) {
    const f32 denom = glm::dot(plane.normal, ray.direction);
    if (std::abs(denom) < 1e-12f) {
        return std::nullopt;
    }
    const f32 t = -plane.signedDistance(ray.origin) / denom;
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

std::optional<RayHit> intersectRayTriangle(const Ray& ray, glm::vec3 a, glm::vec3 b, glm::vec3 c, bool cullBackFace) {
    constexpr f32 kDetEpsilon = 1e-12f;
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 p = glm::cross(ray.direction, e2);
    const f32 det = glm::dot(e1, p);
    // det > 0 <=> the ray hits the counter-clockwise (front) side.
    if (cullBackFace ? det < kDetEpsilon : std::abs(det) < kDetEpsilon) {
        return std::nullopt;
    }
    const f32 invDet = 1.0f / det;
    const glm::vec3 s = ray.origin - a;
    const f32 u = glm::dot(s, p) * invDet;
    if (u < 0.0f || u > 1.0f) {
        return std::nullopt;
    }
    const glm::vec3 q = glm::cross(s, e1);
    const f32 v = glm::dot(ray.direction, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) {
        return std::nullopt;
    }
    const f32 t = glm::dot(e2, q) * invDet;
    if (t < 0.0f) {
        return std::nullopt;
    }
    RayHit hit;
    hit.t = t;
    hit.u = u;
    hit.v = v;
    hit.normal = glm::normalize(glm::cross(e1, e2));
    hit.frontFace = det > 0.0f;
    return hit;
}

// ---------------------------------------------------------------------------------------------

namespace {

u64 splitmix64(u64& x) {
    u64 z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

constexpr u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

} // namespace

Random::Random(u64 s) { seed(s); }

void Random::seed(u64 s) {
    for (u64& v : m_state) {
        v = splitmix64(s);
    }
}

u64 Random::nextU64() {
    const u64 result = rotl(m_state[1] * 5, 7) * 9;
    const u64 t = m_state[1] << 17;
    m_state[2] ^= m_state[0];
    m_state[3] ^= m_state[1];
    m_state[1] ^= m_state[2];
    m_state[0] ^= m_state[3];
    m_state[2] ^= t;
    m_state[3] = rotl(m_state[3], 45);
    return result;
}

f32 Random::nextFloat() { return static_cast<f32>(nextU64() >> 40) * 0x1.0p-24f; }

f64 Random::nextDouble() { return static_cast<f64>(nextU64() >> 11) * 0x1.0p-53; }

i32 Random::rangeInt(i32 a, i32 b) {
    if (a > b) {
        std::swap(a, b);
    }
    const u64 span = static_cast<u64>(static_cast<i64>(b) - static_cast<i64>(a)) + 1u;
    // Rejection sampling removes modulo bias.
    const u64 limit = std::numeric_limits<u64>::max() - std::numeric_limits<u64>::max() % span;
    u64 x = nextU64();
    while (x >= limit) {
        x = nextU64();
    }
    return static_cast<i32>(static_cast<i64>(a) + static_cast<i64>(x % span));
}

glm::vec3 Random::unitVector() {
    const f32 z = range(-1.0f, 1.0f);
    const f32 phi = range(0.0f, kTwoPi);
    const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(phi), r * std::sin(phi), z};
}

glm::vec3 Random::inUnitSphere() {
    for (;;) {
        const glm::vec3 p{range(-1.0f, 1.0f), range(-1.0f, 1.0f), range(-1.0f, 1.0f)};
        if (glm::dot(p, p) < 1.0f) {
            return p;
        }
    }
}

glm::vec2 Random::inUnitDisk() {
    for (;;) {
        const glm::vec2 p{range(-1.0f, 1.0f), range(-1.0f, 1.0f)};
        if (glm::dot(p, p) < 1.0f) {
            return p;
        }
    }
}

glm::quat Random::rotation() {
    // Shoemake, "Uniform random rotations" (Graphics Gems III).
    const f32 u1 = nextFloat();
    const f32 u2 = nextFloat() * kTwoPi;
    const f32 u3 = nextFloat() * kTwoPi;
    const f32 s1 = std::sqrt(1.0f - u1);
    const f32 s2 = std::sqrt(u1);
    return glm::normalize(glm::quat{s2 * std::cos(u3), s1 * std::sin(u2), s1 * std::cos(u2), s2 * std::sin(u3)});
}

} // namespace ox
