// Voronoi fracture (see fracture.hpp). Everything here is deterministic: a private splitmix64 generator for the
// seeds, canonical edge/plane intersections (the same cut point bit-for-bit from both triangles of an edge) and
// containers that are only used for lookups, never iterated to produce output order.
#include <oxwald/core/hash.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/physics/fracture.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <tuple>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace ox::physics {

namespace {

constexpr u32 kOuterPlane = ~0u;

// ---- deterministic random numbers ------------------------------------------------------------------------------

struct SplitMix64 {
    u64 state;
    u64 next() {
        u64 z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    f32 unit() { return f32(next() >> 40) * (1.f / 16777216.f); } // [0, 1)
    glm::vec3 unit3() {
        const f32 x = unit();
        const f32 y = unit();
        const f32 z = unit();
        return {x, y, z};
    }
};

// ---- exact position keys -------------------------------------------------------------------------------------

struct PosKey {
    u32 x, y, z;
    bool operator==(const PosKey&) const = default;
    bool operator<(const PosKey& o) const { return std::tie(x, y, z) < std::tie(o.x, o.y, o.z); }
};

u32 floatBits(f32 f) {
    if (f == 0.f) f = 0.f; // -0 == +0
    return std::bit_cast<u32>(f);
}
PosKey keyOf(const glm::vec3& p) { return {floatBits(p.x), floatBits(p.y), floatBits(p.z)}; }

struct PosKeyHash {
    usize operator()(const PosKey& k) const {
        u64 h = hashCombine(k.x, k.y);
        return usize(hashCombine(h, k.z));
    }
};

struct EdgeKey {
    PosKey a, b;
    bool operator==(const EdgeKey&) const = default;
};
struct EdgeKeyHash {
    usize operator()(const EdgeKey& e) const {
        PosKeyHash h;
        return usize(hashCombine(h(e.a), h(e.b)));
    }
};

bool lexLess(const glm::vec3& a, const glm::vec3& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}

// ---- triangle soup -------------------------------------------------------------------------------------------

struct Corner {
    glm::vec3 p{0.f};
    glm::vec3 n{0.f};
    glm::vec2 uv{0.f};
};

struct Tri {
    std::array<Corner, 3> v;
    u32 plane = kOuterPlane; // kOuterPlane: source surface; else index of the neighbouring seed (cut face)
};

using Soup = std::vector<Tri>;

glm::vec3 triNormal(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 n = glm::cross(b - a, c - a);
    const f32 l = glm::length(n);
    return l > 0.f ? n / l : glm::vec3(0.f, 1.f, 0.f);
}

glm::vec2 boxProjectUv(const glm::vec3& p, const glm::vec3& n, f32 scale) {
    const glm::vec3 a = glm::abs(n);
    if (a.x >= a.y && a.x >= a.z) return glm::vec2(p.z, p.y) * scale;
    if (a.y >= a.z) return glm::vec2(p.x, p.z) * scale;
    return glm::vec2(p.x, p.y) * scale;
}

// Orthonormal basis (u, v) with cross(u, v) == n.
void basisOf(const glm::vec3& n, glm::vec3& u, glm::vec3& v) {
    const glm::vec3 helper = std::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    u = glm::normalize(glm::cross(helper, n));
    v = glm::cross(n, u);
}

struct Plane {
    glm::vec3 n{0.f, 1.f, 0.f};
    f32 d = 0.f; // keep dot(n, p) - d <= 0
    u32 id = kOuterPlane;
};

class Clipper {
public:
    Clipper(f32 eps, f32 uvScale) : m_eps(eps), m_uvScale(uvScale) {}

    // Keeps the part of the closed soup on the negative side of the plane and caps the cut.
    Soup clip(const Soup& in, const Plane& plane) const {
        Soup out;
        out.reserve(in.size() + 16);
        bool cut = false;
        for (const Tri& t : in) {
            std::array<f32, 3> d{};
            u32 pos = 0, neg = 0;
            for (u32 i = 0; i < 3; ++i) {
                d[i] = dist(plane, t.v[i].p);
                if (d[i] > 0.f) ++pos;
                else if (d[i] < 0.f) ++neg;
            }
            if (pos == 0) {
                out.push_back(t);
                if (neg < 3) cut = true; // touches the plane
                continue;
            }
            cut = true;
            if (neg == 0) continue; // entirely outside (or on the plane, facing out)
            // Sutherland–Hodgman against one plane: 3 or 4 vertices.
            std::array<Corner, 4> poly{};
            u32 count = 0;
            for (u32 i = 0; i < 3; ++i) {
                const u32 j = (i + 1) % 3;
                if (d[i] <= 0.f) poly[count++] = t.v[i];
                if ((d[i] < 0.f && d[j] > 0.f) || (d[i] > 0.f && d[j] < 0.f)) {
                    poly[count++] = intersect(t.v[i], d[i], t.v[j], d[j], m_eps);
                }
            }
            for (u32 k = 1; k + 1 < count; ++k) emit(out, Tri{{poly[0], poly[k], poly[k + 1]}, t.plane});
        }
        if (cut) cap(out, plane);
        return out;
    }

