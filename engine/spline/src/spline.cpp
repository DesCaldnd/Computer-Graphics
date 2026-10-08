#include <oxwald/spline/spline.hpp>

#include "nurbs.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ox::spline {

namespace {

using detail::kSubSteps;
constexpr f32 kEpsilon = 1e-6f;

// 5-point Gauss-Legendre on [-1, 1].
constexpr f32 kGaussX[5] = {0.0f, -0.5384693101056831f, 0.5384693101056831f, -0.9061798459386640f,
                            0.9061798459386640f};
constexpr f32 kGaussW[5] = {0.5688888888888889f, 0.4786286704993665f, 0.4786286704993665f, 0.2369268850561891f,
                            0.2369268850561891f};

glm::vec3 rotateAround(const glm::vec3& v, const glm::vec3& axis, f32 angle) {
    if (angle == 0.0f) {
        return v;
    }
    return glm::angleAxis(angle, axis) * v;
}

glm::vec3 anyPerpendicular(const glm::vec3& t) {
    const glm::vec3 a = glm::abs(t);
    const glm::vec3 axis = (a.x <= a.y && a.x <= a.z) ? glm::vec3(1, 0, 0)
                           : (a.y <= a.z)             ? glm::vec3(0, 1, 0)
                                                      : glm::vec3(0, 0, 1);
    return glm::normalize(axis - glm::dot(axis, t) * t);
}

glm::vec3 projectPerpendicular(const glm::vec3& v, const glm::vec3& t) {
    const glm::vec3 p = v - glm::dot(v, t) * t;
    const f32 len = glm::length(p);
    return len > 1e-5f ? p / len : anyPerpendicular(t);
}

// Double reflection step (Wang, Jüttler, Zheng, Liu 2008: "Computation of rotation minimizing frames").
glm::vec3 doubleReflect(const glm::vec3& x0, const glm::vec3& t0, const glm::vec3& r0, const glm::vec3& x1,
                        const glm::vec3& t1) {
    const glm::vec3 v1 = x1 - x0;
    const f32 c1 = glm::dot(v1, v1);
    glm::vec3 rL = r0;
    glm::vec3 tL = t0;
    if (c1 > 1e-14f) {
        rL = r0 - (2.0f / c1) * glm::dot(v1, r0) * v1;
        tL = t0 - (2.0f / c1) * glm::dot(v1, t0) * v1;
    }
    const glm::vec3 v2 = t1 - tL;
    const f32 c2 = glm::dot(v2, v2);
    if (c2 > 1e-14f) {
        rL = rL - (2.0f / c2) * glm::dot(v2, rL) * v2;
    }
    return projectPerpendicular(rL, t1);
}

f32 boxDistanceSq(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec3 d = glm::max(glm::max(lo - p, glm::vec3(0.0f)), p - hi);
    return glm::dot(d, d);
}

} // namespace

glm::quat frameRotation(const glm::vec3& forward, const glm::vec3& up, const glm::vec3& localForward,
                        const glm::vec3& localUp) {
    const f32 fl = glm::length(forward);
    const glm::vec3 f = fl > kEpsilon ? forward / fl : glm::vec3(0, 0, -1);
    const glm::vec3 u = projectPerpendicular(up, f);
    const glm::vec3 lf = glm::normalize(localForward);
    const glm::vec3 lu = projectPerpendicular(localUp, lf);
    const glm::mat3 world(f, u, glm::cross(f, u));
    const glm::mat3 local(lf, lu, glm::cross(lf, lu));
    return glm::normalize(glm::quat_cast(world * glm::transpose(local)));
}

glm::quat SplineSample::rotation(const glm::vec3& localForward, const glm::vec3& localUp) const {
    return frameRotation(tangent, normal, localForward, localUp);
}

// ---------------------------------------------------------------------------------------------------------
// Editing

Spline::Spline(SplineType type, bool closed) : m_type(type), m_closed(closed) {}

void Spline::touch() {
    m_cache.dirty = true;
    ++m_version;
}

void Spline::setType(SplineType type) {
    m_type = type;
    recomputeAutoHandles();
    touch();
}

void Spline::setClosed(bool closed) {
    m_closed = closed;
    recomputeAutoHandles();
    touch();
}

void Spline::setSettings(SplineSettings settings) {
    m_settings = std::move(settings);
    touch();
}

const ControlPoint& Spline::point(usize index) const {
    OX_ASSERT(index < m_points.size(), "point index {} out of range {}", index, m_points.size());
    return m_points[index];
}

void Spline::setPoints(std::vector<ControlPoint> points) {
    m_points = std::move(points);
    recomputeAutoHandles();
    touch();
}

void Spline::addPoint(const glm::vec3& position) {
    ControlPoint p;
    p.position = position;
    addPoint(p);
}

void Spline::addPoint(const ControlPoint& point) {
    insertPoint(m_points.size(), point);
}

void Spline::insertPoint(usize index, const ControlPoint& point) {
    OX_ASSERT(index <= m_points.size());
    m_points.insert(m_points.begin() + static_cast<std::ptrdiff_t>(index), point);
    recomputeAutoHandles();
    touch();
}

