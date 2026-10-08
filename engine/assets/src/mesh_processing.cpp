#include <oxwald/assets/mesh_processing.hpp>
#include <oxwald/core/profile.hpp>

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace ox::assets {

namespace {

f32 cornerAngle(glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const f32 l1 = glm::length(e1);
    const f32 l2 = glm::length(e2);
    if (l1 <= 0.0f || l2 <= 0.0f) return 0.0f;
    return std::acos(glm::clamp(glm::dot(e1, e2) / (l1 * l2), -1.0f, 1.0f));
}

glm::vec3 anyPerpendicular(glm::vec3 n) {
    const glm::vec3 axis = std::abs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    return glm::normalize(glm::cross(axis, n));
}

// Copies vertex `src` of every stream into slot `dst` of the new streams.
struct Streams {
    std::vector<glm::vec3> positions;
    std::vector<VertexAttributes> attributes;
    std::vector<SkinVertex> skin;
};

} // namespace

void generateNormals(std::span<const glm::vec3> positions, std::span<VertexAttributes> attributes,
                     std::span<const u32> indices) {
    std::vector<glm::vec3> acc(positions.size(), glm::vec3(0.0f));
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        const u32 i[3] = {indices[t], indices[t + 1], indices[t + 2]};
        const glm::vec3 p[3] = {positions[i[0]], positions[i[1]], positions[i[2]]};
        const glm::vec3 fn = glm::cross(p[1] - p[0], p[2] - p[0]);
        const f32 len = glm::length(fn);
        if (len <= 0.0f) continue;
        const glm::vec3 n = fn / len;
        for (int k = 0; k < 3; ++k) acc[i[k]] += n * cornerAngle(p[k], p[(k + 1) % 3], p[(k + 2) % 3]);
    }
    for (usize v = 0; v < attributes.size(); ++v) {
        const f32 len = glm::length(acc[v]);
        attributes[v].normal = len > 0.0f ? acc[v] / len : glm::vec3(0, 1, 0);
    }
}

void generateTangents(std::span<const glm::vec3> positions, std::span<VertexAttributes> attributes,
                      std::span<const u32> indices) {
    std::vector<glm::vec3> tan1(positions.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> tan2(positions.size(), glm::vec3(0.0f));
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        const u32 i[3] = {indices[t], indices[t + 1], indices[t + 2]};
        const glm::vec3 p[3] = {positions[i[0]], positions[i[1]], positions[i[2]]};
        const glm::vec2 w[3] = {attributes[i[0]].uv0, attributes[i[1]].uv0, attributes[i[2]].uv0};
        const glm::vec3 e1 = p[1] - p[0];
        const glm::vec3 e2 = p[2] - p[0];
        const glm::vec2 d1 = w[1] - w[0];
        const glm::vec2 d2 = w[2] - w[0];
        const f32 det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-20f) continue;
        const f32 r = 1.0f / det;
        const glm::vec3 sdir = (e1 * d2.y - e2 * d1.y) * r;
        const glm::vec3 tdir = (e2 * d1.x - e1 * d2.x) * r;
        // MikkTSpace weights by the corner angle so results do not depend on the triangulation density.
        for (int k = 0; k < 3; ++k) {
            const f32 a = cornerAngle(p[k], p[(k + 1) % 3], p[(k + 2) % 3]);
            const f32 sl = glm::length(sdir);
            const f32 tl = glm::length(tdir);
            if (sl > 0.0f) tan1[i[k]] += sdir / sl * a;
            if (tl > 0.0f) tan2[i[k]] += tdir / tl * a;
        }
    }
    for (usize v = 0; v < attributes.size(); ++v) {
        const glm::vec3 n = attributes[v].normal;
        glm::vec3 t = tan1[v] - n * glm::dot(n, tan1[v]);
        const f32 len = glm::length(t);
        if (len < 1e-8f || !std::isfinite(len)) {
            t = anyPerpendicular(n);
        } else {
            t /= len;
        }
        const f32 sign = glm::dot(glm::cross(n, t), tan2[v]) < 0.0f ? -1.0f : 1.0f;
        attributes[v].tangent = glm::vec4(t, sign);
    }
}