    [[nodiscard]] f32 dist(const Plane& pl, const glm::vec3& p) const {
        const f32 s = glm::dot(pl.n, p) - pl.d;
        return std::abs(s) <= m_eps ? 0.f : s;
    }

private:
    static void emit(Soup& out, const Tri& t) {
        // Triangles with two identical corners have no area and their remaining edges cancel out.
        if (t.v[0].p == t.v[1].p || t.v[1].p == t.v[2].p || t.v[2].p == t.v[0].p) return;
        out.push_back(t);
    }

    // Canonical order (lexicographic by position) so both triangles of an edge produce the same point.
    // Cut points closer than `eps` to an end point snap to it, so no sliver vertices appear that would collapse
    // when the chunk is later made relative to its centroid.
    static Corner intersect(Corner a, f32 da, Corner b, f32 db, f32 eps) {
        if (lexLess(b.p, a.p)) {
            std::swap(a, b);
            std::swap(da, db);
        }
        const f32 t = da / (da - db);
        const f32 len = glm::length(b.p - a.p);
        if (t * len <= eps) return a;
        if ((1.f - t) * len <= eps) return b;
        Corner c;
        c.p = a.p + (b.p - a.p) * t;
        c.n = a.n + (b.n - a.n) * t;
        c.uv = a.uv + (b.uv - a.uv) * t;
        return c;
    }

    void cap(Soup& out, const Plane& plane) const {
        // Boundary edges of the kept surface: directed edges without their reverse.
        std::unordered_map<EdgeKey, i32, EdgeKeyHash> count;
        std::unordered_map<PosKey, glm::vec3, PosKeyHash> positions;
        std::vector<EdgeKey> order; // first-seen order keeps the output deterministic
        for (const Tri& t : out) {
            for (u32 i = 0; i < 3; ++i) {
                const glm::vec3& a = t.v[i].p;
                const glm::vec3& b = t.v[(i + 1) % 3].p;
                const EdgeKey e{keyOf(a), keyOf(b)};
                auto [it, inserted] = count.try_emplace(e, 0);
                if (inserted) order.push_back(e);
                ++it->second;
                positions.try_emplace(e.a, a);
                positions.try_emplace(e.b, b);
            }
        }
        std::vector<EdgeKey> boundary;
        for (const EdgeKey& e : order) {
            const i32 forward = count[e];
            auto rev = count.find(EdgeKey{e.b, e.a});
            const i32 backward = rev != count.end() ? rev->second : 0;
            if (forward <= backward) continue;
            const glm::vec3& a = positions[e.a];
            const glm::vec3& b = positions[e.b];
            // Only edges lying in the cut plane form the cap (a closed input has no other open edges).
            if (std::abs(glm::dot(plane.n, a) - plane.d) > m_eps * 8.f || std::abs(glm::dot(plane.n, b) - plane.d) > m_eps * 8.f) continue;
            for (i32 k = 0; k < forward - backward; ++k) boundary.push_back(e);
        }
        if (boundary.size() < 3) return;

        // Chain into loops.
        std::unordered_map<PosKey, std::vector<u32>, PosKeyHash> outgoing;
        for (u32 i = 0; i < boundary.size(); ++i) outgoing[boundary[i].a].push_back(i);
        std::vector<bool> used(boundary.size(), false);
        glm::vec3 u, v;
        basisOf(plane.n, u, v);
        for (u32 start = 0; start < boundary.size(); ++start) {
            if (used[start]) continue;
            std::vector<PosKey> loop;
            u32 cur = start;
            bool closed = false;
            for (usize guard = 0; guard <= boundary.size(); ++guard) {
                used[cur] = true;
                loop.push_back(boundary[cur].a);
                const PosKey end = boundary[cur].b;
                if (end == boundary[start].a) {
                    closed = true;
                    break;
                }
                u32 next = ~0u;
                for (u32 cand : outgoing[end]) {
                    if (!used[cand]) {
                        next = cand;
                        break;
                    }
                }
                if (next == ~0u) break;
                cur = next;
            }
            if (!closed || loop.size() < 3) continue;
            // The cap contains the reversed boundary edges.
            std::reverse(loop.begin(), loop.end());
            std::vector<glm::vec3> pts;
            pts.reserve(loop.size());
            for (const PosKey& k : loop) pts.push_back(positions[k]);
            triangulateCap(out, pts, plane, u, v);
        }
    }

