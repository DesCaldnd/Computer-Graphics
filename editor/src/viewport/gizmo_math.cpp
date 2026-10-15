#include "viewport/gizmo_math.hpp"

#include <algorithm>
#include <cmath>

namespace ox::editor::gizmo {

namespace {
constexpr float kTiny = 1e-8f;

glm::vec3 anyPerpendicular(glm::vec3 v) {
    const glm::vec3 a = std::abs(v.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    return glm::normalize(glm::cross(v, a));
}
} // namespace

int axisIndex(GizmoAxis a) { return a == AxisX ? 0 : a == AxisY ? 1 : a == AxisZ ? 2 : -1; }

int planeNormalIndex(GizmoAxis a) { return a == AxisYZ ? 0 : a == AxisXZ ? 1 : a == AxisXY ? 2 : -1; }

glm::quat frameRotation(const glm::mat3& m) {
    glm::vec3 x = m[0];
    const glm::vec3 c1 = m[1], c2 = m[2];
    if (glm::dot(x, x) < kTiny) x = glm::cross(c1, c2);
    if (glm::dot(x, x) < kTiny) return glm::quat(1, 0, 0, 0);
    x = glm::normalize(x);
    if (glm::dot(glm::cross(m[0], c1), c2) < 0.0f) x = -x; // mirrored: fold into X like ox::decompose
    glm::vec3 y = c1 - glm::dot(c1, x) * x;
    if (glm::dot(y, y) < kTiny) y = glm::cross(c2, x);
    y = glm::dot(y, y) < kTiny ? anyPerpendicular(x) : glm::normalize(y);
    const glm::vec3 z = glm::cross(x, y);
    return glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
}

glm::quat frameRotation(const glm::mat4& world) { return frameRotation(glm::mat3(world)); }

glm::quat effectiveOrientation(GizmoMode mode, GizmoSpace space, glm::quat local) {
    if (space == GizmoSpace::Local || mode == GizmoMode::Scale) return glm::normalize(local);
    return glm::quat(1, 0, 0, 0);
}

glm::vec3 axisOf(glm::quat frame, int i) {
    glm::vec3 e(0.0f);
    e[i] = 1.0f;
    return glm::normalize(frame * e);
}

bool Projection::project(glm::vec3 p, glm::vec2& out) const {
    const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
    if (c.w <= 1e-4f) return false;
    const glm::vec3 ndc = glm::vec3(c) / c.w;
    out = glm::vec2((ndc.x * 0.5f + 0.5f) * size.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * size.y);
    return true;
}

Ray Projection::ray(glm::vec2 pixel) const {
    const glm::vec2 ndc{pixel.x / size.x * 2.0f - 1.0f, 1.0f - pixel.y / size.y * 2.0f};
    const glm::mat4 inv = glm::inverse(viewProj);
    // reversed Z: depth 1 = near, ~0 = far
    glm::vec4 n = inv * glm::vec4(ndc, 1.0f, 1.0f);
    glm::vec4 f = inv * glm::vec4(ndc, 0.01f, 1.0f);
    n /= n.w;
    f /= f.w;
    Ray r;
    r.origin = glm::vec3(n);
    r.direction = glm::normalize(glm::vec3(f) - glm::vec3(n));
    return r;
}

std::array<glm::vec3, 4> planeHandle(const Frame& f, int normal) {
    const glm::vec3 a = f.axis((normal + 1) % 3), b = f.axis((normal + 2) % 3);
    std::array<glm::vec3, 4> out;
    const glm::vec2 k[4] = {{0.22f, 0.22f}, {0.42f, 0.22f}, {0.42f, 0.42f}, {0.22f, 0.42f}};
    for (int i = 0; i < 4; ++i) out[usize(i)] = f.pivot + (a * k[i].x + b * k[i].y) * f.length;
    return out;
}

float distToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    const float t = len2 > 1e-9f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

bool pointInQuad(glm::vec2 p, const std::array<glm::vec2, 4>& q) {
    bool inside = false; // even-odd rule (works for either winding of the projected quad)
    for (usize i = 0, j = 3; i < 4; j = i++) {
        if ((q[i].y > p.y) != (q[j].y > p.y) && p.x < (q[j].x - q[i].x) * (p.y - q[i].y) / (q[j].y - q[i].y) + q[i].x) inside = !inside;
    }
    return inside;
}

GizmoAxis pickTranslate(const Projection& p, const Frame& f, glm::vec2 mouse) {
    glm::vec2 c;
    if (!p.project(f.pivot, c)) return AxisNone;
    const GizmoAxis planes[3] = {AxisYZ, AxisXZ, AxisXY};
    for (int i = 0; i < 3; ++i) {
        const auto corners = planeHandle(f, i);
        std::array<glm::vec2, 4> quad;
        bool ok = true;
        for (usize k = 0; k < 4; ++k) ok &= p.project(corners[k], quad[k]);
        if (ok && pointInQuad(mouse, quad)) return planes[i];
    }
    if (glm::length(mouse - c) < 7.0f) return AxisAll;
    float best = kAxisPickPx;
    GizmoAxis hit = AxisNone;
    for (int i = 0; i < 3; ++i) {
        glm::vec2 tip;
        if (!p.project(f.pivot + f.axis(i) * f.length * 1.12f, tip)) continue;
        const float d = distToSegment(mouse, c, tip);
        if (d < best) {
            best = d;
            hit = GizmoAxis(1 << i);
        }
    }
    return hit;
}

GizmoAxis pickScale(const Projection& p, const Frame& f, glm::vec2 mouse) {
    glm::vec2 c;
    if (!p.project(f.pivot, c)) return AxisNone;
    if (glm::length(mouse - c) < 9.0f) return AxisAll;
    float best = kAxisPickPx;
    GizmoAxis hit = AxisNone;
    for (int i = 0; i < 3; ++i) {
        glm::vec2 tip;
        if (!p.project(f.pivot + f.axis(i) * f.length * 1.12f, tip)) continue;
        const float d = distToSegment(mouse, c, tip);
        if (d < best) {
            best = d;
            hit = GizmoAxis(1 << i);
        }
    }
    return hit;
}

GizmoAxis pickRotate(const Projection& p, const Frame& f, glm::vec2 mouse, glm::vec3 cameraRight) {
    glm::vec2 c;
    if (!p.project(f.pivot, c)) return AxisNone;
    float best = kRingPickPx;
    GizmoAxis hit = AxisNone;
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 u = f.axis((i + 1) % 3), v = f.axis((i + 2) % 3);
        glm::vec2 prev(0.0f);
        bool havePrev = false;
        for (int s = 0; s <= 64; ++s) {
            const float a = float(s) / 64.0f * kTwoPi;
            glm::vec2 q;
            const bool ok = p.project(f.pivot + (u * std::cos(a) + v * std::sin(a)) * f.length, q);
            if (ok && havePrev) {
                const float d = distToSegment(mouse, prev, q);
                if (d < best) {
                    best = d;
                    hit = GizmoAxis(1 << i);
                }
            }
            prev = q;
            havePrev = ok;
        }
    }
    if (hit == AxisNone) {
        glm::vec2 edge;
        if (p.project(f.pivot + cameraRight * f.length * 1.2f, edge) && std::abs(glm::length(mouse - c) - glm::length(edge - c)) < 7.0f) {
            hit = AxisAll; // outer view ring
        }
    }
    return hit;
}

GizmoAxis pick(GizmoMode mode, const Projection& p, const Frame& f, glm::vec2 mouse, glm::vec3 cameraRight) {
    switch (mode) {
    case GizmoMode::Translate: return pickTranslate(p, f, mouse);
    case GizmoMode::Scale: return pickScale(p, f, mouse);
    case GizmoMode::Rotate: return pickRotate(p, f, mouse, cameraRight);
    default: return AxisNone;
    }
}

bool closestOnLine(const Ray& ray, glm::vec3 o, glm::vec3 d, float& t) {
    const glm::vec3 w0 = o - ray.origin;
    const float b = glm::dot(ray.direction, d);
    const float denom = 1.0f - b * b;
    if (std::abs(denom) < 1e-5f) return false;
    const float dd = glm::dot(ray.direction, w0);
    const float e = glm::dot(d, w0);
    t = (b * dd - e) / denom;
    return true;
}

bool rayPlane(const Ray& ray, glm::vec3 p0, glm::vec3 n, glm::vec3& out) {
    const float denom = glm::dot(n, ray.direction);
    if (std::abs(denom) < 1e-5f) return false;
    const float t = glm::dot(p0 - ray.origin, n) / denom;
    if (t < 0.0f) return false;
    out = ray.origin + ray.direction * t;
    return true;
}

float snapTo(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

glm::vec3 axisTranslation(glm::vec3 dir, float startT, float t, float snapStep) { return dir * snapTo(t - startT, snapStep); }

glm::vec3 planeTranslation(glm::vec3 startPoint, glm::vec3 point, glm::quat frame, float snapStep) {
    const glm::vec3 delta = point - startPoint;
    if (snapStep <= 0.0f) return delta;
    glm::vec3 local = glm::inverse(frame) * delta;
    for (int i = 0; i < 3; ++i) local[i] = snapTo(local[i], snapStep);
    return frame * local;
}

float signedAngle(glm::vec3 v0, glm::vec3 v1, glm::vec3 n) { return std::atan2(glm::dot(n, glm::cross(v0, v1)), glm::dot(v0, v1)); }

namespace {
// World-space offset → local position offset (linear part of the inverse parent matrix).
glm::vec3 toParent(const EditTarget& t, glm::vec3 worldDelta) {
    const glm::mat3 p(t.parentWorld);
    if (std::abs(glm::determinant(p)) < 1e-12f) return glm::vec3(0.0f);
    return glm::inverse(p) * worldDelta;
}
} // namespace

Transform applyTranslation(const EditTarget& t, glm::vec3 worldDelta) {
    Transform r = t.local;
    if (worldDelta != glm::vec3(0.0f)) r.position += toParent(t, worldDelta);
    return r;
}

Transform applyRotation(const EditTarget& t, glm::vec3 pivot, glm::quat worldRotation) {
    Transform r = t.local;
    const glm::mat3 p(t.parentWorld);
    if (std::abs(glm::determinant(p)) < 1e-12f) return r;
    // rotation expressed in the parent's (possibly sheared) space: L = P^-1 * dR * P
    const glm::mat3 l = glm::inverse(p) * glm::mat3_cast(worldRotation) * p;
    r.rotation = frameRotation(l * glm::mat3_cast(t.local.rotation));
    const glm::vec3 pos0 = t.worldPosition();
    const glm::vec3 offset = pos0 - pivot;
    if (glm::dot(offset, offset) > 1e-12f) r.position += toParent(t, pivot + worldRotation * offset - pos0);
    return r;
}

Transform applyScale(const EditTarget& t, glm::vec3 pivot, glm::quat frame, glm::vec3 factors, bool aroundPivot) {
    Transform r = t.local;
    r.scale = t.local.scale * factors;
    if (aroundPivot) {
        const glm::vec3 pos0 = t.worldPosition();
        const glm::vec3 offset = pos0 - pivot;
        if (glm::dot(offset, offset) > 1e-12f) {
            const glm::vec3 moved = pivot + frame * (factors * (glm::inverse(frame) * offset));
            r.position += toParent(t, moved - pos0);
        }
    }
    return r;
}

} // namespace ox::editor::gizmo
