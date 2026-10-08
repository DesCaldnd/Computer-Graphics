#include <oxwald/world/noise.hpp>
#include <oxwald/world/splat_map.hpp>
#include <oxwald/world/terrain_brush.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ox::world {

f32 brushWeight(const BrushSettings& b, f32 t) {
    if (t >= 1.f) {
        return 0.f;
    }
    const f32 h = glm::clamp(b.hardness, 0.f, 0.999f);
    if (t <= h || b.falloff == BrushFalloff::Constant) {
        return 1.f;
    }
    const f32 u = (t - h) / (1.f - h); // 0 at the hard core edge, 1 at the radius
    switch (b.falloff) {
    case BrushFalloff::Linear: return 1.f - u;
    case BrushFalloff::Smooth: return 1.f - u * u * (3.f - 2.f * u);
    case BrushFalloff::Spherical: return std::sqrt(std::max(0.f, 1.f - u * u));
    default: return 1.f;
    }
}

namespace {

struct DirtyTracker {
    IRect r{std::numeric_limits<i32>::max(), std::numeric_limits<i32>::max(), std::numeric_limits<i32>::min(),
            std::numeric_limits<i32>::min()};
    bool any = false;
    void mark(i32 x, i32 z) {
        any = true;
        r.x0 = std::min(r.x0, x);
        r.z0 = std::min(r.z0, z);
        r.x1 = std::max(r.x1, x + 1);
        r.z1 = std::max(r.z1, z + 1);
    }
    IRect result() const { return any ? r : IRect{}; }
};

IRect coveredRect(glm::vec2 s0, glm::vec2 s1, u32 res) {
    IRect r{i32(std::floor(s0.x)), i32(std::floor(s0.y)), i32(std::ceil(s1.x)) + 1, i32(std::ceil(s1.y)) + 1};
    return r.intersected({0, 0, i32(res), i32(res)});
}

} // namespace

IRect applyBrush(Heightfield& hf, glm::vec2 center, const BrushSettings& b, f32 dt) {
    if (b.radius <= 0.f) {
        return {};
    }
    const IRect area = coveredRect(hf.worldToSample(center - glm::vec2(b.radius)),
                                   hf.worldToSample(center + glm::vec2(b.radius)), hf.resolution());
    if (area.empty()) {
        return {};
    }
    // Smooth reads neighbours, so snapshot the area (+1 border) before writing.
    std::vector<f32> snapshot;
    IRect snap{};
    if (b.op == BrushOp::Smooth) {
        snap = IRect{area.x0 - 1, area.z0 - 1, area.x1 + 1, area.z1 + 1}.intersected(hf.fullRect());
        snapshot = hf.worldHeights(snap);
    }
    auto snapAt = [&](i32 x, i32 z) {
        x = std::clamp(x, snap.x0, snap.x1 - 1);
        z = std::clamp(z, snap.z0, snap.z1 - 1);
        return snapshot[usize(z - snap.z0) * usize(snap.width()) + usize(x - snap.x0)];
    };
    const Noise2D noise(b.noiseSeed);
    DirtyTracker dirty;
    for (i32 z = area.z0; z < area.z1; ++z) {
        for (i32 x = area.x0; x < area.x1; ++x) {
            const glm::vec2 w = hf.sampleToWorld(glm::vec2(f32(x), f32(z)));
            const f32 wgt = brushWeight(b, glm::length(w - center) / b.radius);
            if (wgt <= 0.f) {
                continue;
            }
            if (b.op == BrushOp::SetHole || b.op == BrushOp::ClearHole) {
                const bool hole = b.op == BrushOp::SetHole;
                if (hf.isHole(u32(x), u32(z)) != hole) {
                    hf.setHole(u32(x), u32(z), hole);
                    dirty.mark(x, z);
                }
                continue;
            }
            const f32 h = hf.heightAtSample(x, z);
            f32 nh = h;
            const f32 k = glm::clamp(b.strength * dt * wgt, 0.f, 1.f);
            switch (b.op) {
            case BrushOp::Raise: nh = h + b.strength * dt * wgt; break;
            case BrushOp::Lower: nh = h - b.strength * dt * wgt; break;
            case BrushOp::Flatten: nh = glm::mix(h, b.targetHeight, k); break;
            case BrushOp::Smooth: {
                f32 avg = 0.f;
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        avg += snapAt(x + dx, z + dz);
                    }
                }
                nh = glm::mix(h, avg / 9.f, k);
                break;
            }
            case BrushOp::Noise: nh = h + b.strength * dt * wgt * noise.simplex(w * b.noiseFrequency); break;
            default: break;
            }
            if (nh != h) {
                hf.setHeightAtSample(u32(x), u32(z), nh);
                dirty.mark(x, z);
            }
        }
    }
    return dirty.result();
}

IRect paintSplat(SplatMap& splat, glm::vec2 center, u32 layer, const BrushSettings& b, f32 dt) {
    if (b.radius <= 0.f || layer >= splat.layerCount()) {
        return {};
    }
    const IRect area = coveredRect(splat.worldToTexel(center - glm::vec2(b.radius)),
                                   splat.worldToTexel(center + glm::vec2(b.radius)), splat.resolution());
    DirtyTracker dirty;
    for (i32 z = area.z0; z < area.z1; ++z) {
        for (i32 x = area.x0; x < area.x1; ++x) {
            const glm::vec2 w = splat.origin() + glm::vec2(f32(x), f32(z)) * splat.texelSize();
            const f32 a = glm::clamp(b.strength * dt * brushWeight(b, glm::length(w - center) / b.radius), 0.f, 1.f);
            if (a <= 0.f) {
                continue;
            }
            auto ws = splat.weights(u32(x), u32(z));
            for (f32& v : ws) {
                v *= 1.f - a;
            }
            ws[layer] += a;
            splat.setWeights(u32(x), u32(z), ws);
            dirty.mark(x, z);
        }
    }
    return dirty.result();
}

} // namespace ox::world