    void triangulateCap(Soup& out, const std::vector<glm::vec3>& pts, const Plane& plane, const glm::vec3& u,
                        const glm::vec3& v) const {
        const usize n = pts.size();
        std::vector<glm::vec2> p2(n);
        for (usize i = 0; i < n; ++i) p2[i] = {glm::dot(pts[i], u), glm::dot(pts[i], v)};
        f32 area = 0.f;
        for (usize i = 0; i < n; ++i) {
            const glm::vec2& a = p2[i];
            const glm::vec2& b = p2[(i + 1) % n];
            area += a.x * b.y - b.x * a.y;
        }
        // Negative: a hole loop (the source has a tunnel through this plane) — not supported, see fracture.hpp.
        if (area <= 0.f) return;

        auto corner = [&](usize i) {
            Corner c;
            c.p = pts[i];
            c.n = plane.n;
            c.uv = p2[i] * m_uvScale;
            return c;
        };
        auto cross2 = [&](usize a, usize b, usize c) {
            const glm::vec2 ab = p2[b] - p2[a], ac = p2[c] - p2[a];
            return ab.x * ac.y - ab.y * ac.x;
        };
        auto inside = [&](usize a, usize b, usize c, usize q) {
            if (p2[q] == p2[a] || p2[q] == p2[b] || p2[q] == p2[c]) return false;
            const f32 tol = -1e-7f * std::abs(area);
            return cross2(a, b, q) >= tol && cross2(b, c, q) >= tol && cross2(c, a, q) >= tol;
        };

        std::vector<usize> idx(n);
        for (usize i = 0; i < n; ++i) idx[i] = i;
        const f32 areaEps = 1e-9f * std::abs(area);
        while (idx.size() > 3) {
            const usize m = idx.size();
            usize best = m;
            f32 bestCross = -std::numeric_limits<f32>::infinity();
            for (usize i = 0; i < m; ++i) {
                const usize a = idx[(i + m - 1) % m], b = idx[i], c = idx[(i + 1) % m];
                const f32 cr = cross2(a, b, c);
                if (cr > bestCross) bestCross = cr, best = i;
                if (cr <= areaEps) continue;
                bool ear = true;
                for (usize j = 0; j < m && ear; ++j) {
                    const usize q = idx[j];
                    if (q == a || q == b || q == c) continue;
                    if (inside(a, b, c, q)) ear = false;
                }
                if (!ear) continue;
                best = i;
                break;
            }
            // No proper ear (collinear remainder / round-off): clip the most convex vertex anyway. Degenerate
            // triangles keep the edge pairing intact (the mesh stays closed) and add no area.
            const usize a = idx[(best + m - 1) % m], b = idx[best], c = idx[(best + 1) % m];
            emit(out, Tri{{corner(a), corner(b), corner(c)}, plane.id});
            idx.erase(idx.begin() + std::ptrdiff_t(best));
        }
        emit(out, Tri{{corner(idx[0]), corner(idx[1]), corner(idx[2])}, plane.id});
    }

