#include <oxwald/world/terrain_lod.hpp>

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ox::world {

LodRanges LodRanges::compute(const TerrainLodSettings& s) {
    LodRanges r;
    const u32 n = std::max(1u, s.lodCount);
    r.range.resize(n);
    r.morph.resize(n);
    f32 total = 0.f, balance = 1.f;
    for (u32 i = 0; i < n; ++i) {
        total += balance;
        balance *= s.detailBalance;
    }
    const f32 sect = s.viewDistance / total;
    f32 prev = 0.f;
    balance = 1.f;
    for (u32 i = 0; i < n; ++i) {
        r.range[i] = prev + sect * balance;
        prev = r.range[i];
        balance *= s.detailBalance;
    }
    prev = 0.f;
    for (u32 i = 0; i < n; ++i) {
        const f32 end = r.range[i];
        const f32 start = prev + (end - prev) * s.morphStartRatio;
        r.morph[i] = {start, end};
        prev = start;
    }
    return r;
}

TerrainPatchGpu toGpu(const TerrainPatch& p) {
    const f32 span = std::max(1e-4f, p.morphRange.y - p.morphRange.x);
    return {{p.offset.x, p.offset.y, p.size, f32(p.lod)}, {p.morphRange.x, p.morphRange.y, 1.f / span, f32(p.quadrantMask)}};
}

f32 cdlodMorphFactor(f32 distance, glm::vec2 morphRange) {
    const f32 span = std::max(1e-4f, morphRange.y - morphRange.x);
    return glm::clamp((distance - morphRange.x) / span, 0.f, 1.f);
}

glm::vec2 cdlodMorphVertex(glm::vec2 uv, u32 gridDim, f32 morphK) {
    const f32 g = f32(gridDim);
    const glm::vec2 gridPos = uv * g;
    // 1 for odd grid vertices, 0 for even ones; odd vertices slide onto their even neighbour.
    const glm::vec2 frac = glm::fract(gridPos * 0.5f) * 2.f;
    return uv - frac / g * morphK;
}

TerrainQuadtree::TerrainQuadtree(const Heightfield& hf, const TerrainLodSettings& s) { build(hf, s); }

void TerrainQuadtree::build(const Heightfield& hf, const TerrainLodSettings& s) {
    OX_ASSERT(s.leafNodeSize >= 2 && s.leafNodeSize % 2 == 0, "leafNodeSize must be even");
    OX_ASSERT(s.lodCount >= 1 && s.lodCount <= 16, "lodCount must be in [1,16]");
    m_settings = s;
    m_ranges = LodRanges::compute(s);
    m_origin = hf.desc().origin;
    m_spacing = hf.spacing();
    m_quads = hf.resolution() - 1;
    if (m_quads % s.leafNodeSize != 0) {
        OX_LOG_WARN("world", "terrain: (resolution-1)={} is not a multiple of leafNodeSize={}; border nodes overhang",
                    m_quads, s.leafNodeSize);
    }
    m_levels.assign(s.lodCount, {});
    for (u32 l = 0; l < s.lodCount; ++l) {
        const u32 nodeQuads = s.leafNodeSize << l;
        m_levels[l].countX = m_levels[l].countZ = (m_quads + nodeQuads - 1) / nodeQuads;
        m_levels[l].minMax.assign(usize(m_levels[l].countX) * m_levels[l].countZ, glm::vec2(0.f));
    }
    for (u32 z = 0; z < m_levels[0].countZ; ++z) {
        for (u32 x = 0; x < m_levels[0].countX; ++x) {
            computeLeaf(hf, x, z);
        }
    }
    for (u32 l = 1; l < s.lodCount; ++l) {
        for (u32 z = 0; z < m_levels[l].countZ; ++z) {
            for (u32 x = 0; x < m_levels[l].countX; ++x) {
                propagate(l, x, z);
            }
        }
    }
    // Crack-free condition: a LOD-l vertex bordering an emitted LOD-(l-1) node lies inside that node,
    // which is within range[l-1]; so it is at most range[l-1] + diagonal(l-1) away and must not have
    // started morphing yet. (Sufficient, conservative; height extents are ignored.)
    for (u32 l = 1; l < s.lodCount; ++l) {
        const f32 diag = nodeWorldSize(l - 1) * 1.41421356f;
        if (m_ranges.range[l - 1] + diag > m_ranges.morph[l].x) {
            OX_LOG_WARN("world",
                        "terrain LOD {}: range[{}]={:.1f} + node diagonal {:.1f} > morphStart {:.1f}; cracks are "
                        "possible - increase viewDistance or reduce leafNodeSize",
                        l, l - 1, m_ranges.range[l - 1], diag, m_ranges.morph[l].x);
            break;
        }
    }
}

void TerrainQuadtree::computeLeaf(const Heightfield& hf, u32 nx, u32 nz) {
    const i32 n = i32(m_settings.leafNodeSize);
    f32 lo, hi;
    hf.minMaxInRect({i32(nx) * n, i32(nz) * n, i32(nx) * n + n + 1, i32(nz) * n + n + 1}, lo, hi);
    m_levels[0].minMax[usize(nz) * m_levels[0].countX + nx] = {lo, hi};
}

void TerrainQuadtree::propagate(u32 lod, u32 nx, u32 nz) {
    const Level& c = m_levels[lod - 1];
    glm::vec2 mm{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::lowest()};
    for (u32 q = 0; q < 4; ++q) {
        const u32 cx = nx * 2 + (q & 1), cz = nz * 2 + (q >> 1);
        if (cx < c.countX && cz < c.countZ) {
            const glm::vec2 v = c.minMax[usize(cz) * c.countX + cx];
            mm.x = std::min(mm.x, v.x);
            mm.y = std::max(mm.y, v.y);
        }
    }
    m_levels[lod].minMax[usize(nz) * m_levels[lod].countX + nx] = mm;
}

void TerrainQuadtree::updateBounds(const Heightfield& hf, const IRect& dirty) {
    if (dirty.empty() || m_levels.empty()) {
        return;
    }
    const i32 n = i32(m_settings.leafNodeSize);
    i32 x0 = std::max(0, (dirty.x0 - 1) / n), z0 = std::max(0, (dirty.z0 - 1) / n);
    i32 x1 = std::min(i32(m_levels[0].countX) - 1, (dirty.x1 - 1) / n);
    i32 z1 = std::min(i32(m_levels[0].countZ) - 1, (dirty.z1 - 1) / n);
    for (i32 z = z0; z <= z1; ++z) {
        for (i32 x = x0; x <= x1; ++x) {
            computeLeaf(hf, u32(x), u32(z));
        }
    }
    for (u32 l = 1; l < m_levels.size(); ++l) {
        x0 /= 2;
        z0 /= 2;
        x1 /= 2;
        z1 /= 2;
        for (i32 z = z0; z <= z1; ++z) {
            for (i32 x = x0; x <= x1; ++x) {
                propagate(l, u32(x), u32(z));
            }
        }
    }
}

Aabb TerrainQuadtree::nodeBounds(u32 lod, u32 nx, u32 nz) const {
    const f32 size = nodeWorldSize(lod);
    const glm::vec2 mm = m_levels[lod].minMax[usize(nz) * m_levels[lod].countX + nx];
    const glm::vec2 lo = m_origin + glm::vec2(f32(nx), f32(nz)) * size;
    return {{lo.x, mm.x, lo.y}, {lo.x + size, mm.y, lo.y + size}};
}

f32 TerrainQuadtree::skirtDepth(u32 lod) const {
    return std::max(0.5f, 2.f * nodeWorldSize(lod) / f32(m_settings.leafNodeSize));
}

bool TerrainQuadtree::selectNode(u32 lod, u32 nx, u32 nz, const TerrainSelectParams& p, TerrainSelection& out) const {
    ++out.visitedNodes;
    const Aabb box = nodeBounds(lod, nx, nz);
    const f32 d2 = distanceSq(box, p.cameraPosition);
    const f32 r = m_ranges.range[lod];
    if (d2 > r * r) {
        return false; // parent covers this area at its own LOD
    }
    if (!p.frustum.intersects(box)) {
        return true; // handled (invisible)
    }
    auto emit = [&](u8 mask) {
        if (out.patches.size() >= p.maxPatches) {
            out.truncated = true;
            return;
        }
        TerrainPatch patch;
        patch.offset = {box.min.x, box.min.z};
        patch.size = nodeWorldSize(lod);
        patch.lod = lod;
        patch.morphRange = m_ranges.morph[lod];
        patch.minY = box.min.y;
        patch.maxY = box.max.y;
        patch.quadrantMask = mask;
        out.patches.push_back(patch);
        ++out.patchesPerLod[lod];
    };
    if (lod == 0) {
        emit(0xF);
        return true;
    }
    const f32 rc = m_ranges.range[lod - 1];
    if (d2 > rc * rc) {
        emit(0xF);
        return true;
    }
    const Level& child = m_levels[lod - 1];
    u8 mask = 0;
    for (u32 q = 0; q < 4; ++q) {
        const u32 cx = nx * 2 + (q & 1), cz = nz * 2 + (q >> 1);
        if (cx >= child.countX || cz >= child.countZ) {
            continue; // outside the terrain
        }
        if (!selectNode(lod - 1, cx, cz, p, out)) {
            mask |= u8(1u << q);
        }
    }
    if (mask) {
        emit(mask);
    }
    return true;
}

void TerrainQuadtree::select(const TerrainSelectParams& p, TerrainSelection& out) const {
    out.clear();
    if (m_levels.empty()) {
        return;
    }
    const u32 top = u32(m_levels.size()) - 1;
    for (u32 z = 0; z < m_levels[top].countZ; ++z) {
        for (u32 x = 0; x < m_levels[top].countX; ++x) {
            selectNode(top, x, z, p, out);
        }
    }
}

void TerrainQuadtree::debugDraw(const TerrainSelection& sel, const DebugLineFn& line) const {
    if (!line) {
        return;
    }
    static const glm::vec4 kColors[] = {{1, 0, 0, 1}, {1, 0.5f, 0, 1}, {1, 1, 0, 1}, {0, 1, 0, 1},
                                        {0, 1, 1, 1}, {0, 0.4f, 1, 1}, {0.6f, 0, 1, 1}, {1, 0, 1, 1}};
    for (const TerrainPatch& p : sel.patches) {
        const glm::vec4 c = kColors[p.lod % 8];
        const f32 y = p.maxY + 0.1f;
        const f32 h = p.size * 0.5f;
        for (u32 q = 0; q < 4; ++q) {
            if (!(p.quadrantMask & (1u << q))) {
                continue;
            }
            const glm::vec2 o = p.offset + glm::vec2(f32(q & 1), f32(q >> 1)) * h;
            const glm::vec3 a{o.x, y, o.y}, b{o.x + h, y, o.y}, cc{o.x + h, y, o.y + h}, d{o.x, y, o.y + h};
            line(a, b, c);
            line(b, cc, c);
            line(cc, d, c);
            line(d, a, c);
        }
    }
}

// --- grid mesh -------------------------------------------------------------------------------

TerrainGridMesh generateTerrainGrid(u32 n, bool skirts) {
    OX_ASSERT(n >= 2 && n % 2 == 0, "grid dimension must be even");
    TerrainGridMesh m;
    m.gridDim = n;
    const u32 row = n + 1;
    const f32 inv = 1.f / f32(n);
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            m.vertices.push_back({f32(x) * inv, f32(z) * inv, 0.f});
        }
    }
    auto main = [&](u32 x, u32 z) { return z * row + x; };
    // Skirt vertices: one copy of each border vertex, per edge (north z=0, south z=n, west x=0, east x=n).
    const u32 skirtBase = u32(m.vertices.size());
    if (skirts) {
        for (u32 e = 0; e < 4; ++e) {
            for (u32 i = 0; i <= n; ++i) {
                const u32 x = e == 0 || e == 1 ? i : (e == 2 ? 0 : n);
                const u32 z = e == 2 || e == 3 ? i : (e == 0 ? 0 : n);
                m.vertices.push_back({f32(x) * inv, f32(z) * inv, 1.f});
            }
        }
    }
    auto skirt = [&](u32 edge, u32 i) { return skirtBase + edge * row + i; };
    const u32 h = n / 2;
    for (u32 q = 0; q < 4; ++q) {
        m.quadrants[q].first = u32(m.indices.size());
        const u32 xa = (q & 1) ? h : 0, za = (q >> 1) ? h : 0;
        for (u32 z = za; z < za + h; ++z) {
            for (u32 x = xa; x < xa + h; ++x) {
                const u32 i00 = main(x, z), i10 = main(x + 1, z), i01 = main(x, z + 1), i11 = main(x + 1, z + 1);
                m.indices.insert(m.indices.end(), {i00, i01, i10, i10, i01, i11});
            }
        }
        if (skirts) {
            // Skirt quad from top edge t0→t1 (direction d) to its skirt copies; outward normal = (d.z, 0, -d.x).
            auto quad = [&](u32 t0, u32 t1, u32 b0, u32 b1) { m.indices.insert(m.indices.end(), {t0, t1, b0, t1, b1, b0}); };
            for (u32 i = xa; i < xa + h; ++i) {
                if (za == 0) { // north edge, outward -Z: direction +X
                    quad(main(i, 0), main(i + 1, 0), skirt(0, i), skirt(0, i + 1));
                } else { // south edge, outward +Z: direction -X
                    quad(main(i + 1, n), main(i, n), skirt(1, i + 1), skirt(1, i));
                }
            }
            for (u32 i = za; i < za + h; ++i) {
                if (xa == 0) { // west edge, outward -X: direction -Z
                    quad(main(0, i + 1), main(0, i), skirt(2, i + 1), skirt(2, i));
                } else { // east edge, outward +X: direction +Z
                    quad(main(n, i), main(n, i + 1), skirt(3, i), skirt(3, i + 1));
                }
            }
        }
        m.quadrants[q].count = u32(m.indices.size()) - m.quadrants[q].first;
    }
    return m;
}

