#include <oxwald/spline/spline_mesh.hpp>

#include <oxwald/core/log.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ox::spline {

namespace {

glm::vec2 edgeNormal(const glm::vec2& a, const glm::vec2& b) {
    const glm::vec2 d = b - a;
    const f32 len = glm::length(d);
    return len > 1e-6f ? glm::vec2(d.y, -d.x) / len : glm::vec2(0.0f);
}

void addCap(ExtrudedMesh& mesh, const SplineSample& s, const std::vector<glm::vec2>& pts, bool facingForward) {
    const u32 base = static_cast<u32>(mesh.vertices.size());
    const glm::vec3 n = facingForward ? s.tangent : -s.tangent;
    glm::vec2 centroid(0.0f);
    glm::vec2 lo = pts[0];
    glm::vec2 hi = pts[0];
    for (const glm::vec2& p : pts) {
        centroid += p;
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    centroid /= static_cast<f32>(pts.size());
    const glm::vec2 extent = glm::max(hi - lo, glm::vec2(1e-6f));
    auto toWorld = [&](const glm::vec2& p) { return s.position + s.binormal * p.x + s.normal * p.y; };
    mesh.vertices.push_back({toWorld(centroid), n, (centroid - lo) / extent});
    for (const glm::vec2& p : pts) {
        mesh.vertices.push_back({toWorld(p), n, (p - lo) / extent});
    }
    const u32 count = static_cast<u32>(pts.size());
    for (u32 i = 0; i < count; ++i) {
        u32 a = base + 1 + i;
        u32 b = base + 1 + (i + 1) % count;
        const glm::vec3 faceN = glm::cross(mesh.vertices[a].position - mesh.vertices[base].position,
                                           mesh.vertices[b].position - mesh.vertices[base].position);
        if (glm::dot(faceN, n) < 0.0f) {
            std::swap(a, b);
        }
        mesh.indices.insert(mesh.indices.end(), {base, a, b});
    }
}

} // namespace

ExtrudedMesh extrude(const Spline& spline, const ExtrusionProfile& profile, const ExtrusionSettings& settings) {
    ExtrudedMesh mesh;
    const usize pointCount = profile.points.size();
    if (pointCount < 2 || spline.segmentCount() == 0) {
        OX_LOG_WARN("spline", "extrude: need a profile with >= 2 points and a spline with >= 1 segment");
        return mesh;
    }
    const std::vector<SplineSample> rings = spline.sampleByDistance(settings.spacing);
    const u32 ringCount = static_cast<u32>(rings.size());
    const u32 columns = static_cast<u32>(pointCount + (profile.closed ? 1 : 0));

    // Profile normals (smoothed across non-degenerate neighbouring edges) and u coordinates.
    std::vector<glm::vec2> normals(pointCount, glm::vec2(0.0f));
    for (usize i = 0; i < pointCount; ++i) {
        glm::vec2 n(0.0f);
        if (i > 0 || profile.closed) {
            n += edgeNormal(profile.points[(i + pointCount - 1) % pointCount], profile.points[i]);
        }
        if (i + 1 < pointCount || profile.closed) {
            n += edgeNormal(profile.points[i], profile.points[(i + 1) % pointCount]);
        }
        const f32 len = glm::length(n);
        normals[i] = len > 1e-6f ? n / len : glm::vec2(0.0f, 1.0f);
    }
    std::vector<f32> us(columns, 0.0f);
    if (profile.u.size() >= pointCount) {
        for (u32 c = 0; c < columns; ++c) {
            us[c] = c < profile.u.size() ? profile.u[c] : 1.0f;
        }
    } else {
        for (u32 c = 1; c < columns; ++c) {
            us[c] = us[c - 1] + glm::length(profile.points[c % pointCount] - profile.points[c - 1]);
        }
        if (us.back() > 0.0f) {
            const f32 inv = 1.0f / us.back();
            for (f32& u : us) {
                u *= inv;
            }
        }
    }

    const f32 total = spline.length();
    mesh.vertices.reserve(static_cast<usize>(ringCount) * columns);
    for (u32 r = 0; r < ringCount; ++r) {
        const SplineSample& s = rings[r];
        const f32 v = total * static_cast<f32>(r) / static_cast<f32>(ringCount - 1) * settings.vPerUnit;
        for (u32 c = 0; c < columns; ++c) {
            const glm::vec2& p = profile.points[c % pointCount];
            const glm::vec2& n = normals[c % pointCount];
            mesh.vertices.push_back({s.position + s.binormal * p.x + s.normal * p.y,
                                     glm::normalize(s.binormal * n.x + s.normal * n.y), glm::vec2(us[c], v)});
        }
    }
    mesh.indices.reserve(static_cast<usize>(ringCount - 1) * (columns - 1) * 6);
    for (u32 r = 0; r + 1 < ringCount; ++r) {
        for (u32 c = 0; c + 1 < columns; ++c) {
            const u32 a = r * columns + c;
            const u32 b = a + 1;
            const u32 cc = a + columns;
            const u32 d = cc + 1;
            mesh.indices.insert(mesh.indices.end(), {a, cc, b, b, cc, d});
        }
    }

    if (profile.closed && !spline.closed()) {
        if (settings.capStart) {
            addCap(mesh, rings.front(), profile.points, false);
        }
        if (settings.capEnd) {
            addCap(mesh, rings.back(), profile.points, true);
        }
    }
    return mesh;
}

ExtrusionProfile makeCircleProfile(f32 radius, u32 segments) {
    ExtrusionProfile p;
    p.closed = true;
    segments = std::max(segments, 3u);
    for (u32 i = 0; i < segments; ++i) {
        const f32 a = glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(segments);
        p.points.emplace_back(radius * std::cos(a), radius * std::sin(a));
    }
    return p;
}

ExtrusionProfile makeRectangleProfile(f32 width, f32 height) {
    const f32 x = 0.5f * width;
    const f32 y = 0.5f * height;
    ExtrusionProfile p;
    p.closed = true;
    // Each corner twice so the sides get flat normals.
    p.points = {{x, -y}, {x, y}, {x, y}, {-x, y}, {-x, y}, {-x, -y}, {-x, -y}, {x, -y}};
    return p;
}

ExtrusionProfile makeStripProfile(f32 width) {
    ExtrusionProfile p;
    p.points = {{0.5f * width, 0.0f}, {-0.5f * width, 0.0f}};
    p.u = {1.0f, 0.0f};
    return p;
}

} // namespace ox::spline