    f32 m_eps;
    f32 m_uvScale;
};

f32 soupVolume(const Soup& s, glm::vec3* centroid) {
    f64 vol = 0.0;
    glm::dvec3 c{0.0};
    for (const Tri& t : s) {
        const glm::dvec3 a(t.v[0].p), b(t.v[1].p), d(t.v[2].p);
        const f64 v6 = glm::dot(a, glm::cross(b, d));
        vol += v6;
        c += (a + b + d) * v6;
    }
    if (centroid) *centroid = std::abs(vol) > 1e-30 ? glm::vec3(c / (4.0 * vol)) : glm::vec3(0.f);
    return f32(vol / 6.0);
}

Soup toSoup(const FractureMesh& m) {
    Soup s;
    s.reserve(m.indices.size() / 3);
    const bool hasN = m.normals.size() == m.positions.size();
    const bool hasUv = m.uvs.size() == m.positions.size();
    for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
        Tri t;
        const glm::vec3 fn = triNormal(m.positions[m.indices[i]], m.positions[m.indices[i + 1]], m.positions[m.indices[i + 2]]);
        for (u32 k = 0; k < 3; ++k) {
            const u32 vi = m.indices[i + k];
            t.v[k].p = m.positions[vi];
            t.v[k].n = hasN ? m.normals[vi] : fn;
            t.v[k].uv = hasUv ? m.uvs[vi] : boxProjectUv(m.positions[vi], fn, 1.f);
        }
        if (t.v[0].p == t.v[1].p || t.v[1].p == t.v[2].p || t.v[2].p == t.v[0].p) continue;
        s.push_back(t);
    }
    return s;
}

f64 solidAngle(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c) {
    const f64 la = glm::length(a), lb = glm::length(b), lc = glm::length(c);
    const f64 det = glm::dot(a, glm::cross(b, c));
    const f64 div = la * lb * lc + glm::dot(a, b) * lc + glm::dot(a, c) * lb + glm::dot(b, c) * la;
    return 2.0 * std::atan2(det, div);
}

struct VertexKey {
    std::array<u32, 9> bits;
    bool operator==(const VertexKey&) const = default;
};
struct VertexKeyHash {
    usize operator()(const VertexKey& k) const {
        u64 h = kFnv64Offset;
        for (u32 b : k.bits) h = hashCombine(h, b);
        return usize(h);
    }
};

FractureChunk buildChunk(const Soup& soup, const glm::vec3& centroid, f32 volume) {
    FractureChunk c;
    c.centroid = centroid;
    c.volume = volume;
    c.boundsMin = glm::vec3(std::numeric_limits<f32>::max());
    c.boundsMax = glm::vec3(std::numeric_limits<f32>::lowest());
    std::unordered_map<VertexKey, u32, VertexKeyHash> vertices;
    std::unordered_set<PosKey, PosKeyHash> hull;
    auto addCorner = [&](const Corner& k, bool interior) {
        const glm::vec3 local = k.p - centroid;
        const glm::vec3 n = glm::length(k.n) > 0.f ? glm::normalize(k.n) : glm::vec3(0, 1, 0);
        VertexKey key{{floatBits(local.x), floatBits(local.y), floatBits(local.z), floatBits(n.x), floatBits(n.y),
                       floatBits(n.z), floatBits(k.uv.x), floatBits(k.uv.y), interior ? 1u : 0u}};
        auto [it, inserted] = vertices.try_emplace(key, u32(c.positions.size()));
        if (inserted) {
            c.positions.push_back(local);
            c.normals.push_back(n);
            c.uvs.push_back(k.uv);
            if (hull.insert(keyOf(local)).second) c.hullPoints.push_back(local);
            c.boundsMin = glm::min(c.boundsMin, k.p);
            c.boundsMax = glm::max(c.boundsMax, k.p);
        }
        c.indices.push_back(it->second);
    };
    for (int pass = 0; pass < 2; ++pass) {
        const bool interior = pass == 1;
        if (interior) c.interiorIndexStart = u32(c.indices.size());
        for (const Tri& t : soup) {
            if ((t.plane != kOuterPlane) != interior) continue;
            // Corners that become identical once relative to the centroid: the triangle has no area and its two
            // remaining edges cancel out, so dropping it keeps the mesh closed.
            const glm::vec3 l0 = t.v[0].p - centroid, l1 = t.v[1].p - centroid, l2 = t.v[2].p - centroid;
            if (l0 == l1 || l1 == l2 || l2 == l0) continue;
            for (const Corner& k : t.v) addCorner(k, interior);
        }
    }
    return c;
}

void boundsOf(std::span<const glm::vec3> pts, glm::vec3& mn, glm::vec3& mx) {
    mn = glm::vec3(std::numeric_limits<f32>::max());
    mx = glm::vec3(std::numeric_limits<f32>::lowest());
    for (const auto& p : pts) {
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
}

} // namespace

// ---- public helpers --------------------------------------------------------------------------------------------

std::vector<std::vector<u32>> FractureResult::adjacency() const {
    std::vector<std::vector<u32>> adj(chunks.size());
    for (const FractureLink& l : links) {
        if (l.a < adj.size() && l.b < adj.size()) {
            adj[l.a].push_back(l.b);
            adj[l.b].push_back(l.a);
        }
    }
    return adj;
}

f32 meshVolume(std::span<const glm::vec3> positions, std::span<const u32> indices) {
    f64 vol = 0.0;
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        const glm::dvec3 a(positions[indices[i]]), b(positions[indices[i + 1]]), c(positions[indices[i + 2]]);
        vol += glm::dot(a, glm::cross(b, c));
    }
    return f32(vol / 6.0);
}

glm::vec3 meshCentroid(std::span<const glm::vec3> positions, std::span<const u32> indices) {
    f64 vol = 0.0;
    glm::dvec3 c{0.0};
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        const glm::dvec3 a(positions[indices[i]]), b(positions[indices[i + 1]]), d(positions[indices[i + 2]]);
        const f64 v6 = glm::dot(a, glm::cross(b, d));
        vol += v6;
        c += (a + b + d) * v6;
    }
    return std::abs(vol) > 1e-30 ? glm::vec3(c / (4.0 * vol)) : glm::vec3(0.f);
}

