#include <oxwald/animation/blend_space.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ox::anim {

void BlendSpace1D::addSample(f32 position, std::shared_ptr<const AnimationClip> clip) {
    m_samples.push_back({position, std::move(clip)});
    std::stable_sort(m_samples.begin(), m_samples.end(),
                     [](const Sample& a, const Sample& b) { return a.position < b.position; });
}

void BlendSpace1D::computeWeights(f32 x, std::vector<f32>& weights) const {
    weights.assign(m_samples.size(), 0.0f);
    if (m_samples.empty()) {
        return;
    }
    if (x <= m_samples.front().position) {
        weights.front() = 1.0f;
        return;
    }
    if (x >= m_samples.back().position) {
        weights.back() = 1.0f;
        return;
    }
    for (usize i = 0; i + 1 < m_samples.size(); ++i) {
        const f32 a = m_samples[i].position;
        const f32 b = m_samples[i + 1].position;
        if (x >= a && x <= b) {
            const f32 t = b > a ? (x - a) / (b - a) : 0.0f;
            weights[i] = 1.0f - t;
            weights[i + 1] = t;
            return;
        }
    }
}

// ---- Delaunay ----

std::vector<std::array<u32, 3>> delaunayTriangulate(const std::vector<glm::vec2>& points) {
    std::vector<std::array<u32, 3>> result;
    const usize n = points.size();
    if (n < 3) {
        return result;
    }
    glm::vec2 lo(std::numeric_limits<f32>::max()), hi(std::numeric_limits<f32>::lowest());
    for (const auto& p : points) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec2 c = (lo + hi) * 0.5f;
    const f32 d = std::max(hi.x - lo.x, hi.y - lo.y) * 20.0f + 1.0f;
    std::vector<glm::dvec2> pts(points.begin(), points.end());
    pts.emplace_back(c.x - d, c.y - d);
    pts.emplace_back(c.x + d, c.y - d);
    pts.emplace_back(c.x, c.y + d);

    struct Tri {
        u32 a, b, c;
        glm::dvec2 center;
        f64 r2;
    };
    auto makeTri = [&](u32 a, u32 b, u32 cc) {
        const glm::dvec2 A = pts[a], B = pts[b], C = pts[cc];
        const f64 dd = 2.0 * (A.x * (B.y - C.y) + B.x * (C.y - A.y) + C.x * (A.y - B.y));
        Tri t{a, b, cc, {0, 0}, std::numeric_limits<f64>::max()};
        if (std::abs(dd) > 1e-12) {
            const f64 a2 = glm::dot(A, A), b2 = glm::dot(B, B), c2 = glm::dot(C, C);
            t.center = {(a2 * (B.y - C.y) + b2 * (C.y - A.y) + c2 * (A.y - B.y)) / dd,
                        (a2 * (C.x - B.x) + b2 * (A.x - C.x) + c2 * (B.x - A.x)) / dd};
            const glm::dvec2 e = A - t.center;
            t.r2 = glm::dot(e, e);
        }
        return t;
    };

    std::vector<Tri> tris{makeTri(static_cast<u32>(n), static_cast<u32>(n + 1), static_cast<u32>(n + 2))};
    std::vector<std::pair<u32, u32>> edges;
    for (u32 i = 0; i < n; ++i) {
        const glm::dvec2 p = pts[i];
        edges.clear();
        for (usize t = 0; t < tris.size();) {
            const glm::dvec2 e = p - tris[t].center;
            if (glm::dot(e, e) <= tris[t].r2 * (1.0 + 1e-9)) {
                edges.emplace_back(tris[t].a, tris[t].b);
                edges.emplace_back(tris[t].b, tris[t].c);
                edges.emplace_back(tris[t].c, tris[t].a);
                tris[t] = tris.back();
                tris.pop_back();
            } else {
                ++t;
            }
        }
        // Boundary of the cavity = edges that appear exactly once.
        for (usize a = 0; a < edges.size(); ++a) {
            bool shared = false;
            for (usize b = 0; b < edges.size(); ++b) {
                if (a != b && ((edges[a].first == edges[b].second && edges[a].second == edges[b].first) ||
                               (edges[a] == edges[b]))) {
                    shared = true;
                    break;
                }
            }
            if (!shared) {
                tris.push_back(makeTri(edges[a].first, edges[a].second, i));
            }
        }
    }
    for (const auto& t : tris) {
        if (t.a < n && t.b < n && t.c < n) {
            const glm::dvec2 ab = pts[t.b] - pts[t.a], ac = pts[t.c] - pts[t.a];
            if (std::abs(ab.x * ac.y - ab.y * ac.x) > 1e-9) {
                result.push_back({t.a, t.b, t.c});
            }
        }
    }
    return result;
}

void BlendSpace2D::addSample(glm::vec2 position, std::shared_ptr<const AnimationClip> clip) {
    m_samples.push_back({position, std::move(clip)});
    m_dirty = true;
}

const std::vector<std::array<u32, 3>>& BlendSpace2D::triangles() const {
    if (m_dirty) {
        triangulate();
    }
    return m_triangles;
}