void optimizeMesh(MeshData& mesh) {
    OX_PROFILE_ZONE();
    Streams out;
    const bool skinned = mesh.skinned();
    std::vector<u32> newIndices;
    newIndices.reserve(mesh.indices.size());
    for (auto& sm : mesh.submeshes) {
        if (sm.lods.empty()) continue;
        MeshLod& lod0 = sm.lods[0];
        std::vector<u32> local(mesh.indices.begin() + lod0.indexOffset,
                               mesh.indices.begin() + lod0.indexOffset + lod0.indexCount);
        for (u32& i : local) i -= sm.vertexOffset;
        meshopt_optimizeVertexCache(local.data(), local.data(), local.size(), sm.vertexCount);
        std::vector<u32> remap(sm.vertexCount);
        const usize unique = meshopt_optimizeVertexFetchRemap(remap.data(), local.data(), local.size(), sm.vertexCount);
        const u32 newOffset = static_cast<u32>(out.positions.size());
        out.positions.resize(newOffset + unique);
        out.attributes.resize(newOffset + unique);
        if (skinned) out.skin.resize(newOffset + unique);
        for (u32 v = 0; v < sm.vertexCount; ++v) {
            if (remap[v] == ~0u) continue;
            out.positions[newOffset + remap[v]] = mesh.positions[sm.vertexOffset + v];
            out.attributes[newOffset + remap[v]] = mesh.attributes[sm.vertexOffset + v];
            if (skinned) out.skin[newOffset + remap[v]] = mesh.skin[sm.vertexOffset + v];
        }
        lod0.indexOffset = static_cast<u32>(newIndices.size());
        for (u32 i : local) newIndices.push_back(remap[i] + newOffset);
        sm.vertexOffset = newOffset;
        sm.vertexCount = static_cast<u32>(unique);
        sm.lods.resize(1); // other LODs referenced the old vertex order
        lod0.meshletOffset = lod0.meshletCount = 0;
    }
    mesh.positions = std::move(out.positions);
    mesh.attributes = std::move(out.attributes);
    mesh.skin = std::move(out.skin);
    mesh.indices = std::move(newIndices);
    mesh.meshlets.clear();
    mesh.meshletVertices.clear();
    mesh.meshletTriangles.clear();
}

void generateLods(MeshData& mesh, const MeshProcessSettings& settings) {
    OX_PROFILE_ZONE();
    // Rebuild the index buffer as [submesh0 lod0..n][submesh1 lod0..n]...
    std::vector<u32> newIndices;
    newIndices.reserve(mesh.indices.size() * 2);
    for (auto& sm : mesh.submeshes) {
        if (sm.lods.empty()) continue;
        const MeshLod lod0 = sm.lods[0];
        std::vector<u32> base(mesh.indices.begin() + lod0.indexOffset,
                              mesh.indices.begin() + lod0.indexOffset + lod0.indexCount);
        for (u32& i : base) i -= sm.vertexOffset;
        sm.lods.clear();
        auto append = [&](const std::vector<u32>& local, f32 error) {
            MeshLod l;
            l.indexOffset = static_cast<u32>(newIndices.size());
            l.indexCount = static_cast<u32>(local.size());
            l.error = error;
            for (u32 i : local) newIndices.push_back(i + sm.vertexOffset);
            sm.lods.push_back(l);
        };
        append(base, 0.0f);
        if (!settings.generateLods || sm.vertexCount == 0) continue;
        const f32* pos = &mesh.positions[sm.vertexOffset].x;
        const f32 scale = meshopt_simplifyScale(pos, sm.vertexCount, sizeof(glm::vec3));
        usize previous = base.size();
        const f32* normals = &mesh.attributes[sm.vertexOffset].normal.x;
        const f32 normalWeights[3] = {0.5f, 0.5f, 0.5f};
        auto simplify = [&](std::vector<u32>& out, usize target, u32 options, f32& error) {
            return meshopt_simplifyWithAttributes(out.data(), base.data(), base.size(), pos, sm.vertexCount,
                                                  sizeof(glm::vec3), normals, sizeof(VertexAttributes), normalWeights, 3,
                                                  nullptr, target, settings.lodMaxError, options, &error);
        };
        bool permissive = false;
        for (f32 ratio : settings.lodRatios) {
            const usize target = static_cast<usize>(f32(base.size() / 3) * ratio) * 3;
            if (target < 3) break;
            std::vector<u32> lod(base.size());
            f32 error = 0.0f;
            usize count = simplify(lod, target, permissive ? meshopt_SimplifyPermissive : 0, error);
            if (!permissive && count > previous * 9 / 10) {
                // Flat-shaded / heavily seamed meshes cannot collapse along attribute seams: allow it (normals are
                // still part of the error metric, so hard edges are preserved where it matters).
                permissive = true;
                count = simplify(lod, target, meshopt_SimplifyPermissive, error);
            }
            // Stop when simplification stalls (error bound reached): a LOD that is barely smaller is useless.
            if (count == 0 || count > previous * 9 / 10) break;
            lod.resize(count);
            meshopt_optimizeVertexCache(lod.data(), lod.data(), lod.size(), sm.vertexCount);
            append(lod, error * scale);
            previous = count;
        }
    }
    mesh.indices = std::move(newIndices);
}