bool isClosedMesh(std::span<const glm::vec3> positions, std::span<const u32> indices) {
    if (indices.size() < 12 || indices.size() % 3 != 0) return false;
    std::unordered_map<EdgeKey, i32, EdgeKeyHash> count;
    for (usize i = 0; i < indices.size(); i += 3) {
        for (u32 k = 0; k < 3; ++k) {
            const PosKey a = keyOf(positions[indices[i + k]]);
            const PosKey b = keyOf(positions[indices[i + (k + 1) % 3]]);
            if (a == b) return false;
            ++count[EdgeKey{a, b}];
        }
    }
    for (const auto& [e, n] : count) {
        auto it = count.find(EdgeKey{e.b, e.a});
        if (it == count.end() || it->second != n) return false;
    }
    return true;
}

bool isConvexMesh(std::span<const glm::vec3> positions, std::span<const u32> indices, f32 tolerance) {
    glm::vec3 mn, mx;
    boundsOf(positions, mn, mx);
    const f32 tol = tolerance * std::max(glm::length(mx - mn), 1e-6f);
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        const glm::vec3 a = positions[indices[i]], b = positions[indices[i + 1]], c = positions[indices[i + 2]];
        const glm::vec3 cr = glm::cross(b - a, c - a);
        const f32 len = glm::length(cr);
        // Slivers have unreliable normals; the planes of the real faces decide.
        if (len < 1e-5f * glm::dot(mx - mn, mx - mn)) continue;
        const glm::vec3 n = cr / len;
        for (const glm::vec3& p : positions) {
            if (glm::dot(n, p - a) > tol) return false;
        }
    }
    return true;
}

bool pointInsideMesh(const glm::vec3& p, std::span<const glm::vec3> positions, std::span<const u32> indices) {
    f64 w = 0.0;
    const glm::dvec3 q(p);
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        w += solidAngle(glm::dvec3(positions[indices[i]]) - q, glm::dvec3(positions[indices[i + 1]]) - q,
                        glm::dvec3(positions[indices[i + 2]]) - q);
    }
    return w / (4.0 * glm::pi<f64>()) > 0.5;
}