// --- physics ---------------------------------------------------------------------------------

PhysicsHeightfieldTile buildPhysicsTile(const Heightfield& hf, i32 tileX, i32 tileZ, u32 tileQuads) {
    PhysicsHeightfieldTile t;
    t.tileX = tileX;
    t.tileZ = tileZ;
    t.sampleCount = tileQuads + 1;
    const i32 x0 = tileX * i32(tileQuads), z0 = tileZ * i32(tileQuads);
    const i32 res = i32(hf.resolution());
    t.heights.resize(usize(t.sampleCount) * t.sampleCount);
    for (u32 z = 0; z < t.sampleCount; ++z) {
        for (u32 x = 0; x < t.sampleCount; ++x) {
            const i32 sx = x0 + i32(x), sz = z0 + i32(z);
            const bool outside = sx < 0 || sz < 0 || sx >= res || sz >= res;
            t.heights[usize(z) * t.sampleCount + x] =
                outside || hf.isHole(u32(sx), u32(sz)) ? kPhysicsHeightHole : hf.heightAtSample(sx, sz);
        }
    }
    const glm::vec2 o = hf.sampleToWorld(glm::vec2(f32(x0), f32(z0)));
    t.offset = {o.x, 0.f, o.y};
    t.scale = {hf.spacing(), 1.f, hf.spacing()};
    return t;
}