void buildMeshlets(MeshData& mesh, const MeshProcessSettings& settings) {
    OX_PROFILE_ZONE();
    mesh.meshlets.clear();
    mesh.meshletVertices.clear();
    mesh.meshletTriangles.clear();
    const usize maxV = std::clamp<u32>(settings.meshletMaxVertices, 3, 255);
    const usize maxT = std::clamp<u32>(settings.meshletMaxTriangles & ~3u, 4, 512);
    for (u32 si = 0; si < mesh.submeshes.size(); ++si) {
        auto& sm = mesh.submeshes[si];
        const f32* pos = sm.vertexCount ? &mesh.positions[sm.vertexOffset].x : nullptr;
        for (auto& lod : sm.lods) {
            lod.meshletOffset = static_cast<u32>(mesh.meshlets.size());
            lod.meshletCount = 0;
            if (lod.indexCount == 0) continue;
            std::vector<u32> local(mesh.indices.begin() + lod.indexOffset,
                                   mesh.indices.begin() + lod.indexOffset + lod.indexCount);
            for (u32& i : local) i -= sm.vertexOffset;
            const usize bound = meshopt_buildMeshletsBound(local.size(), maxV, maxT);
            std::vector<meshopt_Meshlet> ml(bound);
            std::vector<u32> mv(bound * maxV);
            std::vector<u8> mt(bound * maxT * 3);
            const usize count = meshopt_buildMeshlets(ml.data(), mv.data(), mt.data(), local.data(), local.size(), pos,
                                                      sm.vertexCount, sizeof(glm::vec3), maxV, maxT,
                                                      settings.meshletConeWeight);
            for (usize m = 0; m < count; ++m) {
                const meshopt_Meshlet& src = ml[m];
                meshopt_optimizeMeshlet(&mv[src.vertex_offset], &mt[src.triangle_offset], src.triangle_count,
                                        src.vertex_count);
                const meshopt_Bounds b = meshopt_computeMeshletBounds(&mv[src.vertex_offset], &mt[src.triangle_offset],
                                                                      src.triangle_count, pos, sm.vertexCount,
                                                                      sizeof(glm::vec3));
                Meshlet out;
                out.center = glm::vec3(b.center[0], b.center[1], b.center[2]);
                out.radius = b.radius;
                out.coneAxis = glm::vec3(b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]);
                out.coneCutoff = b.cone_cutoff;
                out.coneApex = glm::vec3(b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]);
                out.submesh = si;
                out.vertexOffset = static_cast<u32>(mesh.meshletVertices.size());
                out.triangleOffset = static_cast<u32>(mesh.meshletTriangles.size());
                out.vertexCount = src.vertex_count;
                out.triangleCount = src.triangle_count;
                for (u32 v = 0; v < src.vertex_count; ++v) {
                    mesh.meshletVertices.push_back(mv[src.vertex_offset + v] + sm.vertexOffset);
                }
                mesh.meshletTriangles.insert(mesh.meshletTriangles.end(), mt.begin() + src.triangle_offset,
                                             mt.begin() + src.triangle_offset + src.triangle_count * 3);
                while (mesh.meshletTriangles.size() % 4 != 0) mesh.meshletTriangles.push_back(0);
                mesh.meshlets.push_back(out);
                ++lod.meshletCount;
            }
        }
    }
}