FractureMesh makeBoxFractureMesh(const glm::vec3& h) {
    FractureMesh m;
    struct Face {
        glm::vec3 n, u, v;
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (const Face& f : faces) {
        const u32 base = u32(m.positions.size());
        const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const glm::vec2& c : corners) {
            m.positions.push_back((f.n + f.u * c.x + f.v * c.y) * h);
            m.normals.push_back(f.n);
            m.uvs.push_back({c.x * 0.5f + 0.5f, 0.5f - c.y * 0.5f});
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return m;
}

FractureMesh makeConvexHullFractureMesh(std::span<const glm::vec3> input) {
    FractureMesh out;
    std::vector<glm::vec3> pts;
    {
        std::unordered_set<PosKey, PosKeyHash> seen;
        for (const glm::vec3& p : input) {
            if (seen.insert(keyOf(p)).second) pts.push_back(p);
        }
    }
    if (pts.size() < 4) return out;
    glm::vec3 mn, mx;
    boundsOf(pts, mn, mx);
    const f32 eps = 1e-6f * std::max(glm::length(mx - mn), 1e-6f);

    // Initial tetrahedron from extreme points.
    usize i0 = 0;
    for (usize i = 1; i < pts.size(); ++i) {
        if (lexLess(pts[i], pts[i0])) i0 = i;
    }
    usize i1 = i0;
    f32 best = 0.f;
    for (usize i = 0; i < pts.size(); ++i) {
        const f32 d = glm::length(pts[i] - pts[i0]);
        if (d > best) best = d, i1 = i;
    }
    usize i2 = i0;
    best = 0.f;
    for (usize i = 0; i < pts.size(); ++i) {
        const f32 d = glm::length(glm::cross(pts[i1] - pts[i0], pts[i] - pts[i0]));
        if (d > best) best = d, i2 = i;
    }
    const glm::vec3 n0 = triNormal(pts[i0], pts[i1], pts[i2]);
    usize i3 = i0;
    best = 0.f;
    for (usize i = 0; i < pts.size(); ++i) {
        const f32 d = std::abs(glm::dot(n0, pts[i] - pts[i0]));
        if (d > best) best = d, i3 = i;
    }
    if (i1 == i0 || i2 == i0 || i2 == i1 || best <= eps) return out;

    struct Face {
        std::array<u32, 3> v;
        glm::vec3 n;
        f32 d;
        bool alive = true;
    };
    std::vector<Face> faces;
    auto makeFace = [&](u32 a, u32 b, u32 c) {
        Face f;
        f.v = {a, b, c};
        f.n = triNormal(pts[a], pts[b], pts[c]);
        f.d = glm::dot(f.n, pts[a]);
        return f;
    };
    const glm::vec3 inner = (pts[i0] + pts[i1] + pts[i2] + pts[i3]) * 0.25f;
    const std::array<std::array<u32, 3>, 4> tet = {{{u32(i0), u32(i1), u32(i2)}, {u32(i0), u32(i3), u32(i1)},
                                                    {u32(i1), u32(i3), u32(i2)}, {u32(i2), u32(i3), u32(i0)}}};
    for (const auto& t : tet) {
        Face f = makeFace(t[0], t[1], t[2]);
        if (glm::dot(f.n, inner) - f.d > 0.f) f = makeFace(t[0], t[2], t[1]);
        faces.push_back(f);
    }
    auto edgeKey = [](u32 a, u32 b) { return (u64(a) << 32) | b; };
    for (usize pi = 0; pi < pts.size(); ++pi) {
        if (pi == i0 || pi == i1 || pi == i2 || pi == i3) continue;
        const glm::vec3& p = pts[pi];
        std::vector<u32> visible;
        for (u32 fi = 0; fi < faces.size(); ++fi) {
            if (faces[fi].alive && glm::dot(faces[fi].n, p) - faces[fi].d > eps) visible.push_back(fi);
        }
        if (visible.empty()) continue;
        std::unordered_set<u64> visibleEdges;
        for (u32 fi : visible) {
            const auto& v = faces[fi].v;
            for (u32 k = 0; k < 3; ++k) visibleEdges.insert(edgeKey(v[k], v[(k + 1) % 3]));
        }
        std::vector<std::pair<u32, u32>> horizon;
        for (u32 fi : visible) {
            const auto& v = faces[fi].v;
            for (u32 k = 0; k < 3; ++k) {
                const u32 a = v[k], b = v[(k + 1) % 3];
                if (!visibleEdges.contains(edgeKey(b, a))) horizon.emplace_back(a, b);
            }
        }
        for (u32 fi : visible) faces[fi].alive = false;
        for (const auto& [a, b] : horizon) faces.push_back(makeFace(a, b, u32(pi)));
    }

    for (const Face& f : faces) {
        if (!f.alive) continue;
        const u32 base = u32(out.positions.size());
        for (u32 k = 0; k < 3; ++k) {
            const glm::vec3& p = pts[f.v[k]];
            out.positions.push_back(p);
            out.normals.push_back(f.n);
            out.uvs.push_back(boxProjectUv(p, f.n, 1.f));
        }
        out.indices.insert(out.indices.end(), {base, base + 1, base + 2});
    }
    return out;
}

u64 fractureKey(const FractureMesh& mesh, const FractureSettings& s) {
    auto bytes = [](const auto& v) {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.data()), v.size() * sizeof(v[0]));
    };
    u64 h = fnv1a64(bytes(mesh.positions));
    h = fnv1a64(bytes(mesh.normals), h);
    h = fnv1a64(bytes(mesh.uvs), h);
    h = fnv1a64(bytes(mesh.indices), h);
    h = hashCombine(h, s.chunkCount);
    h = hashCombine(h, s.seed);
    h = hashCombine(h, floatBits(s.interiorUvScale));
    h = hashCombine(h, floatBits(s.minSeedSpacing));
    h = hashCombine(h, floatBits(s.planarRatio));
    if (s.focus) {
        h = hashCombine(h, floatBits(s.focus->x));
        h = hashCombine(h, floatBits(s.focus->y));
        h = hashCombine(h, floatBits(s.focus->z));
        h = hashCombine(h, floatBits(s.focusRadius));
        h = hashCombine(h, floatBits(s.focusFraction));
    }
    return h;
}

// ---- fracture --------------------------------------------------------------------------------------------------