std::vector<PhysicsHeightfieldTile> buildPhysicsTiles(const Heightfield& hf, u32 tileQuads) {
    std::vector<PhysicsHeightfieldTile> out;
    const u32 n = (hf.resolution() - 1 + tileQuads - 1) / tileQuads;
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            out.push_back(buildPhysicsTile(hf, i32(x), i32(z), tileQuads));
        }
    }
    return out;
}

std::vector<glm::ivec2> physicsTilesOverlapping(const IRect& dirty, u32 tileQuads, u32 heightfieldResolution) {
    std::vector<glm::ivec2> out;
    if (dirty.empty()) {
        return out;
    }
    const i32 n = i32(tileQuads);
    // Sample s belongs to tile s/n and (when on a shared edge) to tile s/n - 1.
    const i32 x0 = std::max(0, (dirty.x0 - 1) / n), z0 = std::max(0, (dirty.z0 - 1) / n);
    const i32 last = i32((heightfieldResolution - 1 + tileQuads - 1) / tileQuads) - 1;
    const i32 x1 = std::min(last, (dirty.x1 - 1) / n), z1 = std::min(last, (dirty.z1 - 1) / n);
    for (i32 z = z0; z <= z1; ++z) {
        for (i32 x = x0; x <= x1; ++x) {
            out.push_back({x, z});
        }
    }
    return out;
}

} // namespace ox::world
