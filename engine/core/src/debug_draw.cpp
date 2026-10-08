#include <oxwald/core/debug_draw.hpp>

#include <algorithm>
#include <cmath>

namespace ox {

namespace debug_color {

u32 pack(const glm::vec4& c) {
    const auto to8 = [](f32 v) { return static_cast<u8>(std::lround(saturate(v) * 255.0f)); };
    return rgba(to8(c.r), to8(c.g), to8(c.b), to8(c.a));
}

glm::vec4 unpack(u32 c) {
    return glm::vec4{static_cast<f32>(c & 0xffu), static_cast<f32>((c >> 8) & 0xffu), static_cast<f32>((c >> 16) & 0xffu),
                     static_cast<f32>((c >> 24) & 0xffu)} /
           255.0f;
}

} // namespace debug_color

namespace {

// Builds an orthonormal basis (u, v) perpendicular to n (n need not be normalised).
void orthonormalBasis(glm::vec3 n, glm::vec3& u, glm::vec3& v) {
    const f32 len = glm::length(n);
    n = len > kEpsilon ? n / len : glm::vec3{0.0f, 1.0f, 0.0f};
    const glm::vec3 helper = std::abs(n.y) < 0.99f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
    u = glm::normalize(glm::cross(helper, n));
    v = glm::cross(n, u);
}

void appendBoxEdges(std::vector<glm::vec3>& out, const std::array<glm::vec3, 8>& c) {
    // Corner index bits: x = 1, y = 2, z = 4 (matches OBB::corners and the AABB layout below).
    constexpr std::array<std::array<u32, 2>, 12> kEdges{{{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                                         {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
    for (const auto& e : kEdges) {
        out.push_back(c[e[0]]);
        out.push_back(c[e[1]]);
    }
}

} // namespace

void DebugDraw::submit(std::span<const glm::vec3> points, u32 color, f32 duration, bool depthTest) {
    if (!enabled() || points.size() < 2) {
        return;
    }
    duration = std::max(duration, 0.0f);
    std::lock_guard lock(m_mutex);
    m_pendingLines.reserve(m_pendingLines.size() + points.size() / 2);
    for (usize i = 0; i + 1 < points.size(); i += 2) {
        m_pendingLines.push_back(
            LineItem{DebugVertex{points[i], color}, DebugVertex{points[i + 1], color}, duration, depthTest});
    }
}

void DebugDraw::appendCircle(std::vector<glm::vec3>& out, glm::vec3 center, glm::vec3 axisU, glm::vec3 axisV, f32 radius,
                             u32 segments, f32 startAngle, f32 sweep) const {
    segments = std::max<u32>(segments, 3);
    glm::vec3 prev = center + radius * (std::cos(startAngle) * axisU + std::sin(startAngle) * axisV);
    for (u32 i = 1; i <= segments; ++i) {
        const f32 a = startAngle + sweep * static_cast<f32>(i) / static_cast<f32>(segments);
        const glm::vec3 p = center + radius * (std::cos(a) * axisU + std::sin(a) * axisV);
        out.push_back(prev);
        out.push_back(p);
        prev = p;
    }
}

void DebugDraw::line(glm::vec3 a, glm::vec3 b, DebugColor color, f32 duration, bool depthTest) {
    const std::array<glm::vec3, 2> pts{a, b};
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::ray(glm::vec3 origin, glm::vec3 direction, f32 length, DebugColor color, f32 duration, bool depthTest) {
    const f32 len = glm::length(direction);
    if (len < kEpsilon) {
        return;
    }
    line(origin, origin + direction / len * length, color, duration, depthTest);
}

void DebugDraw::aabb(const AABB& b, DebugColor color, f32 duration, bool depthTest) {
    if (!b.valid()) {
        return;
    }
    std::array<glm::vec3, 8> c{};
    for (u32 i = 0; i < 8; ++i) {
        c[i] = {(i & 1u) ? b.max.x : b.min.x, (i & 2u) ? b.max.y : b.min.y, (i & 4u) ? b.max.z : b.min.z};
    }
    std::vector<glm::vec3> pts;
    pts.reserve(24);
    appendBoxEdges(pts, c);
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::box(glm::vec3 center, glm::vec3 halfExtents, const glm::quat& rotation, DebugColor color, f32 duration,
                    bool depthTest) {
    obb(OBB{center, halfExtents, rotation}, color, duration, depthTest);
}

void DebugDraw::obb(const OBB& b, DebugColor color, f32 duration, bool depthTest) {
    std::vector<glm::vec3> pts;
    pts.reserve(24);
    appendBoxEdges(pts, b.corners());
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::sphere(glm::vec3 center, f32 radius, DebugColor color, f32 duration, bool depthTest, u32 segments) {
    std::vector<glm::vec3> pts;
    pts.reserve(static_cast<usize>(std::max<u32>(segments, 3)) * 6);
    const glm::vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    appendCircle(pts, center, x, y, radius, segments);
    appendCircle(pts, center, y, z, radius, segments);
    appendCircle(pts, center, z, x, radius, segments);
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::circle(glm::vec3 center, glm::vec3 normal, f32 radius, DebugColor color, f32 duration, bool depthTest,
                       u32 segments) {
    glm::vec3 u, v;
    orthonormalBasis(normal, u, v);
    std::vector<glm::vec3> pts;
    pts.reserve(static_cast<usize>(std::max<u32>(segments, 3)) * 2);
    appendCircle(pts, center, u, v, radius, segments);
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::capsule(glm::vec3 p0, glm::vec3 p1, f32 radius, DebugColor color, f32 duration, bool depthTest,
                        u32 segments) {
    const glm::vec3 axis = p1 - p0;
    const f32 len = glm::length(axis);
    if (len < kEpsilon) {
        sphere(p0, radius, color, duration, depthTest, segments);
        return;
    }
    const glm::vec3 w = axis / len;
    glm::vec3 u, v;
    orthonormalBasis(w, u, v);
    const u32 halfSegments = std::max<u32>(segments / 2, 2);
    std::vector<glm::vec3> pts;
    appendCircle(pts, p0, u, v, radius, segments);
    appendCircle(pts, p1, u, v, radius, segments);
    for (const glm::vec3& side : {u, -u, v, -v}) {
        pts.push_back(p0 + side * radius);
        pts.push_back(p1 + side * radius);
    }
    // Hemispherical caps: two half circles per end.
    appendCircle(pts, p1, u, w, radius, halfSegments, 0.0f, kPi);
    appendCircle(pts, p1, v, w, radius, halfSegments, 0.0f, kPi);
    appendCircle(pts, p0, u, -w, radius, halfSegments, 0.0f, kPi);
    appendCircle(pts, p0, v, -w, radius, halfSegments, 0.0f, kPi);
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::cone(glm::vec3 apex, glm::vec3 direction, f32 length, f32 halfAngle, DebugColor color, f32 duration,
                     bool depthTest, u32 segments) {
    const f32 dlen = glm::length(direction);
    if (dlen < kEpsilon) {
        return;
    }
    const glm::vec3 w = direction / dlen;
    glm::vec3 u, v;
    orthonormalBasis(w, u, v);
    const glm::vec3 baseCenter = apex + w * length;
    const f32 baseRadius = length * std::tan(std::clamp(halfAngle, 0.0f, kHalfPi - 1e-3f));
    std::vector<glm::vec3> pts;
    appendCircle(pts, baseCenter, u, v, baseRadius, segments);
    for (const glm::vec3& side : {u, -u, v, -v}) {
        pts.push_back(apex);
        pts.push_back(baseCenter + side * baseRadius);
    }
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::cylinder(glm::vec3 p0, glm::vec3 p1, f32 radius, DebugColor color, f32 duration, bool depthTest,
                         u32 segments) {
    glm::vec3 u, v;
    orthonormalBasis(p1 - p0, u, v);
    std::vector<glm::vec3> pts;
    appendCircle(pts, p0, u, v, radius, segments);
    appendCircle(pts, p1, u, v, radius, segments);
    for (const glm::vec3& side : {u, -u, v, -v}) {
        pts.push_back(p0 + side * radius);
        pts.push_back(p1 + side * radius);
    }
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::frustum(const glm::mat4& invViewProj, bool reversedZ, DebugColor color, f32 duration, bool depthTest) {
    const auto c = Frustum::corners(invViewProj, reversedZ);
    std::vector<glm::vec3> pts;
    pts.reserve(24);
    for (u32 i = 0; i < 4; ++i) {
        const u32 n = (i + 1) % 4;
        pts.insert(pts.end(), {c[i], c[n], c[4 + i], c[4 + n], c[i], c[4 + i]});
    }
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::arrow(glm::vec3 from, glm::vec3 to, f32 headSize, DebugColor color, f32 duration, bool depthTest) {
    const glm::vec3 d = to - from;
    const f32 len = glm::length(d);
    if (len < kEpsilon) {
        return;
    }
    const glm::vec3 w = d / len;
    glm::vec3 u, v;
    orthonormalBasis(w, u, v);
    const glm::vec3 headBase = to - w * headSize;
    const f32 headRadius = headSize * 0.5f;
    std::vector<glm::vec3> pts{from, to};
    for (const glm::vec3& side : {u, -u, v, -v}) {
        pts.push_back(to);
        pts.push_back(headBase + side * headRadius);
    }
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::axes(const Transform& t, f32 size, f32 duration, bool depthTest) {
    axes(t.toMatrix(), size, duration, depthTest);
}

void DebugDraw::axes(const glm::mat4& m, f32 size, f32 duration, bool depthTest) {
    const glm::vec3 o{m[3]};
    line(o, o + glm::vec3(m[0]) * size, debug_color::kRed, duration, depthTest);
    line(o, o + glm::vec3(m[1]) * size, debug_color::kGreen, duration, depthTest);
    line(o, o + glm::vec3(m[2]) * size, debug_color::kBlue, duration, depthTest);
}

void DebugDraw::grid(glm::vec3 center, f32 cellSize, u32 cells, DebugColor color, f32 duration, bool depthTest) {
    if (cells == 0) {
        return;
    }
    const f32 half = cellSize * static_cast<f32>(cells) * 0.5f;
    std::vector<glm::vec3> pts;
    pts.reserve((static_cast<usize>(cells) + 1) * 4);
    for (u32 i = 0; i <= cells; ++i) {
        const f32 o = -half + cellSize * static_cast<f32>(i);
        pts.push_back(center + glm::vec3{o, 0.0f, -half});
        pts.push_back(center + glm::vec3{o, 0.0f, half});
        pts.push_back(center + glm::vec3{-half, 0.0f, o});
        pts.push_back(center + glm::vec3{half, 0.0f, o});
    }
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::point(glm::vec3 p, f32 size, DebugColor color, f32 duration, bool depthTest) {
    const f32 h = size * 0.5f;
    const std::array<glm::vec3, 6> pts{p - glm::vec3{h, 0, 0}, p + glm::vec3{h, 0, 0}, p - glm::vec3{0, h, 0},
                                       p + glm::vec3{0, h, 0}, p - glm::vec3{0, 0, h}, p + glm::vec3{0, 0, h}};
    submit(pts, color.packed, duration, depthTest);
}

void DebugDraw::text3D(glm::vec3 position, std::string text, DebugColor color, f32 duration, bool depthTest) {
    if (!enabled()) {
        return;
    }
    std::lock_guard lock(m_mutex);
    m_pendingTexts.push_back(
        TextItem{DebugText{position, std::move(text), color.packed, depthTest}, std::max(duration, 0.0f)});
}

void DebugDraw::flush(f32 dt) {
    dt = std::max(dt, 0.0f);
    // Age the items shown by previous flushes and drop expired ones; items submitted since then are
    // merged afterwards so every item is output at least once.
    std::erase_if(m_lines, [dt](LineItem& l) {
        l.remaining -= dt;
        return l.remaining <= 0.0f;
    });
    std::erase_if(m_texts, [dt](TextItem& t) {
        t.remaining -= dt;
        return t.remaining <= 0.0f;
    });
    {
        std::lock_guard lock(m_mutex);
        m_lines.insert(m_lines.end(), m_pendingLines.begin(), m_pendingLines.end());
        m_pendingLines.clear();
        for (TextItem& t : m_pendingTexts) {
            m_texts.push_back(std::move(t));
        }
        m_pendingTexts.clear();
    }

    m_outDepth.clear();
    m_outOverlay.clear();
    m_outTexts.clear();
    for (const LineItem& l : m_lines) {
        auto& out = l.depthTest ? m_outDepth : m_outOverlay;
        out.push_back(l.a);
        out.push_back(l.b);
    }
    for (const TextItem& t : m_texts) {
        m_outTexts.push_back(t.text);
    }
}

void DebugDraw::clear() {
    {
        std::lock_guard lock(m_mutex);
        m_pendingLines.clear();
        m_pendingTexts.clear();
    }
    m_lines.clear();
    m_texts.clear();
    m_outDepth.clear();
    m_outOverlay.clear();
    m_outTexts.clear();
}

} // namespace ox
