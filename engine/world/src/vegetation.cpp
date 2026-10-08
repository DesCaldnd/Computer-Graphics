#include <oxwald/world/noise.hpp>
#include <oxwald/world/vegetation.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ox::world {

namespace {

constexpr f32 kTwoPi = 6.28318530718f;

std::vector<glm::vec2> bridson(glm::vec2 size, f32 r, u32 seed, u32 attempts, bool wrap) {
    std::vector<glm::vec2> pts;
    if (r <= 0.f || size.x <= 0.f || size.y <= 0.f) {
        return pts;
    }
    const f32 ideal = r / 1.41421356f;
    const i32 gx = std::max(1, i32(std::ceil(size.x / ideal))), gz = std::max(1, i32(std::ceil(size.y / ideal)));
    const glm::vec2 cell = size / glm::vec2(f32(gx), f32(gz)); // <= r/sqrt(2): at most one point per cell
    const i32 reach = i32(std::ceil(r / std::min(cell.x, cell.y)));
    std::vector<i32> grid(usize(gx) * usize(gz), -1);
    std::vector<u32> active;
    Rng rng(seed, 0x9E3779B97F4A7C15ULL);
    const f32 r2 = r * r;

    auto cellOf = [&](glm::vec2 p) {
        return glm::ivec2(std::clamp(i32(p.x / cell.x), 0, gx - 1), std::clamp(i32(p.y / cell.y), 0, gz - 1));
    };
    auto fits = [&](glm::vec2 p) {
        const glm::ivec2 c = cellOf(p);
        for (i32 dz = -reach; dz <= reach; ++dz) {
            for (i32 dx = -reach; dx <= reach; ++dx) {
                i32 x = c.x + dx, z = c.y + dz;
                if (wrap) {
                    x = ((x % gx) + gx) % gx;
                    z = ((z % gz) + gz) % gz;
                } else if (x < 0 || z < 0 || x >= gx || z >= gz) {
                    continue;
                }
                const i32 idx = grid[usize(z) * usize(gx) + usize(x)];
                if (idx < 0) {
                    continue;
                }
                glm::vec2 d = glm::abs(pts[usize(idx)] - p);
                if (wrap) {
                    d = glm::min(d, size - d);
                }
                if (glm::dot(d, d) < r2) {
                    return false;
                }
            }
        }
        return true;
    };
    auto add = [&](glm::vec2 p) {
        const glm::ivec2 c = cellOf(p);
        grid[usize(c.y) * usize(gx) + usize(c.x)] = i32(pts.size());
        active.push_back(u32(pts.size()));
        pts.push_back(p);
    };

    add({rng.nextFloat() * size.x, rng.nextFloat() * size.y});
    while (!active.empty()) {
        const u32 ai = rng.below(u32(active.size()));
        const glm::vec2 base = pts[active[ai]];
        bool found = false;
        for (u32 k = 0; k < attempts; ++k) {
            const f32 ang = rng.nextFloat() * kTwoPi;
            const f32 rad = r * (1.f + rng.nextFloat()); // annulus [r, 2r)
            glm::vec2 p = base + rad * glm::vec2(std::cos(ang), std::sin(ang));
            if (wrap) {
                p = glm::mod(p, size);
                p = glm::min(p, size - glm::vec2(1e-4f)); // fmod may return size for tiny negatives
            } else if (p.x < 0.f || p.y < 0.f || p.x >= size.x || p.y >= size.y) {
                continue;
            }
            if (fits(p)) {
                add(p);
                found = true;
                break;
            }
        }
        if (!found) {
            active[ai] = active.back();
            active.pop_back();
        }
    }
    return pts;
}

} // namespace

std::vector<glm::vec2> PoissonDisk::generate(glm::vec2 size, f32 minDistance, u32 seed, u32 attempts) {
    return bridson(size, minDistance, seed, attempts, false);
}

std::vector<glm::vec2> PoissonDisk::generateTileable(f32 period, f32 minDistance, u32 seed, u32 attempts) {
    return bridson(glm::vec2(period), minDistance, seed, attempts, true);
}