FractureResult voronoiFracture(const FractureMesh& mesh, const FractureSettings& settings) {
    FractureResult result;
    result.seed = settings.seed;
    if (mesh.positions.empty() || mesh.indices.size() < 12) return result;
    boundsOf(mesh.positions, result.boundsMin, result.boundsMax);
    result.sourceVolume = meshVolume(mesh.positions, mesh.indices);
    result.sourceCentroid = meshCentroid(mesh.positions, mesh.indices);
    if (result.sourceVolume <= 0.f) {
        OX_LOG_WARN("physics", "fracture: source mesh has no positive volume (open or inverted)");
        return result;
    }
    const glm::vec3 extent = result.boundsMax - result.boundsMin;
    const f32 diag = glm::length(extent);
    const f32 eps = std::max(diag * 2e-6f, 1e-7f);

    // Seeds inside the mesh.
    const u32 wanted = std::max(settings.chunkCount, 1u);
    std::vector<glm::vec3> seeds;
    SplitMix64 rng{settings.seed ^ 0x6F78574C44465243ull};
    const f32 spacing = settings.minSeedSpacing * diag;
    const u32 focusCount = settings.focus ? u32(std::round(f32(wanted) * std::clamp(settings.focusFraction, 0.f, 1.f))) : 0u;
    // Planar seeding of thin sources (see FractureSettings::planarRatio).
    i32 flatAxis = -1;
    {
        i32 order[3] = {0, 1, 2};
        std::sort(order, order + 3, [&](i32 a, i32 b) { return extent[a] < extent[b]; });
        if (extent[order[0]] < settings.planarRatio * extent[order[1]]) flatAxis = order[0];
    }
    const glm::vec3 boundsCenter = (result.boundsMin + result.boundsMax) * 0.5f;
    for (u32 attempt = 0; attempt < wanted * 200u && seeds.size() < wanted; ++attempt) {
        glm::vec3 p;
        if (seeds.size() < focusCount) {
            glm::vec3 r;
            do {
                r = rng.unit3() * 2.f - 1.f;
            } while (glm::dot(r, r) > 1.f);
            p = *settings.focus + r * settings.focusRadius;
        } else {
            p = result.boundsMin + rng.unit3() * extent;
        }
        if (flatAxis >= 0) p[flatAxis] = boundsCenter[flatAxis];
        bool tooClose = false;
        for (const glm::vec3& s : seeds) {
            if (glm::length(s - p) < spacing) {
                tooClose = true;
                break;
            }
        }
        if (tooClose || !pointInsideMesh(p, mesh.positions, mesh.indices)) continue;
        seeds.push_back(p);
    }
    if (seeds.empty()) seeds.push_back(result.sourceCentroid);

    const Soup source = toSoup(mesh);
    const Clipper clipper(eps, settings.interiorUvScale);
    std::vector<i32> chunkOfSeed(seeds.size(), -1);
    std::vector<std::map<u32, f64>> cutArea(seeds.size()); // per seed: neighbour seed -> cut face area

    for (u32 i = 0; i < seeds.size(); ++i) {
        // Nearest neighbours first: the cell shrinks quickly and far planes are skipped.
        std::vector<u32> others;
        for (u32 j = 0; j < seeds.size(); ++j) {
            if (j != i) others.push_back(j);
        }
        std::sort(others.begin(), others.end(), [&](u32 a, u32 b) {
            const f32 da = glm::length(seeds[a] - seeds[i]), db = glm::length(seeds[b] - seeds[i]);
            return da != db ? da < db : a < b;
        });
        Soup cell = source;
        for (u32 j : others) {
            if (cell.empty()) break;
            const glm::vec3 delta = seeds[j] - seeds[i];
            const f32 len = glm::length(delta);
            if (len <= 0.f) continue;
            f32 reach = 0.f;
            for (const Tri& t : cell) {
                for (const Corner& c : t.v) reach = std::max(reach, glm::length(c.p - seeds[i]));
            }
            if (len * 0.5f > reach + eps) break; // sorted: no farther plane can cut the cell either
            Plane pl;
            pl.n = delta / len;
            pl.d = glm::dot(pl.n, (seeds[i] + seeds[j]) * 0.5f);
            pl.id = j;
            cell = clipper.clip(cell, pl);
        }
        glm::vec3 centroid;
        const f32 volume = soupVolume(cell, &centroid);
        if (cell.empty() || volume <= result.sourceVolume * 1e-7f) continue;
        for (const Tri& t : cell) {
            if (t.plane == kOuterPlane) continue;
            const f64 area = 0.5 * glm::length(glm::cross(glm::dvec3(t.v[1].p - t.v[0].p), glm::dvec3(t.v[2].p - t.v[0].p)));
            cutArea[i][t.plane] += area;
        }
        FractureChunk chunk = buildChunk(cell, centroid, volume);
        chunk.seed = seeds[i];
        chunkOfSeed[i] = i32(result.chunks.size());
        result.chunks.push_back(std::move(chunk));
    }

    // Links between chunks that share a cut face of noticeable area.
    const f64 minArea = f64(diag) * f64(diag) * 1e-6;
    std::map<std::pair<u32, u32>, f32> links;
    for (u32 i = 0; i < seeds.size(); ++i) {
        if (chunkOfSeed[i] < 0) continue;
        for (const auto& [j, area] : cutArea[i]) {
            if (j >= seeds.size() || chunkOfSeed[j] < 0 || area < minArea) continue;
            u32 a = u32(chunkOfSeed[i]), b = u32(chunkOfSeed[j]);
            if (a > b) std::swap(a, b);
            f32& stored = links[{a, b}];
            stored = std::max(stored, f32(area));
        }
    }
    for (const auto& [ab, area] : links) result.links.push_back({ab.first, ab.second, area});
    return result;
}