void computeBounds(MeshData& mesh) {
    mesh.bounds = AABB{};
    for (auto& sm : mesh.submeshes) {
        sm.bounds = AABB{};
        for (u32 v = 0; v < sm.vertexCount; ++v) sm.bounds.expand(mesh.positions[sm.vertexOffset + v]);
        mesh.bounds.expand(sm.bounds);
    }
    if (!mesh.bounds.valid()) {
        for (const auto& p : mesh.positions) mesh.bounds.expand(p);
    }
    mesh.boundingSphere = Sphere{};
    if (!mesh.bounds.valid()) return;
    mesh.boundingSphere.center = mesh.bounds.center();
    f32 r2 = 0.0f;
    for (const auto& p : mesh.positions) r2 = std::max(r2, glm::dot(p - mesh.boundingSphere.center, p - mesh.boundingSphere.center));
    mesh.boundingSphere.radius = std::sqrt(r2);
}

namespace {

std::vector<glm::vec3> fibonacciDirections(u32 n) {
    std::vector<glm::vec3> dirs;
    dirs.reserve(n);
    const f32 golden = glm::pi<f32>() * (3.0f - std::sqrt(5.0f));
    for (u32 i = 0; i < n; ++i) {
        const f32 y = 1.0f - (f32(i) + 0.5f) / f32(n) * 2.0f;
        const f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const f32 phi = golden * f32(i);
        dirs.emplace_back(std::cos(phi) * r, y, std::sin(phi) * r);
    }
    return dirs;
}

std::vector<glm::vec3> supportPoints(std::span<const glm::vec3> points, u32 directions) {
    std::vector<u32> picked;
    for (const glm::vec3& d : fibonacciDirections(directions)) {
        u32 best = 0;
        f32 bestDot = -std::numeric_limits<f32>::infinity();
        for (u32 i = 0; i < points.size(); ++i) {
            const f32 v = glm::dot(points[i], d);
            if (v > bestDot) {
                bestDot = v;
                best = i;
            }
        }
        picked.push_back(best);
    }
    std::sort(picked.begin(), picked.end());
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
    std::vector<glm::vec3> out;
    for (u32 i : picked) out.push_back(points[i]);
    return out;
}

ConvexHull quickHull(std::span<const glm::vec3> pts) {
    ConvexHull hull;
    const usize n = pts.size();
    if (n < 4) {
        hull.vertices.assign(pts.begin(), pts.end());
        return hull;
    }
    AABB box;
    for (const auto& p : pts) box.expand(p);
    const f32 eps = std::max(1e-6f, glm::length(box.size()) * 1e-5f);

    // Initial tetrahedron from extreme points.
    u32 i0 = 0, i1 = 0;
    for (u32 i = 0; i < n; ++i) {
        if (pts[i].x < pts[i0].x) i0 = i;
    }
    f32 best = -1.0f;
    for (u32 i = 0; i < n; ++i) {
        const f32 d = glm::distance(pts[i], pts[i0]);
        if (d > best) {
            best = d;
            i1 = i;
        }
    }
    u32 i2 = 0;
    best = -1.0f;
    const glm::vec3 dir01 = glm::normalize(pts[i1] - pts[i0]);
    for (u32 i = 0; i < n; ++i) {
        const glm::vec3 v = pts[i] - pts[i0];
        const f32 d = glm::length(v - dir01 * glm::dot(v, dir01));
        if (d > best) {
            best = d;
            i2 = i;
        }
    }
    if (best < eps) {
        hull.vertices.assign(pts.begin(), pts.end());
        return hull;
    }
    const glm::vec3 pn = glm::normalize(glm::cross(pts[i1] - pts[i0], pts[i2] - pts[i0]));
    u32 i3 = 0;
    best = -1.0f;
    for (u32 i = 0; i < n; ++i) {
        const f32 d = std::abs(glm::dot(pts[i] - pts[i0], pn));
        if (d > best) {
            best = d;
            i3 = i;
        }
    }
    if (best < eps) { // planar input
        hull.vertices.assign(pts.begin(), pts.end());
        return hull;
    }

    struct Face {
        u32 v[3];
        glm::vec3 n;
        f32 d;
        bool alive;
    };
    std::vector<Face> faces;
    const glm::vec3 centroid = (pts[i0] + pts[i1] + pts[i2] + pts[i3]) * 0.25f;
    auto makeFace = [&](u32 a, u32 b, u32 c) {
        Face f{{a, b, c}, glm::cross(pts[b] - pts[a], pts[c] - pts[a]), 0.0f, true};
        const f32 len = glm::length(f.n);
        f.n = len > 0.0f ? f.n / len : glm::vec3(0.0f);
        f.d = glm::dot(f.n, pts[a]);
        return f;
    };
    auto addOriented = [&](u32 a, u32 b, u32 c) {
        Face f = makeFace(a, b, c);
        if (glm::dot(f.n, centroid) - f.d > 0.0f) f = makeFace(a, c, b);
        faces.push_back(f);
    };
    addOriented(i0, i1, i2);
    addOriented(i0, i1, i3);
    addOriented(i0, i2, i3);
    addOriented(i1, i2, i3);

    std::vector<u8> used(n, 0);
    used[i0] = used[i1] = used[i2] = used[i3] = 1;
    std::vector<usize> visible;
    std::unordered_set<u64> visibleEdges;
    std::vector<std::pair<u32, u32>> horizon;
    auto edgeKey = [](u32 a, u32 b) { return (u64(a) << 32) | b; };
    for (u32 p = 0; p < n; ++p) {
        if (used[p]) continue;
        visible.clear();
        for (usize f = 0; f < faces.size(); ++f) {
            if (faces[f].alive && glm::dot(faces[f].n, pts[p]) - faces[f].d > eps) visible.push_back(f);
        }
        if (visible.empty()) continue;
        visibleEdges.clear();
        for (usize f : visible) {
            const auto& v = faces[f].v;
            for (int k = 0; k < 3; ++k) visibleEdges.insert(edgeKey(v[k], v[(k + 1) % 3]));
        }
        horizon.clear();
        for (usize f : visible) {
            const auto& v = faces[f].v;
            for (int k = 0; k < 3; ++k) {
                const u32 a = v[k], b = v[(k + 1) % 3];
                if (!visibleEdges.count(edgeKey(b, a))) horizon.emplace_back(a, b);
            }
            faces[f].alive = false;
        }
        for (auto [a, b] : horizon) faces.push_back(makeFace(a, b, p));
        used[p] = 1;
        // Compact dead faces occasionally to keep the scan linear in the live face count.
        if (faces.size() > 64 && faces.size() > 4 * n) {
            std::erase_if(faces, [](const Face& f) { return !f.alive; });
        }
        if ((p & 63) == 0) std::erase_if(faces, [](const Face& f) { return !f.alive; });
    }
    std::unordered_map<u32, u32> remap;
    for (const auto& f : faces) {
        if (!f.alive) continue;
        for (u32 v : f.v) {
            auto [it, inserted] = remap.try_emplace(v, static_cast<u32>(hull.vertices.size()));
            if (inserted) hull.vertices.push_back(pts[v]);
            hull.indices.push_back(it->second);
        }
    }
    return hull;
}

} // namespace