void Spline::removePoint(usize index) {
    OX_ASSERT(index < m_points.size());
    m_points.erase(m_points.begin() + static_cast<std::ptrdiff_t>(index));
    recomputeAutoHandles();
    touch();
}

void Spline::clear() {
    m_points.clear();
    m_markers.clear();
    touch();
}

void Spline::setPoint(usize index, const ControlPoint& point) {
    OX_ASSERT(index < m_points.size());
    m_points[index] = point;
    recomputeAutoHandles();
    touch();
}

void Spline::setPosition(usize index, const glm::vec3& position) {
    OX_ASSERT(index < m_points.size());
    m_points[index].position = position;
    recomputeAutoHandles();
    touch();
}

void Spline::setInHandle(usize index, const glm::vec3& offset) {
    OX_ASSERT(index < m_points.size());
    m_points[index].inHandle = offset;
    enforceHandleMode(index, false);
    touch();
}

void Spline::setOutHandle(usize index, const glm::vec3& offset) {
    OX_ASSERT(index < m_points.size());
    m_points[index].outHandle = offset;
    enforceHandleMode(index, true);
    touch();
}

void Spline::enforceHandleMode(usize index, bool outMoved) {
    ControlPoint& cp = m_points[index];
    if (cp.handleMode == HandleMode::Auto) {
        cp.handleMode = HandleMode::Aligned;
    }
    const glm::vec3 moved = outMoved ? cp.outHandle : cp.inHandle;
    glm::vec3& other = outMoved ? cp.inHandle : cp.outHandle;
    switch (cp.handleMode) {
    case HandleMode::Free:
    case HandleMode::Auto:
        break;
    case HandleMode::Aligned: {
        const f32 len = glm::length(moved);
        if (len > kEpsilon) {
            other = -moved / len * glm::length(other);
        }
        break;
    }
    case HandleMode::Mirrored:
        other = -moved;
        break;
    }
}

void Spline::setHandleMode(usize index, HandleMode mode) {
    OX_ASSERT(index < m_points.size());
    ControlPoint& cp = m_points[index];
    cp.handleMode = mode;
    if (mode == HandleMode::Mirrored) {
        cp.inHandle = -cp.outHandle;
    } else if (mode == HandleMode::Aligned) {
        enforceHandleMode(index, true);
    } else if (mode == HandleMode::Auto) {
        recomputeAutoHandles();
    }
    touch();
}

void Spline::setRoll(usize index, f32 roll) {
    OX_ASSERT(index < m_points.size());
    m_points[index].roll = roll;
    touch();
}

void Spline::setWeight(usize index, f32 weight) {
    OX_ASSERT(index < m_points.size());
    OX_ASSERT(weight > 0.0f, "NURBS weights must be positive");
    m_points[index].weight = weight;
    touch();
}

void Spline::setUp(usize index, std::optional<glm::vec3> up) {
    OX_ASSERT(index < m_points.size());
    m_points[index].up = up;
    touch();
}

void Spline::recomputeAutoHandles() {
    const usize n = m_points.size();
    const bool closed = m_closed && n >= 3;
    for (usize i = 0; i < n; ++i) {
        ControlPoint& cp = m_points[i];
        if (cp.handleMode != HandleMode::Auto) {
            continue;
        }
        if (n < 2) {
            cp.inHandle = cp.outHandle = glm::vec3(0.0f);
            continue;
        }
        // Uniform Catmull-Rom tangent: equal handle lengths on both sides give C1 at the point.
        glm::vec3 out;
        if (closed || (i > 0 && i + 1 < n)) {
            const glm::vec3& prev = m_points[(i + n - 1) % n].position;
            const glm::vec3& next = m_points[(i + 1) % n].position;
            out = (next - prev) / 6.0f;
        } else if (i == 0) {
            out = (m_points[1].position - cp.position) / 3.0f;
        } else {
            out = (cp.position - m_points[n - 2].position) / 3.0f;
        }
        cp.outHandle = out;
        cp.inHandle = -out;
    }
}