// ---- serialization ---------------------------------------------------------------------------------------------

namespace {

constexpr u32 kFractureMagic = 0x5246584Fu; // "OXFR"
constexpr u32 kFractureVersion = 1;

class Writer {
public:
    std::vector<u8> bytes;
    template <class T>
    void pod(const T& v) {
        const auto* p = reinterpret_cast<const u8*>(&v);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    template <class T>
    void array(const std::vector<T>& v) {
        pod(u32(v.size()));
        const auto* p = reinterpret_cast<const u8*>(v.data());
        bytes.insert(bytes.end(), p, p + v.size() * sizeof(T));
    }
};

class Reader {
public:
    explicit Reader(std::span<const u8> b) : m_bytes(b) {}
    template <class T>
    bool pod(T& v) {
        if (m_pos + sizeof(T) > m_bytes.size()) return false;
        std::memcpy(&v, m_bytes.data() + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return true;
    }
    template <class T>
    bool array(std::vector<T>& v) {
        u32 n = 0;
        if (!pod(n) || m_pos + usize(n) * sizeof(T) > m_bytes.size()) return false;
        v.resize(n);
        if (n) std::memcpy(v.data(), m_bytes.data() + m_pos, usize(n) * sizeof(T));
        m_pos += usize(n) * sizeof(T);
        return true;
    }
    [[nodiscard]] bool done() const { return m_pos == m_bytes.size(); }

private:
    std::span<const u8> m_bytes;
    usize m_pos = 0;
};

} // namespace

std::vector<u8> serializeFracture(const FractureResult& r) {
    Writer w;
    w.pod(r.seed);
    w.pod(r.sourceVolume);
    w.pod(r.sourceCentroid);
    w.pod(r.boundsMin);
    w.pod(r.boundsMax);
    w.pod(u32(r.chunks.size()));
    for (const FractureChunk& c : r.chunks) {
        w.array(c.positions);
        w.array(c.normals);
        w.array(c.uvs);
        w.array(c.indices);
        w.pod(c.interiorIndexStart);
        w.array(c.hullPoints);
        w.pod(c.centroid);
        w.pod(c.boundsMin);
        w.pod(c.boundsMax);
        w.pod(c.seed);
        w.pod(c.volume);
    }
    w.array(r.links);
    Writer out;
    out.pod(kFractureMagic);
    out.pod(kFractureVersion);
    out.pod(crc32(w.bytes.data(), w.bytes.size()));
    out.pod(u64(w.bytes.size()));
    out.bytes.insert(out.bytes.end(), w.bytes.begin(), w.bytes.end());
    return std::move(out.bytes);
}

std::optional<FractureResult> deserializeFracture(std::span<const u8> bytes) {
    Reader header(bytes);
    u32 magic = 0, version = 0, crc = 0;
    u64 size = 0;
    if (!header.pod(magic) || !header.pod(version) || !header.pod(crc) || !header.pod(size)) return std::nullopt;
    constexpr usize kHeader = 4 + 4 + 4 + 8;
    if (magic != kFractureMagic || version != kFractureVersion || size != bytes.size() - kHeader) return std::nullopt;
    const std::span<const u8> payload = bytes.subspan(kHeader);
    if (crc32(payload.data(), payload.size()) != crc) return std::nullopt;
    Reader rd(payload);
    FractureResult r;
    u32 chunkCount = 0;
    if (!rd.pod(r.seed) || !rd.pod(r.sourceVolume) || !rd.pod(r.sourceCentroid) || !rd.pod(r.boundsMin) ||
        !rd.pod(r.boundsMax) || !rd.pod(chunkCount)) {
        return std::nullopt;
    }
    r.chunks.resize(chunkCount);
    for (FractureChunk& c : r.chunks) {
        if (!rd.array(c.positions) || !rd.array(c.normals) || !rd.array(c.uvs) || !rd.array(c.indices) ||
            !rd.pod(c.interiorIndexStart) || !rd.array(c.hullPoints) || !rd.pod(c.centroid) || !rd.pod(c.boundsMin) ||
            !rd.pod(c.boundsMax) || !rd.pod(c.seed) || !rd.pod(c.volume)) {
            return std::nullopt;
        }
        if (c.normals.size() != c.positions.size() || c.uvs.size() != c.positions.size() ||
            c.interiorIndexStart > c.indices.size()) {
            return std::nullopt;
        }
        for (u32 i : c.indices) {
            if (i >= c.positions.size()) return std::nullopt;
        }
    }
    if (!rd.array(r.links) || !rd.done()) return std::nullopt;
    for (const FractureLink& l : r.links) {
        if (l.a >= chunkCount || l.b >= chunkCount) return std::nullopt;
    }
    return r;
}

} // namespace ox::physics