f32 DensityMap::sample(glm::vec2 w) const {
    if (resolution < 2 || values.size() < usize(resolution) * resolution) {
        return 1.f;
    }
    const f32 r = f32(resolution - 1);
    const glm::vec2 t = glm::clamp((w - origin) / worldSize * r, glm::vec2(0.f), glm::vec2(r));
    const u32 x0 = std::min(u32(t.x), resolution - 2), z0 = std::min(u32(t.y), resolution - 2);
    const f32 fx = t.x - f32(x0), fz = t.y - f32(z0);
    auto v = [&](u32 x, u32 z) { return values[usize(z) * resolution + x]; };
    return glm::mix(glm::mix(v(x0, z0), v(x0 + 1, z0), fx), glm::mix(v(x0, z0 + 1), v(x0 + 1, z0 + 1), fx), fz);
}

bool ExclusionZone::contains(glm::vec2 p) const {
    const glm::vec2 d = p - center;
    if (shape == Shape::Circle) {
        return glm::dot(d, d) <= halfExtents.x * halfExtents.x;
    }
    return std::abs(d.x) <= halfExtents.x && std::abs(d.y) <= halfExtents.y;
}

u32 packRgba8(glm::vec4 c) {
    const glm::vec4 v = glm::round(glm::clamp(c, 0.f, 1.f) * 255.f);
    return u32(v.x) | (u32(v.y) << 8) | (u32(v.z) << 16) | (u32(v.w) << 24);
}

glm::vec4 unpackRgba8(u32 c) {
    return glm::vec4(f32(c & 0xFF), f32((c >> 8) & 0xFF), f32((c >> 16) & 0xFF), f32(c >> 24)) / 255.f;
}

glm::mat4 instanceMatrix(const VegetationInstance& inst) {
    glm::mat4 m = glm::mat4_cast(inst.rotation);
    m[0] *= inst.scale;
    m[1] *= inst.scale;
    m[2] *= inst.scale;
    m[3] = glm::vec4(inst.position, 1.f);
    return m;
}

VegetationInstanceGpu toGpu(const VegetationInstance& inst, const VegetationLayer& layer) {
    const glm::mat4 m = instanceMatrix(inst);
    VegetationInstanceGpu g{};
    for (int r = 0; r < 3; ++r) {
        g.transform[r] = glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]);
    }
    g.tint = inst.tint;
    g.random = inst.random;
    u32 flags = 0;
    if (layer.kind == VegetationKind::Tree) {
        flags |= kVegFlagTree | kVegFlagCastsShadow;
    }
    g.prototypeLayerFlags = u32(inst.prototype) | (u32(inst.layer & 0xFF) << 16) | flags;
    g.boundingRadius = layer.boundingRadius * inst.scale;
    return g;
}

VegetationScatterer::VegetationScatterer(std::vector<VegetationLayer> layers, f32 patternPeriod) : m_layers(std::move(layers)) {
    m_patterns.resize(m_layers.size());
    for (usize i = 0; i < m_layers.size(); ++i) {
        const VegetationLayer& l = m_layers[i];
        Pattern& p = m_patterns[i];
        p.period = std::max(patternPeriod, 8.f * l.minDistance);
        p.points = PoissonDisk::generateTileable(p.period, l.minDistance, hashCombine(l.seed, u32(i)));
    }
}

f32 VegetationScatterer::acceptance(u32 li, glm::vec2 w, const ScatterContext& ctx) const {
    const VegetationLayer& l = m_layers[li];
    const Heightfield& hf = *ctx.heightfield;
    if (!hf.containsWorld(w)) {
        return 0.f;
    }
    if (hf.hasHoles()) {
        const glm::vec2 s = glm::round(hf.worldToSample(w));
        if (hf.isHole(u32(s.x), u32(s.y))) {
            return 0.f;
        }
    }
    const f32 h = hf.sampleHeight(w);
    if (h < l.minHeight || h > l.maxHeight) {
        return 0.f;
    }
    const f32 slope = glm::degrees(hf.sampleSlope(w));
    if (slope < l.minSlopeDeg || slope > l.maxSlopeDeg) {
        return 0.f;
    }
    for (const ExclusionZone& z : ctx.exclusions) {
        if ((z.layerMask & (1u << (li & 31))) && z.contains(w)) {
            return 0.f;
        }
    }
    f32 a = glm::clamp(l.density, 0.f, 1.f);
    if (l.splatLayer >= 0 && ctx.splat) {
        const f32 sw = ctx.splat->sampleLayer(w, u32(l.splatLayer));
        if (sw < l.minSplatWeight) {
            return 0.f;
        }
        a *= sw;
    }
    if (l.densityMapIndex >= 0 && usize(l.densityMapIndex) < ctx.densityMaps.size()) {
        a *= glm::clamp(ctx.densityMaps[usize(l.densityMapIndex)].sample(w), 0.f, 1.f);
    }
    if (ctx.customDensity) {
        a *= glm::clamp(ctx.customDensity(w, li), 0.f, 1.f);
    }
    return a;
}

