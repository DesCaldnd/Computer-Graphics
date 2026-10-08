#include <oxwald/core/uuid.hpp>
#include <oxwald/render/mesh_primitives.hpp>

#include <algorithm>
#include <cmath>

namespace ox::render {

namespace {

struct Builder {
    assets::MeshData mesh;

    u32 vertex(glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
        mesh.positions.push_back(p);
        assets::VertexAttributes a;
        a.normal = glm::normalize(n);
        a.uv0 = uv;
        a.uv1 = uv;
        mesh.attributes.push_back(a);
        return u32(mesh.positions.size() - 1);
    }
    // Front faces are counter-clockwise seen from the side the vertex normals point to.
    void tri(u32 a, u32 b, u32 c) {
        const glm::vec3 pa = mesh.positions[a], pb = mesh.positions[b], pc = mesh.positions[c];
        const glm::vec3 gn = glm::cross(pb - pa, pc - pa);
        const glm::vec3 vn = mesh.attributes[a].normal + mesh.attributes[b].normal + mesh.attributes[c].normal;
        if (glm::dot(gn, vn) < 0.0f) std::swap(b, c);
        mesh.indices.insert(mesh.indices.end(), {a, b, c});
    }
    void quad(u32 a, u32 b, u32 c, u32 d) {
        tri(a, b, c);
        tri(a, c, d);
    }

    // Grid of (cols+1)×(rows+1) vertices from fn(u, v) → (position, normal); u, v in [0,1].
    template <class Fn>
    void grid(u32 cols, u32 rows, glm::vec2 uvScale, Fn&& fn) {
        const u32 base = u32(mesh.positions.size());
        for (u32 r = 0; r <= rows; ++r) {
            for (u32 c = 0; c <= cols; ++c) {
                const f32 u = f32(c) / f32(cols), v = f32(r) / f32(rows);
                auto [p, n] = fn(u, v);
                vertex(p, n, glm::vec2(u, v) * uvScale);
            }
        }
        for (u32 r = 0; r < rows; ++r) {
            for (u32 c = 0; c < cols; ++c) {
                const u32 i0 = base + r * (cols + 1) + c;
                const u32 i1 = i0 + 1, i2 = i0 + cols + 2, i3 = i0 + cols + 1;
                // Skip degenerate triangles at poles.
                const auto degenerate = [&](u32 a, u32 b, u32 c2) {
                    return glm::length(glm::cross(mesh.positions[b] - mesh.positions[a],
                                                  mesh.positions[c2] - mesh.positions[a])) < 1e-9f;
                };
                if (!degenerate(i0, i1, i2)) tri(i0, i1, i2);
                if (!degenerate(i0, i2, i3)) tri(i0, i2, i3);
            }
        }
    }