usize Spline::insertPointAt(f32 t) {
    const u32 segs = segmentCount();
    if (segs == 0) {
        addPoint(position(t));
        return m_points.size() - 1;
    }
    const Location loc = locate(t);
    if (m_type == SplineType::Bezier) {
        const usize n = m_points.size();
        const usize i = loc.segment;
        const usize j = (i + 1) % n;
        const f32 u = std::clamp(loc.u, 1e-4f, 1.0f - 1e-4f);
        const auto [left, right] = splitBezier(m_cache.segments[i].bezier, u);
        auto keepShape = [](ControlPoint& cp) {
            if (cp.handleMode == HandleMode::Auto || cp.handleMode == HandleMode::Mirrored) {
                cp.handleMode = HandleMode::Aligned;
            }
        };
        keepShape(m_points[i]);
        keepShape(m_points[j]);
        m_points[i].outHandle = left[1] - left[0];
        m_points[j].inHandle = right[2] - right[3];

        ControlPoint mid;
        mid.position = left[3];
        mid.inHandle = left[2] - left[3];
        mid.outHandle = right[1] - right[0];
        mid.handleMode = HandleMode::Aligned;
        mid.roll = glm::mix(m_points[i].roll, m_points[j].roll, u);
        mid.weight = 1.0f;
        if (m_points[i].up || m_points[j].up) {
            mid.up = upAt(t);
        }
        for (SplineMarker& m : m_markers) {
            const Location ml = locate(m.t);
            if (ml.segment > i) {
                m.t += 1.0f;
            } else if (ml.segment == i) {
                m.t = ml.u < u ? static_cast<f32>(i) + ml.u / u
                               : static_cast<f32>(i + 1) + (ml.u - u) / (1.0f - u);
            }
        }
        const usize index = i + 1;
        m_points.insert(m_points.begin() + static_cast<std::ptrdiff_t>(index), mid);
        touch();
        return index;
    }

    const usize n = m_points.size();
    const bool closed = m_closed && n >= 3;
    const f32 pointSpan = static_cast<f32>(closed ? n : n - 1);
    const f32 tw = static_cast<f32>(loc.segment) + loc.u;
    const usize index = std::min(static_cast<usize>(std::floor(tw * pointSpan / static_cast<f32>(segs))) + 1, n);
    ControlPoint cp;
    cp.position = position(t);
    cp.roll = rollAt(tw);
    insertPoint(index, cp);
    return index;
}

// ---------------------------------------------------------------------------------------------------------
// Markers

void Spline::addMarker(std::string name, f32 t) {
    m_markers.push_back(SplineMarker{std::move(name), t});
    ++m_version;
}

bool Spline::removeMarker(std::string_view name) {
    const auto it = std::find_if(m_markers.begin(), m_markers.end(), [&](const SplineMarker& m) { return m.name == name; });
    if (it == m_markers.end()) {
        return false;
    }
    m_markers.erase(it);
    ++m_version;
    return true;
}