VegetationChunk VegetationScatterer::scatter(glm::vec2 origin, f32 size, const ScatterContext& ctx, f32 cellSize) const {
    VegetationChunk chunk;
    chunk.origin = origin;
    chunk.size = size;
    if (!ctx.heightfield) {
        return chunk;
    }
    const Heightfield& hf = *ctx.heightfield;
    const glm::vec2 hi = origin + glm::vec2(size);
    for (u32 li = 0; li < m_layers.size(); ++li) {
        const VegetationLayer& l = m_layers[li];
        const Pattern& pat = m_patterns[li];
        const f32 P = pat.period;
        const u32 ls = hashCombine(l.seed, li);
        const glm::vec2 off{hashToUnit(hash32(ls)) * P, hashToUnit(hash32(ls ^ 0x68E31DA4u)) * P};
        const i32 tx0 = i32(std::floor((origin.x - off.x) / P)), tx1 = i32(std::floor((hi.x - off.x) / P));
        const i32 tz0 = i32(std::floor((origin.y - off.y) / P)), tz1 = i32(std::floor((hi.y - off.y) / P));
        for (i32 tz = tz0; tz <= tz1; ++tz) {
            for (i32 tx = tx0; tx <= tx1; ++tx) {
                const glm::vec2 base = off + glm::vec2(f32(tx), f32(tz)) * P;
                for (u32 pi = 0; pi < pat.points.size(); ++pi) {
                    const glm::vec2 w = base + pat.points[pi];
                    if (w.x < origin.x || w.y < origin.y || w.x >= hi.x || w.y >= hi.y) {
                        continue;
                    }
                    Rng rng(hashCoord(tx, tz, hashCombine(ls, pi)), 0xB5297A4Du);
                    const f32 accept = acceptance(li, w, ctx);
                    if (accept <= 0.f || rng.nextFloat() >= accept) {
                        continue;
                    }
                    VegetationInstance inst;
                    inst.layer = u16(li);
                    inst.prototype = l.prototype;
                    inst.scale = rng.range(l.minScale, l.maxScale);
                    const f32 yaw = l.randomYaw ? rng.nextFloat() * kTwoPi : 0.f;
                    const glm::vec3 n = hf.sampleNormal(w);
                    const glm::vec3 up = glm::normalize(glm::mix(glm::vec3(0, 1, 0), n, glm::clamp(l.alignToNormal, 0.f, 1.f)));
                    inst.rotation = glm::rotation(glm::vec3(0, 1, 0), up) * glm::angleAxis(yaw, glm::vec3(0, 1, 0));
                    inst.position = {w.x, hf.sampleHeight(w) - l.sinkDepth * inst.scale, w.y};
                    inst.tint = packRgba8(glm::mix(l.tintA, l.tintB, rng.nextFloat()));
                    inst.random = rng.nextFloat();
                    chunk.instances.push_back(inst);
                }
            }
        }
    }
    chunk.buildCells(m_layers, cellSize);
    for (u32 i = 0; i < chunk.instances.size(); ++i) {
        const VegetationInstance& inst = chunk.instances[i];
        const VegetationLayer& l = m_layers[inst.layer];
        if (!l.collider) {
            continue;
        }
        VegetationCollider c;
        c.shape = l.colliderShape;
        c.radius = l.colliderRadius * inst.scale;
        c.halfHeight = l.colliderHalfHeight * inst.scale;
        // Trunks stay upright: only the yaw of the instance is kept.
        const glm::vec3 fwd = inst.rotation * glm::vec3(0, 0, 1);
        c.rotation = glm::angleAxis(std::atan2(fwd.x, fwd.z), glm::vec3(0, 1, 0));
        const f32 base = c.shape == VegetationColliderShape::Capsule ? c.halfHeight + c.radius : c.halfHeight;
        c.position = inst.position + glm::vec3(0.f, base, 0.f);
        c.instanceIndex = i;
        c.layer = inst.layer;
        chunk.colliders.push_back(c);
    }
    return chunk;
}