    void disk(glm::vec3 center, glm::vec3 normal, f32 radius, u32 segments, f32 uvScale) {
        const u32 c = vertex(center, normal, glm::vec2(0.5f) * uvScale);
        const u32 first = u32(mesh.positions.size());
        for (u32 i = 0; i <= segments; ++i) {
            const f32 a = kTwoPi * f32(i) / f32(segments);
            const glm::vec3 p = center + glm::vec3(std::cos(a), 0.0f, std::sin(a)) * radius;
            vertex(p, normal, (glm::vec2(std::cos(a), std::sin(a)) * 0.5f + 0.5f) * uvScale);
        }
        for (u32 i = 0; i < segments; ++i) tri(c, first + i, first + i + 1);
    }
};

assets::MeshData finish(Builder& b, const char* name) {
    assets::MeshData& m = b.mesh;
    m.name = name;
    assets::Submesh sm;
    sm.name = name;
    sm.vertexCount = u32(m.positions.size());
    assets::MeshLod lod;
    lod.indexCount = u32(m.indices.size());
    sm.lods.push_back(lod);
    m.submeshes.push_back(sm);
    m.materials.push_back({"Default", Uuid{}});
    computeTangents(m);
    computeBounds(m);
    return std::move(m);
}

} // namespace

const char* primitiveName(Primitive p) {
    switch (p) {
    case Primitive::Cube: return "Cube";
    case Primitive::Sphere: return "Sphere";
    case Primitive::Plane: return "Plane";
    case Primitive::Cylinder: return "Cylinder";
    case Primitive::Capsule: return "Capsule";
    case Primitive::Cone: return "Cone";
    case Primitive::Torus: return "Torus";
    default: return "?";
    }
}

Uuid primitiveUuid(Primitive p) { return Uuid::fromName(std::string("ox.render.primitive.") + primitiveName(p)); }

assets::MeshData makePrimitive(Primitive p, const PrimitiveParams& params) {
    Builder b;
    const f32 s = params.size;
    const u32 seg = std::max(params.segments, 3u);
    const u32 rings = std::max(params.rings, 2u);
    const glm::vec2 uvs{params.uvScale};
    switch (p) {
    case Primitive::Cube: {
        const f32 h = s * 0.5f;
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const glm::vec3& n : normals) {
            const glm::vec3 up = std::abs(n.y) > 0.5f ? glm::vec3(0, 0, -1) : glm::vec3(0, 1, 0);
            const glm::vec3 right = glm::cross(up, n);
            b.grid(1, 1, uvs, [&](f32 u, f32 v) {
                const glm::vec3 pos = n * h + right * ((u - 0.5f) * s) + up * ((0.5f - v) * s);
                return std::pair{pos, n};
            });
        }
        break;
    }
    case Primitive::Sphere: {
        const f32 r = s * 0.5f;
        b.grid(seg, rings, uvs, [&](f32 u, f32 v) {
            const f32 theta = v * kPi, phi = u * kTwoPi;
            const glm::vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi)};
            return std::pair{n * r, n};
        });
        break;
    }
    case Primitive::Plane: {
        const u32 sub = std::max(1u, params.segments / 8);
        b.grid(sub, sub, uvs, [&](f32 u, f32 v) {
            return std::pair{glm::vec3((u - 0.5f) * s, 0.0f, (v - 0.5f) * s), glm::vec3(0, 1, 0)};
        });
        break;
    }
    case Primitive::Cylinder: {
        const f32 r = s * 0.5f, h = params.height * 0.5f;
        b.grid(seg, 1, uvs, [&](f32 u, f32 v) {
            const f32 a = u * kTwoPi;
            const glm::vec3 n{std::cos(a), 0.0f, -std::sin(a)};
            return std::pair{n * r + glm::vec3(0, h - v * 2.0f * h, 0), n};
        });
        b.disk({0, h, 0}, {0, 1, 0}, r, seg, params.uvScale);
        b.disk({0, -h, 0}, {0, -1, 0}, r, seg, params.uvScale);
        break;
    }
    case Primitive::Capsule: {
        const f32 r = s * 0.5f;
        const f32 half = std::max(params.height * 0.5f - r, 0.0f);
        const u32 hr = std::max(rings / 2, 2u);
        // Top hemisphere, cylinder, bottom hemisphere as one grid (v over the profile).
        b.grid(seg, hr * 2 + 1, uvs, [&](f32 u, f32 v) {
            const f32 phi = u * kTwoPi;
            const f32 t = v * f32(hr * 2 + 1);
            f32 theta, yOff;
            if (t <= f32(hr)) {
                theta = t / f32(hr) * kHalfPi;
                yOff = half;
            } else if (t >= f32(hr + 1)) {
                theta = kHalfPi + (t - f32(hr + 1)) / f32(hr) * kHalfPi;
                yOff = -half;
            } else {
                theta = kHalfPi;
                yOff = half - (t - f32(hr)) * 2.0f * half;
            }
            const glm::vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi)};
            return std::pair{n * r + glm::vec3(0, yOff, 0), n};
        });
        break;
    }
    case Primitive::Cone: {
        const f32 r = s * 0.5f, h = params.height;
        const f32 slope = r / h;
        b.grid(seg, 1, uvs, [&](f32 u, f32 v) {
            const f32 a = u * kTwoPi;
            const glm::vec3 radial{std::cos(a), 0.0f, -std::sin(a)};
            const glm::vec3 n = glm::normalize(radial + glm::vec3(0, slope, 0));
            const f32 rr = r * v;
            return std::pair{radial * rr + glm::vec3(0, h * 0.5f - v * h, 0), n};
        });
        b.disk({0, -h * 0.5f, 0}, {0, -1, 0}, r, seg, params.uvScale);
        break;
    }
    case Primitive::Torus: {
        const f32 minor = params.minorRadius;
        const f32 major = std::max(s * 0.5f - minor, minor);
        b.grid(seg, std::max(rings, 8u), uvs, [&](f32 u, f32 v) {
            const f32 a = u * kTwoPi, t = v * kTwoPi;
            const glm::vec3 ring{std::cos(a), 0.0f, -std::sin(a)};
            const glm::vec3 n = ring * std::cos(t) + glm::vec3(0, std::sin(t), 0);
            return std::pair{ring * major + n * minor, n};
        });
        break;
    }
    default: break;
    }
    return finish(b, primitiveName(p));
}

void computeBounds(assets::MeshData& mesh) {
    AABB box;
    for (const glm::vec3& p : mesh.positions) box.expand(p);
    mesh.bounds = box;
    const glm::vec3 c = box.valid() ? box.center() : glm::vec3(0.0f);
    f32 r2 = 0.0f;
    for (const glm::vec3& p : mesh.positions) r2 = std::max(r2, glm::dot(p - c, p - c));
    mesh.boundingSphere = {c, std::sqrt(r2)};
    for (assets::Submesh& sm : mesh.submeshes) {
        AABB sb;
        for (u32 i = sm.vertexOffset; i < sm.vertexOffset + sm.vertexCount && i < mesh.positions.size(); ++i) {
            sb.expand(mesh.positions[i]);
        }
        sm.bounds = sb;
    }
}

void computeTangents(assets::MeshData& mesh) {
    const usize n = mesh.positions.size();
    std::vector<glm::vec3> tan(n, glm::vec3(0.0f)), bit(n, glm::vec3(0.0f));
    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const u32 a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        const glm::vec3 e1 = mesh.positions[b] - mesh.positions[a], e2 = mesh.positions[c] - mesh.positions[a];
        const glm::vec2 d1 = mesh.attributes[b].uv0 - mesh.attributes[a].uv0;
        const glm::vec2 d2 = mesh.attributes[c].uv0 - mesh.attributes[a].uv0;
        const f32 det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) continue;
        const f32 r = 1.0f / det;
        const glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        const glm::vec3 bt = (e2 * d1.x - e1 * d2.x) * r;
        for (u32 v : {a, b, c}) {
            tan[v] += t;
            bit[v] += bt;
        }
    }
    for (usize i = 0; i < n; ++i) {
        const glm::vec3 nn = mesh.attributes[i].normal;
        glm::vec3 t = tan[i] - nn * glm::dot(nn, tan[i]);
        if (glm::dot(t, t) < 1e-12f) {
            const glm::vec3 helper = std::abs(nn.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            t = glm::cross(helper, nn);
        }
        t = glm::normalize(t);
        const f32 w = glm::dot(glm::cross(nn, t), bit[i]) < 0.0f ? -1.0f : 1.0f;
        mesh.attributes[i].tangent = glm::vec4(t, w);
    }
}

} // namespace ox::render