ConvexHull computeConvexHull(std::span<const glm::vec3> points, u32 maxVertices) {
    OX_PROFILE_ZONE();
    std::vector<glm::vec3> input(points.begin(), points.end());
    if (input.size() > 4096) input = supportPoints(input, 1024);
    ConvexHull hull = quickHull(input);
    if (maxVertices >= 4 && hull.vertices.size() > maxVertices) {
        // Support points of evenly spread directions keep the silhouette in every direction.
        for (u32 dirs = maxVertices; dirs >= 4; dirs = dirs * 3 / 4) {
            auto reduced = supportPoints(hull.vertices, dirs);
            ConvexHull h = quickHull(reduced);
            if (h.vertices.size() <= maxVertices) return h;
        }
    }
    return hull;
}

MeshCollisionData buildCollisionData(const MeshData& mesh, const MeshProcessSettings& settings) {
    OX_PROFILE_ZONE();
    MeshCollisionData out;
    std::vector<u32> indices;
    for (const auto& sm : mesh.submeshes) {
        if (sm.lods.empty()) continue;
        indices.insert(indices.end(), mesh.indices.begin() + sm.lods[0].indexOffset,
                       mesh.indices.begin() + sm.lods[0].indexOffset + sm.lods[0].indexCount);
    }
    if (indices.empty() || mesh.positions.empty()) return out;
    // Weld by position only (UV/normal seams are irrelevant for physics).
    std::vector<u32> remap(mesh.positions.size());
    const usize unique = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(), mesh.positions.data(),
                                                     mesh.positions.size(), sizeof(glm::vec3));
    std::vector<glm::vec3> welded(unique);
    meshopt_remapVertexBuffer(welded.data(), mesh.positions.data(), mesh.positions.size(), sizeof(glm::vec3),
                              remap.data());
    std::vector<u32> weldedIdx(indices.size());
    meshopt_remapIndexBuffer(weldedIdx.data(), indices.data(), indices.size(), remap.data());

    const usize target = std::max<usize>(3, usize(f32(weldedIdx.size() / 3) * settings.collisionSimplifyRatio) * 3);
    std::vector<u32> simplified(weldedIdx.size());
    usize count = meshopt_simplify(simplified.data(), weldedIdx.data(), weldedIdx.size(), &welded[0].x, welded.size(),
                                   sizeof(glm::vec3), target, 0.02f, meshopt_SimplifyLockBorder, nullptr);
    simplified.resize(count);
    std::vector<u32> fetch(welded.size());
    const usize used = meshopt_optimizeVertexFetchRemap(fetch.data(), simplified.data(), simplified.size(), welded.size());
    out.vertices.resize(used);
    for (usize v = 0; v < welded.size(); ++v) {
        if (fetch[v] != ~0u) out.vertices[fetch[v]] = welded[v];
    }
    out.indices.resize(simplified.size());
    for (usize i = 0; i < simplified.size(); ++i) out.indices[i] = fetch[simplified[i]];

    ConvexHull hull = computeConvexHull(welded, settings.maxHullVertices);
    out.hullVertices = std::move(hull.vertices);
    out.hullIndices = std::move(hull.indices);
    return out;
}

void processMesh(MeshData& mesh, const MeshProcessSettings& settings) {
    OX_PROFILE_ZONE();
    if (settings.optimizeVertexCache) optimizeMesh(mesh);
    generateLods(mesh, settings);
    if (settings.generateMeshlets) {
        buildMeshlets(mesh, settings);
    } else {
        mesh.meshlets.clear();
        mesh.meshletVertices.clear();
        mesh.meshletTriangles.clear();
        for (auto& sm : mesh.submeshes) {
            for (auto& l : sm.lods) l.meshletOffset = l.meshletCount = 0;
        }
    }
    computeBounds(mesh);
    if (settings.generateCollision) mesh.collision = buildCollisionData(mesh, settings);
}

} // namespace ox::assets