void VegetationChunk::buildCells(std::span<const VegetationLayer> layers, f32 cellSize) {
    cells.clear();
    if (instances.empty()) {
        return;
    }
    cellSize = std::max(cellSize, 1.f);
    const i32 n = std::max(1, i32(std::ceil(size / cellSize)));
    auto key = [&](const VegetationInstance& v) {
        const i32 cx = std::clamp(i32((v.position.x - origin.x) / cellSize), 0, n - 1);
        const i32 cz = std::clamp(i32((v.position.z - origin.y) / cellSize), 0, n - 1);
        return (u64(u32(cz * n + cx)) << 16) | v.layer;
    };
    std::vector<u32> order(instances.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return key(instances[a]) < key(instances[b]); });
    std::vector<VegetationInstance> sorted(instances.size());
    std::vector<u32> remap(instances.size());
    for (u32 i = 0; i < order.size(); ++i) {
        sorted[i] = instances[order[i]];
        remap[order[i]] = i;
    }
    instances = std::move(sorted);
    for (VegetationCollider& c : colliders) {
        c.instanceIndex = remap[c.instanceIndex];
    }
    u64 current = ~0ull;
    for (u32 i = 0; i < instances.size(); ++i) {
        const VegetationInstance& v = instances[i];
        const u64 k = key(v);
        const f32 r = (v.layer < layers.size() ? layers[v.layer].boundingRadius : 1.f) * v.scale;
        if (k != current) {
            current = k;
            VegetationCell c;
            c.first = i;
            c.layer = v.layer;
            c.bounds = {v.position - glm::vec3(r), v.position + glm::vec3(r)};
            cells.push_back(c);
        }
        VegetationCell& c = cells.back();
        ++c.count;
        c.bounds.min = glm::min(c.bounds.min, v.position - glm::vec3(r));
        c.bounds.max = glm::max(c.bounds.max, v.position + glm::vec3(r));
    }
}

VegetationLodResult selectVegetationLod(const VegetationLodSettings& s, f32 d) {
    VegetationLodResult r;
    if (d > s.cullDistance) {
        r.lod = VegetationLodResult::kCulled;
        r.fade = 0.f;
        return r;
    }
    const bool impostors = s.impostorDistance > 0.f && s.impostorDistance < s.cullDistance;
    const f32 ends[3] = {s.lodDistances[0], s.lodDistances[1], impostors ? s.impostorDistance : s.cullDistance};
    f32 end = s.cullDistance;
    r.lod = impostors ? VegetationLodResult::kImpostor : 2;
    for (u8 i = 0; i < 3; ++i) {
        if (d <= ends[i]) {
            r.lod = i;
            end = ends[i];
            break;
        }
    }
    r.fade = s.fadeRange > 0.f ? glm::clamp((end - d) / s.fadeRange, 0.f, 1.f) : 1.f;
    return r;
}

void cullVegetationCells(const VegetationChunk& chunk, std::span<const VegetationLayer> layers, const Frustum& frustum,
                         glm::vec3 cameraPos, std::vector<VisibleVegetationCell>& out, f32 distanceScale) {
    for (u32 i = 0; i < chunk.cells.size(); ++i) {
        const VegetationCell& c = chunk.cells[i];
        const f32 d = std::sqrt(distanceSq(c.bounds, cameraPos)) / std::max(distanceScale, 1e-3f);
        const VegetationLodSettings& lod = c.layer < layers.size() ? layers[c.layer].lod : VegetationLodSettings{};
        const VegetationLodResult r = selectVegetationLod(lod, d);
        if (r.lod == VegetationLodResult::kCulled || !frustum.intersects(c.bounds)) {
            continue;
        }
        out.push_back({i, d, r});
    }
}

void debugDrawVegetationCells(const VegetationChunk& chunk, const DebugLineFn& line) {
    if (!line) {
        return;
    }
    for (const VegetationCell& c : chunk.cells) {
        const glm::vec4 col = hashToUnit(hash32(c.layer)) > 0.5f ? glm::vec4(0.2f, 0.9f, 0.2f, 1) : glm::vec4(0.1f, 0.5f, 0.1f, 1);
        const glm::vec3 a = c.bounds.min, b = c.bounds.max;
        const glm::vec3 p[8] = {{a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, a.y, b.z}, {a.x, a.y, b.z},
                                {a.x, b.y, a.z}, {b.x, b.y, a.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}};
        for (int i = 0; i < 4; ++i) {
            line(p[i], p[(i + 1) % 4], col);
            line(p[i + 4], p[(i + 1) % 4 + 4], col);
            line(p[i], p[i + 4], col);
        }
    }
}

} // namespace ox::world