void BlendSpace2D::triangulate() const {
    std::vector<glm::vec2> pts;
    pts.reserve(m_samples.size());
    for (const auto& s : m_samples) pts.push_back(s.position);
    m_triangles = delaunayTriangulate(pts);
    m_dirty = false;
}

void BlendSpace2D::computeWeights(glm::vec2 p, std::vector<f32>& weights) const {
    weights.assign(m_samples.size(), 0.0f);
    if (m_samples.empty()) {
        return;
    }
    if (m_samples.size() == 1) {
        weights[0] = 1.0f;
        return;
    }
    switch (m_mode) {
    case Mode::Delaunay:
        delaunayWeights(p, weights);
        break;
    case Mode::FreeformDirectional:
        gradientBandWeights(p, true, weights);
        break;
    case Mode::FreeformCartesian:
        gradientBandWeights(p, false, weights);
        break;
    }
}

void BlendSpace2D::delaunayWeights(glm::vec2 p, std::vector<f32>& weights) const {
    const auto& tris = triangles();
    if (tris.empty()) {
        // Collinear samples: no triangles; gradient bands degrade gracefully to 1D.
        gradientBandWeights(p, false, weights);
        return;
    }
    for (const auto& t : tris) {
        const glm::vec2 a = m_samples[t[0]].position, b = m_samples[t[1]].position, c = m_samples[t[2]].position;
        const glm::vec2 v0 = b - a, v1 = c - a, v2 = p - a;
        const f32 den = v0.x * v1.y - v1.x * v0.y;
        const f32 v = (v2.x * v1.y - v1.x * v2.y) / den;
        const f32 w = (v0.x * v2.y - v2.x * v0.y) / den;
        const f32 u = 1.0f - v - w;
        constexpr f32 eps = -1e-5f;
        if (u >= eps && v >= eps && w >= eps) {
            const f32 s = std::max(u, 0.0f) + std::max(v, 0.0f) + std::max(w, 0.0f);
            weights[t[0]] = std::max(u, 0.0f) / s;
            weights[t[1]] = std::max(v, 0.0f) / s;
            weights[t[2]] = std::max(w, 0.0f) / s;
            return;
        }
    }
    // Outside the hull: interpolate along the closest triangle edge.
    f32 best = std::numeric_limits<f32>::max();
    u32 bi = 0, bj = 0;
    f32 bt = 0.0f;
    for (const auto& t : tris) {
        for (u32 e = 0; e < 3; ++e) {
            const u32 i = t[e], j = t[(e + 1) % 3];
            const glm::vec2 a = m_samples[i].position, b = m_samples[j].position;
            const glm::vec2 ab = b - a;
            const f32 len2 = glm::dot(ab, ab);
            const f32 s = len2 > 0.0f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            const glm::vec2 q = a + ab * s;
            const f32 d = glm::dot(p - q, p - q);
            if (d < best) {
                best = d;
                bi = i;
                bj = j;
                bt = s;
            }
        }
    }
    weights[bi] += 1.0f - bt;
    weights[bj] += bt;
}

namespace {
f32 signedAngle(glm::vec2 a, glm::vec2 b) {
    return std::atan2(a.x * b.y - a.y * b.x, glm::dot(a, b));
}
} // namespace

void BlendSpace2D::gradientBandWeights(glm::vec2 p, bool polar, std::vector<f32>& weights) const {
    constexpr f32 kAngleScale = 2.0f; // Johansen's directional weighting between radial and angular distance
    constexpr f32 kEps = 1e-5f;
    const usize n = m_samples.size();
    f32 total = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const glm::vec2 pi = m_samples[i].position;
        f32 h = 1.0f;
        for (usize j = 0; j < n && h > 0.0f; ++j) {
            if (i == j) continue;
            const glm::vec2 pj = m_samples[j].position;
            glm::vec2 vij, vip;
            if (polar) {
                const f32 li = glm::length(pi), lj = glm::length(pj), lp = glm::length(p);
                const f32 mean = (li + lj) * 0.5f;
                if (mean < kEps) continue;
                // Reference direction: p_i's, or p_j's when p_i sits at the origin.
                const glm::vec2 ref = li > kEps ? pi / li : (lj > kEps ? pj / lj : glm::vec2(0, 1));
                const f32 aij = lj > kEps ? signedAngle(ref, pj) : 0.0f;
                const f32 aip = lp > kEps ? signedAngle(ref, p) : 0.0f;
                vij = {(lj - li) / mean, aij * kAngleScale};
                vip = {(lp - li) / mean, aip * kAngleScale};
            } else {
                vij = pj - pi;
                vip = p - pi;
            }
            const f32 len2 = glm::dot(vij, vij);
            if (len2 < kEps * kEps) continue;
            h = std::min(h, std::clamp(1.0f - glm::dot(vip, vij) / len2, 0.0f, 1.0f));
        }
        weights[i] = h;
        total += h;
    }
    if (total > 0.0f) {
        for (auto& w : weights) w /= total;
    } else {
        // Numerically possible far outside the space: pick the nearest sample.
        usize best = 0;
        for (usize i = 1; i < n; ++i) {
            if (glm::length(m_samples[i].position - p) < glm::length(m_samples[best].position - p)) best = i;
        }
        weights[best] = 1.0f;
    }
}

} // namespace ox::anim