std::optional<f32> Spline::markerT(std::string_view name) const {
    for (const SplineMarker& m : m_markers) {
        if (m.name == name) {
            return m.t;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------
// Cache

void Spline::rebuild() const {
    m_cache.dirty = true;
    ensureCache();
}

void Spline::ensureCache() const {
    if (!m_cache.dirty) {
        return;
    }
    m_cache.segments.clear();
    m_cache.homogeneous.clear();
    m_cache.knots.clear();
    m_cache.frames.clear();
    m_cache.length = 0.0f;
    m_cache.cubic = m_type == SplineType::Linear || m_type == SplineType::Bezier || m_type == SplineType::CatmullRom;
    if (m_cache.cubic) {
        buildCubicSegments();
    } else {
        buildNurbsSegments();
    }
    // Arc length and frames evaluate the segments, so the cache must already look clean.
    m_cache.dirty = false;
    buildArcLength();
    buildFrames();
}

void Spline::buildCubicSegments() const {
    const usize n = m_points.size();
    if (n < 2) {
        return;
    }
    const bool closed = m_closed && n >= 3;
    const usize segs = closed ? n : n - 1;
    const f32 alpha = m_settings.catmullRomAlpha;
    auto pos = [&](std::ptrdiff_t i) -> glm::vec3 {
        const auto sn = static_cast<std::ptrdiff_t>(n);
        if (closed) {
            return m_points[static_cast<usize>(((i % sn) + sn) % sn)].position;
        }
        // Open Catmull-Rom ends use mirrored phantom points.
        if (i < 0) {
            return 2.0f * m_points[0].position - m_points[1].position;
        }
        if (i >= sn) {
            return 2.0f * m_points[n - 1].position - m_points[n - 2].position;
        }
        return m_points[static_cast<usize>(i)].position;
    };

    m_cache.segments.resize(segs);
    for (usize s = 0; s < segs; ++s) {
        const usize j = (s + 1) % n;
        CubicBezier& b = m_cache.segments[s].bezier;
        const glm::vec3 a = m_points[s].position;
        const glm::vec3 c = m_points[j].position;
        switch (m_type) {
        case SplineType::Linear:
            b = {a, a + (c - a) / 3.0f, a + 2.0f * (c - a) / 3.0f, c};
            break;
        case SplineType::Bezier:
            b = {a, a + m_points[s].outHandle, c + m_points[j].inHandle, c};
            break;
        case SplineType::CatmullRom: {
            const auto si = static_cast<std::ptrdiff_t>(s);
            const glm::vec3 p0 = pos(si - 1);
            const glm::vec3 p3 = pos(si + 2);
            const f32 rawD1 = glm::length(c - a);
            if (rawD1 < kEpsilon) {
                b = {a, a, c, c};
                break;
            }
            // Non-uniform Catmull-Rom as Hermite tangents (Yuksel et al. parametrisation, cusp-free for α=0.5).
            const f32 d0 = std::max(std::pow(glm::length(a - p0), alpha), 1e-4f);
            const f32 d1 = std::max(std::pow(rawD1, alpha), 1e-4f);
            const f32 d2 = std::max(std::pow(glm::length(p3 - c), alpha), 1e-4f);
            glm::vec3 m1 = (a - p0) / d0 - (c - p0) / (d0 + d1) + (c - a) / d1;
            glm::vec3 m2 = (c - a) / d1 - (p3 - a) / (d1 + d2) + (p3 - c) / d2;
            m1 *= d1;
            m2 *= d1;
            b = hermiteToBezier(a, m1, c, m2);
            break;
        }
        default:
            OX_UNREACHABLE();
        }
        detail::Segment& seg = m_cache.segments[s];
        seg.hullMin = glm::min(glm::min(b[0], b[1]), glm::min(b[2], b[3]));
        seg.hullMax = glm::max(glm::max(b[0], b[1]), glm::max(b[2], b[3]));
    }
}

void Spline::buildNurbsSegments() const {
    const usize n = m_points.size();
    if (n < 2) {
        return;
    }
    const bool closed = m_closed && n >= 3;
    const bool rational = m_type == SplineType::Nurbs;
    u32 p = rational ? m_settings.degree : 3u;
    p = std::clamp<u32>(p, 1u, std::min<u32>(kMaxNurbsDegree, static_cast<u32>(n - 1)));
    m_cache.degree = p;

    auto homogeneous = [&](usize i) {
        const f32 w = rational ? std::max(m_points[i].weight, 1e-6f) : 1.0f;
        return glm::vec4(m_points[i].position * w, w);
    };
    std::vector<glm::vec4>& hw = m_cache.homogeneous;
    std::vector<f32>& knots = m_cache.knots;

    if (closed) {
        // Periodic: wrap p points and use a uniform unclamped knot vector; domain [p, n+p].
        for (usize i = 0; i < n + p; ++i) {
            hw.push_back(homogeneous(i % n));
        }
        for (usize i = 0; i < n + 2 * p + 1; ++i) {
            knots.push_back(static_cast<f32>(i));
        }
        if (rational && !m_settings.knots.empty()) {
            OX_LOG_WARN("spline", "custom NURBS knots are ignored for closed splines");
        }
    } else {
        for (usize i = 0; i < n; ++i) {
            hw.push_back(homogeneous(i));
        }
        const usize knotCount = n + p + 1;
        const std::vector<f32>& custom = m_settings.knots;
        bool useCustom = rational && !custom.empty();
        if (useCustom && (custom.size() != knotCount || !std::is_sorted(custom.begin(), custom.end()))) {
            OX_LOG_WARN("spline", "custom NURBS knot vector must be non-decreasing with {} entries (got {}), using clamped uniform",
                        knotCount, custom.size());
            useCustom = false;
        }
        if (useCustom) {
            knots = custom;
        } else {
            const usize interior = n - p; // number of spans
            for (usize i = 0; i <= p; ++i) {
                knots.push_back(0.0f);
            }
            for (usize i = 1; i < interior; ++i) {
                knots.push_back(static_cast<f32>(i) / static_cast<f32>(interior));
            }
            for (usize i = 0; i <= p; ++i) {
                knots.push_back(1.0f);
            }
        }
    }

    const usize lastSpan = hw.size() - 1;
    for (usize span = p; span <= lastSpan; ++span) {
        if (knots[span + 1] <= knots[span]) {
            continue;
        }
        detail::Segment seg;
        seg.span = static_cast<u32>(span);
        seg.u0 = knots[span];
        seg.u1 = knots[span + 1];
        seg.hullMin = glm::vec3(std::numeric_limits<f32>::max());
        seg.hullMax = glm::vec3(-std::numeric_limits<f32>::max());
        for (usize k = span - p; k <= span; ++k) {
            const glm::vec3 cp = glm::vec3(hw[k]) / hw[k].w;
            seg.hullMin = glm::min(seg.hullMin, cp);
            seg.hullMax = glm::max(seg.hullMax, cp);
        }
        m_cache.segments.push_back(seg);
    }
}

void Spline::evalSegment(u32 segment, f32 u, glm::vec3* p, glm::vec3* d1, glm::vec3* d2) const {
    const detail::Segment& seg = m_cache.segments[segment];
    if (m_cache.cubic) {
        if (p) {
            *p = bezierPosition(seg.bezier, u);
        }
        if (d1) {
            *d1 = bezierDerivative(seg.bezier, u);
        }
        if (d2) {
            *d2 = bezierSecondDerivative(seg.bezier, u);
        }
        return;
    }
    const f32 du = seg.u1 - seg.u0;
    detail::nurbsEvaluate(m_cache.homogeneous, m_cache.knots, m_cache.degree, seg.span, seg.u0 + u * du, p, d1, d2);
    if (d1) {
        *d1 *= du;
    }
    if (d2) {
        *d2 *= du * du;
    }
}

glm::vec3 Spline::safeTangent(u32 segment, f32 u) const {
    glm::vec3 d1;
    evalSegment(segment, u, nullptr, &d1, nullptr);
    const f32 len = glm::length(d1);
    if (len > 1e-5f) {
        return d1 / len;
    }
    // Zero-length handles create a vanishing derivative at segment ends; fall back to a short chord.
    glm::vec3 a;
    glm::vec3 b;
    evalSegment(segment, std::max(u - 1e-3f, 0.0f), &a, nullptr, nullptr);
    evalSegment(segment, std::min(u + 1e-3f, 1.0f), &b, nullptr, nullptr);
    const glm::vec3 chord = b - a;
    const f32 cl = glm::length(chord);
    return cl > 1e-9f ? chord / cl : glm::vec3(0.0f, 0.0f, -1.0f);
}

void Spline::buildArcLength() const {
    f32 total = 0.0f;
    for (u32 s = 0; s < m_cache.segments.size(); ++s) {
        detail::Segment& seg = m_cache.segments[s];
        seg.startDistance = total;
        seg.lut[0] = 0.0f;
        for (u32 k = 0; k < kSubSteps; ++k) {
            const f32 a = static_cast<f32>(k) / kSubSteps;
            const f32 b = static_cast<f32>(k + 1) / kSubSteps;
            f32 sum = 0.0f;
            for (int g = 0; g < 5; ++g) {
                glm::vec3 d1;
                evalSegment(s, 0.5f * (a + b) + 0.5f * (b - a) * kGaussX[g], nullptr, &d1, nullptr);
                sum += kGaussW[g] * glm::length(d1);
            }
            seg.lut[k + 1] = seg.lut[k] + 0.5f * (b - a) * sum;
        }
        seg.length = seg.lut[kSubSteps];
        total += seg.length;
    }
    m_cache.length = total;
}

void Spline::buildFrames() const {
    const u32 segs = static_cast<u32>(m_cache.segments.size());
    if (segs == 0) {
        return;
    }
    const u32 count = segs * kSubSteps + 1;
    std::vector<detail::FrameSample>& frames = m_cache.frames;
    frames.resize(count);
    for (u32 k = 0; k < count; ++k) {
        const u32 s = std::min(k / kSubSteps, segs - 1);
        const f32 u = static_cast<f32>(k - s * kSubSteps) / kSubSteps;
        evalSegment(s, u, &frames[k].position, nullptr, nullptr);
        frames[k].tangent = safeTangent(s, u);
    }
    frames[0].normal = projectPerpendicular(m_settings.upVector, frames[0].tangent);
    for (u32 k = 1; k < count; ++k) {
        frames[k].normal = doubleReflect(frames[k - 1].position, frames[k - 1].tangent, frames[k - 1].normal,
                                         frames[k].position, frames[k].tangent);
    }
    if (m_closed && m_points.size() >= 3 && m_cache.length > kEpsilon) {
        // Parallel transport around a loop generally does not return to the start frame (holonomy);
        // spread the mismatch along the arc length so the seam is continuous.
        const glm::vec3 t0 = frames[0].tangent;
        const glm::vec3 n0 = frames[0].normal;
        const glm::vec3 nEnd = projectPerpendicular(frames[count - 1].normal, t0);
        const f32 angle = std::atan2(glm::dot(glm::cross(nEnd, n0), t0), glm::dot(nEnd, n0));
        for (u32 k = 0; k < count; ++k) {
            const u32 s = std::min(k / kSubSteps, segs - 1);
            const detail::Segment& seg = m_cache.segments[s];
            const f32 dist = seg.startDistance + seg.lut[k - s * kSubSteps];
            frames[k].correction = angle * dist / m_cache.length;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Evaluation

u32 Spline::segmentCount() const {
    ensureCache();
    return static_cast<u32>(m_cache.segments.size());
}

Spline::Location Spline::locate(f32 t) const {
    ensureCache();
    const u32 segs = static_cast<u32>(m_cache.segments.size());
    if (segs == 0) {
        return {};
    }
    const f32 maxT = static_cast<f32>(segs);
    if (!std::isfinite(t)) {
        t = 0.0f;
    }
    if (m_closed && m_points.size() >= 3) {
        if (t < 0.0f || t > maxT) {
            t = std::fmod(t, maxT);
            if (t < 0.0f) {
                t += maxT;
            }
        }
    } else {
        t = std::clamp(t, 0.0f, maxT);
    }
    const u32 s = std::min(static_cast<u32>(t), segs - 1);
    return {s, std::clamp(t - static_cast<f32>(s), 0.0f, 1.0f)};
}

glm::vec3 Spline::position(f32 t) const {
    ensureCache();
    if (m_cache.segments.empty()) {
        return m_points.empty() ? glm::vec3(0.0f) : m_points[0].position;
    }
    const Location loc = locate(t);
    glm::vec3 p;
    evalSegment(loc.segment, loc.u, &p, nullptr, nullptr);
    return p;
}

glm::vec3 Spline::derivative(f32 t) const {
    ensureCache();
    if (m_cache.segments.empty()) {
        return glm::vec3(0.0f);
    }
    const Location loc = locate(t);
    glm::vec3 d;
    evalSegment(loc.segment, loc.u, nullptr, &d, nullptr);
    return d;
}

glm::vec3 Spline::secondDerivative(f32 t) const {
    ensureCache();
    if (m_cache.segments.empty()) {
        return glm::vec3(0.0f);
    }
    const Location loc = locate(t);
    glm::vec3 d;
    evalSegment(loc.segment, loc.u, nullptr, nullptr, &d);
    return d;
}

glm::vec3 Spline::tangent(f32 t) const {
    ensureCache();
    if (m_cache.segments.empty()) {
        return glm::vec3(0.0f, 0.0f, -1.0f);
    }
    const Location loc = locate(t);
    return safeTangent(loc.segment, loc.u);
}

f32 Spline::curvature(f32 t) const {
    ensureCache();
    if (m_cache.segments.empty()) {
        return 0.0f;
    }
    const Location loc = locate(t);
    glm::vec3 d1;
    glm::vec3 d2;
    evalSegment(loc.segment, loc.u, nullptr, &d1, &d2);
    const f32 speed = glm::length(d1);
    return speed > 1e-6f ? glm::length(glm::cross(d1, d2)) / (speed * speed * speed) : 0.0f;
}

f32 Spline::rollAt(f32 t) const {
    const usize n = m_points.size();
    const u32 segs = static_cast<u32>(m_cache.segments.size());
    if (n == 0) {
        return 0.0f;
    }
    if (n == 1 || segs == 0) {
        return m_points[0].roll;
    }
    const bool closed = m_closed && n >= 3;
    const f32 f = t * static_cast<f32>(closed ? n : n - 1) / static_cast<f32>(segs);
    const usize i = static_cast<usize>(std::max(std::floor(f), 0.0f));
    const f32 frac = f - static_cast<f32>(i);
    if (!closed && i + 1 >= n) {
        return m_points[n - 1].roll;
    }
    return glm::mix(m_points[i % n].roll, m_points[(i + 1) % n].roll, frac);
}

glm::vec3 Spline::upAt(f32 t) const {
    const usize n = m_points.size();
    const u32 segs = static_cast<u32>(m_cache.segments.size());
    auto up = [&](usize i) { return m_points[i].up ? glm::normalize(*m_points[i].up) : m_settings.upVector; };
    if (n == 0) {
        return m_settings.upVector;
    }
    if (n == 1 || segs == 0) {
        return up(0);
    }
    const bool closed = m_closed && n >= 3;
    const f32 f = t * static_cast<f32>(closed ? n : n - 1) / static_cast<f32>(segs);
    const usize i = static_cast<usize>(std::max(std::floor(f), 0.0f));
    const f32 frac = f - static_cast<f32>(i);
    if (!closed && i + 1 >= n) {
        return up(n - 1);
    }
    const glm::vec3 v = glm::mix(up(i % n), up((i + 1) % n), frac);
    const f32 len = glm::length(v);
    return len > kEpsilon ? v / len : up(i % n);
}

SplineSample Spline::evaluate(f32 t) const {
    ensureCache();
    SplineSample out;
    out.t = t;
    if (m_cache.segments.empty()) {
        out.position = position(t);
        return out;
    }
    const Location loc = locate(t);
    const f32 tw = static_cast<f32>(loc.segment) + loc.u;
    out.t = tw;
    evalSegment(loc.segment, loc.u, &out.position, &out.derivative, &out.secondDerivative);
    const f32 speed = glm::length(out.derivative);
    out.curvature = speed > 1e-6f ? glm::length(glm::cross(out.derivative, out.secondDerivative)) / (speed * speed * speed)
                                  : 0.0f;
    const glm::vec3 tan = safeTangent(loc.segment, loc.u);
    out.tangent = tan;

    glm::vec3 normal(0.0f);
    bool haveNormal = false;
    if (m_settings.frameMode == FrameMode::UpVector) {
        const glm::vec3 up = upAt(tw);
        const glm::vec3 nrm = up - glm::dot(up, tan) * tan;
        const f32 len = glm::length(nrm);
        if (len > 1e-4f) {
            normal = nrm / len;
            haveNormal = true;
        }
    }
    if (!haveNormal) {
        const f32 sub = loc.u * kSubSteps;
        const u32 local = std::min(static_cast<u32>(sub), kSubSteps - 1);
        const u32 k = loc.segment * kSubSteps + local;
        const detail::FrameSample& a = m_cache.frames[k];
        const detail::FrameSample& b = m_cache.frames[k + 1];
        normal = doubleReflect(a.position, a.tangent, a.normal, out.position, tan);
        const f32 correction = glm::mix(a.correction, b.correction, sub - static_cast<f32>(local));
        normal = rotateAround(normal, tan, correction);
    }
    out.roll = rollAt(tw);
    normal = projectPerpendicular(rotateAround(normal, tan, out.roll), tan);
    out.normal = normal;
    out.binormal = glm::normalize(glm::cross(tan, normal));
    return out;
}

// ---------------------------------------------------------------------------------------------------------
// Arc length

f32 Spline::length() const {
    ensureCache();
    return m_cache.length;
}

f32 Spline::segmentLength(u32 segment) const {
    ensureCache();
    return segment < m_cache.segments.size() ? m_cache.segments[segment].length : 0.0f;
}

f32 Spline::segmentDistance(u32 segment, f32 u) const {
    const detail::Segment& seg = m_cache.segments[segment];
    const u32 k = std::min(static_cast<u32>(u * kSubSteps), kSubSteps - 1);
    const f32 a = static_cast<f32>(k) / kSubSteps;
    f32 sum = 0.0f;
    if (u > a) {
        for (int g = 0; g < 5; ++g) {
            glm::vec3 d1;
            evalSegment(segment, 0.5f * (a + u) + 0.5f * (u - a) * kGaussX[g], nullptr, &d1, nullptr);
            sum += kGaussW[g] * glm::length(d1);
        }
        sum *= 0.5f * (u - a);
    }
    return seg.lut[k] + sum;
}

f32 Spline::tToDistance(f32 t) const {
    ensureCache();
    if (m_cache.segments.empty()) {
        return 0.0f;
    }
    const Location loc = locate(t);
    return m_cache.segments[loc.segment].startDistance + segmentDistance(loc.segment, loc.u);
}

f32 Spline::distanceToT(f32 distance) const {
    ensureCache();
    const f32 total = m_cache.length;
    if (m_cache.segments.empty() || total <= 0.0f) {
        return 0.0f;
    }
    if (m_closed && m_points.size() >= 3 && (distance < 0.0f || distance > total)) {
        distance = std::fmod(distance, total);
        if (distance < 0.0f) {
            distance += total;
        }
    }
    return distanceToTClamped(distance);
}

f32 Spline::distanceToTClamped(f32 distance) const {
    const std::vector<detail::Segment>& segs = m_cache.segments;
    distance = std::clamp(distance, 0.0f, m_cache.length);
    const auto it = std::upper_bound(segs.begin(), segs.end(), distance,
                                     [](f32 d, const detail::Segment& s) { return d < s.startDistance; });
    const u32 s = static_cast<u32>(std::max<std::ptrdiff_t>(it - segs.begin() - 1, 0));
    const detail::Segment& seg = segs[s];
    const f32 local = std::clamp(distance - seg.startDistance, 0.0f, seg.length);
    if (seg.length <= kEpsilon) {
        return static_cast<f32>(s);
    }
    const auto lutIt = std::upper_bound(seg.lut.begin(), seg.lut.end(), local);
    const u32 k = static_cast<u32>(std::clamp<std::ptrdiff_t>(lutIt - seg.lut.begin() - 1, 0, kSubSteps - 1));
    f32 lo = static_cast<f32>(k) / kSubSteps;
    f32 hi = static_cast<f32>(k + 1) / kSubSteps;
    const f32 subLen = seg.lut[k + 1] - seg.lut[k];
    f32 u = subLen > 0.0f ? lo + (hi - lo) * (local - seg.lut[k]) / subLen : lo;
    const f32 tolerance = 1e-6f * std::max(1.0f, m_cache.length);
    // Newton on L(u) - d with the speed as derivative, bracketed by the LUT interval.
    for (int iter = 0; iter < 10; ++iter) {
        const f32 err = segmentDistance(s, u) - local;
        if (std::abs(err) <= tolerance) {
            break;
        }
        if (err > 0.0f) {
            hi = u;
        } else {
            lo = u;
        }
        glm::vec3 d1;
        evalSegment(s, u, nullptr, &d1, nullptr);
        const f32 speed = glm::length(d1);
        f32 next = speed > kEpsilon ? u - err / speed : 0.5f * (lo + hi);
        if (next <= lo || next >= hi) {
            next = 0.5f * (lo + hi);
        }
        u = next;
    }
    return static_cast<f32>(s) + u;
}

std::vector<f32> Spline::uniformParameters(u32 intervals) const {
    ensureCache();
    intervals = std::max(intervals, 1u);
    std::vector<f32> ts(intervals + 1);
    for (u32 k = 0; k <= intervals; ++k) {
        ts[k] = distanceToTClamped(m_cache.length * static_cast<f32>(k) / static_cast<f32>(intervals));
    }
    ts.back() = maxT();
    return ts;
}

std::vector<SplineSample> Spline::sampleByDistance(f32 spacing) const {
    const f32 total = length();
    if (total <= 0.0f || spacing <= 0.0f) {
        return {evaluate(0.0f)};
    }
    const u32 intervals = std::max(1u, static_cast<u32>(std::lround(total / spacing)));
    std::vector<SplineSample> out;
    out.reserve(intervals + 1);
    for (f32 t : uniformParameters(intervals)) {
        out.push_back(evaluate(t));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------
// Queries

ClosestPointResult Spline::closestPoint(const glm::vec3& q) const {
    ensureCache();
    ClosestPointResult best;
    const u32 segs = static_cast<u32>(m_cache.segments.size());
    if (segs == 0) {
        best.position = position(0.0f);
        best.distance = glm::length(best.position - q);
        return best;
    }
    struct Candidate {
        f32 boundSq;
        u32 segment;
    };
    std::vector<Candidate> order(segs);
    for (u32 s = 0; s < segs; ++s) {
        order[s] = {boxDistanceSq(q, m_cache.segments[s].hullMin, m_cache.segments[s].hullMax), s};
    }
    std::sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) { return a.boundSq < b.boundSq; });

    f32 bestSq = std::numeric_limits<f32>::max();
    auto distSq = [&](u32 s, f32 u) {
        glm::vec3 p;
        evalSegment(s, u, &p, nullptr, nullptr);
        const glm::vec3 d = p - q;
        return glm::dot(d, d);
    };
    constexpr u32 kCoarse = 16;
    for (const Candidate& c : order) {
        if (c.boundSq >= bestSq) {
            break; // hull boxes are conservative, nothing further can be closer
        }
        const u32 s = c.segment;
        u32 bestK = 0;
        f32 coarseSq = std::numeric_limits<f32>::max();
        for (u32 k = 0; k <= kCoarse; ++k) {
            const f32 d = distSq(s, static_cast<f32>(k) / kCoarse);
            if (d < coarseSq) {
                coarseSq = d;
                bestK = k;
            }
        }
        // Golden-section in the bracket around the coarse minimum, then a Newton polish.
        f32 lo = static_cast<f32>(bestK == 0 ? 0 : bestK - 1) / kCoarse;
        f32 hi = static_cast<f32>(std::min(bestK + 1, kCoarse)) / kCoarse;
        constexpr f32 kInvPhi = 0.6180339887f;
        f32 x1 = hi - kInvPhi * (hi - lo);
        f32 x2 = lo + kInvPhi * (hi - lo);
        f32 f1 = distSq(s, x1);
        f32 f2 = distSq(s, x2);
        for (int i = 0; i < 24; ++i) {
            if (f1 < f2) {
                hi = x2;
                x2 = x1;
                f2 = f1;
                x1 = hi - kInvPhi * (hi - lo);
                f1 = distSq(s, x1);
            } else {
                lo = x1;
                x1 = x2;
                f1 = f2;
                x2 = lo + kInvPhi * (hi - lo);
                f2 = distSq(s, x2);
            }
        }
        f32 u = 0.5f * (lo + hi);
        f32 uSq = distSq(s, u);
        for (int i = 0; i < 3; ++i) {
            glm::vec3 p;
            glm::vec3 d1;
            glm::vec3 d2;
            evalSegment(s, u, &p, &d1, &d2);
            const f32 g = glm::dot(p - q, d1);
            const f32 h = glm::dot(d1, d1) + glm::dot(p - q, d2);
            if (h <= kEpsilon) {
                break;
            }
            const f32 next = std::clamp(u - g / h, 0.0f, 1.0f);
            const f32 nextSq = distSq(s, next);
            if (nextSq >= uSq) {
                break;
            }
            u = next;
            uSq = nextSq;
        }
        if (coarseSq < uSq) {
            u = static_cast<f32>(bestK) / kCoarse;
            uSq = coarseSq;
        }
        if (uSq < bestSq) {
            bestSq = uSq;
            best.t = static_cast<f32>(s) + u;
        }
    }
    best.position = position(best.t);
    best.distance = std::sqrt(bestSq);
    return best;
}

Bounds Spline::bounds() const {
    ensureCache();
    Bounds b;
    if (m_cache.segments.empty()) {
        b.min = b.max = position(0.0f);
        return b;
    }
    b.min = glm::vec3(std::numeric_limits<f32>::max());
    b.max = glm::vec3(-std::numeric_limits<f32>::max());
    auto include = [&](const glm::vec3& p) {
        b.min = glm::min(b.min, p);
        b.max = glm::max(b.max, p);
    };
    for (u32 s = 0; s < m_cache.segments.size(); ++s) {
        const detail::Segment& seg = m_cache.segments[s];
        if (!m_cache.cubic) {
            constexpr u32 kSamples = 32;
            for (u32 k = 0; k <= kSamples; ++k) {
                glm::vec3 p;
                evalSegment(s, static_cast<f32>(k) / kSamples, &p, nullptr, nullptr);
                include(p);
            }
            continue;
        }
        const CubicBezier& c = seg.bezier;
        include(c[0]);
        include(c[3]);
        // Extrema where a component of the (quadratic) derivative vanishes.
        const glm::vec3 d0 = c[1] - c[0];
        const glm::vec3 d1 = c[2] - c[1];
        const glm::vec3 d2 = c[3] - c[2];
        const glm::vec3 qa = d0 - 2.0f * d1 + d2;
        const glm::vec3 qb = 2.0f * (d1 - d0);
        const glm::vec3 qc = d0;
        for (int axis = 0; axis < 3; ++axis) {
            const f32 a = qa[axis];
            const f32 bb = qb[axis];
            const f32 cc = qc[axis];
            f32 roots[2];
            int rootCount = 0;
            if (std::abs(a) < 1e-9f) {
                if (std::abs(bb) > 1e-9f) {
                    roots[rootCount++] = -cc / bb;
                }
            } else {
                const f32 disc = bb * bb - 4.0f * a * cc;
                if (disc >= 0.0f) {
                    const f32 sq = std::sqrt(disc);
                    roots[rootCount++] = (-bb + sq) / (2.0f * a);
                    roots[rootCount++] = (-bb - sq) / (2.0f * a);
                }
            }
            for (int r = 0; r < rootCount; ++r) {
                if (roots[r] > 0.0f && roots[r] < 1.0f) {
                    include(bezierPosition(c, roots[r]));
                }
            }
        }
    }
    return b;
}

} // namespace ox::spline
