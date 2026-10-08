#include <oxwald/render/shadows.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace ox::render {

std::vector<f32> cascadeSplits(f32 nearPlane, f32 shadowDistance, u32 count, f32 lambda) {
    std::vector<f32> out(count);
    const f32 n = std::max(nearPlane, 1e-3f);
    const f32 f = std::max(shadowDistance, n + 1e-3f);
    for (u32 i = 1; i <= count; ++i) {
        const f32 p = f32(i) / f32(count);
        const f32 logSplit = n * std::pow(f / n, p);
        const f32 uniSplit = n + (f - n) * p;
        out[i - 1] = glm::mix(uniSplit, logSplit, lambda);
    }
    out.back() = f;
    return out;
}

namespace {

glm::vec3 stableUp(const glm::vec3& dir) {
    return std::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
}

} // namespace

std::vector<Cascade> computeCascades(const CascadeInput& in) {
    std::vector<Cascade> out;
    if (in.cascadeCount == 0) return out;
    const std::vector<f32> splits = cascadeSplits(in.nearPlane, in.shadowDistance, in.cascadeCount, in.lambda);
    const glm::vec3 lightDir = glm::normalize(in.lightDirection);
    const glm::mat4 lightRot = glm::lookAtRH(glm::vec3(0.0f), lightDir, stableUp(lightDir));
    const glm::vec3 camPos = glm::vec3(in.cameraWorld[3]);
    const glm::vec3 camFwd = -glm::normalize(glm::vec3(in.cameraWorld[2]));
    const f32 tanH = std::tan(in.verticalFov * 0.5f);
    const f32 tanW = tanH * in.aspect;
    const f32 k = in.orthographic ? 0.0f : tanW * tanW + tanH * tanH;
    const f32 res = f32(std::max(in.resolution, 16u));

    f32 prev = in.nearPlane;
    for (u32 i = 0; i < in.cascadeCount; ++i) {
        Cascade c;
        c.splitNear = prev;
        c.splitFar = splits[i];
        prev = c.splitFar;
        // Minimal bounding sphere of the frustum slice [n, f]: its size only depends on the split distances, so it
        // does not change when the camera rotates (no resolution swimming).
        const f32 n = c.splitNear, f = c.splitFar;
        f32 centerDist, radius;
        if (in.orthographic) {
            const f32 hh = in.orthographicHeight * 0.5f, hw = hh * in.aspect;
            centerDist = (n + f) * 0.5f;
            radius = std::sqrt(hh * hh + hw * hw + (f - n) * (f - n) * 0.25f);
        } else {
            centerDist = std::min((f + n) * (1.0f + k) * 0.5f, f);
            const f32 dn = centerDist - n, df = f - centerDist;
            radius = std::sqrt(std::max(dn * dn + k * n * n, df * df + k * f * f));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;
        const glm::vec3 center = camPos + camFwd * centerDist;

        // Snap the light-space centre to whole texels (xy) and the depth origin to a coarse step.
        const f32 texel = 2.0f * radius / res;
        glm::vec3 lc = glm::vec3(lightRot * glm::vec4(center, 1.0f));
        lc.x = std::floor(lc.x / texel) * texel;
        lc.y = std::floor(lc.y / texel) * texel;
        const f32 zStep = texel * 4.0f;
        lc.z = std::floor(lc.z / zStep) * zStep;

        const f32 nearDist = -(lc.z + radius + in.casterExtension);
        const f32 farDist = -(lc.z - radius);
        c.view = lightRot;
        c.proj = orthoReversedZ(lc.x - radius, lc.x + radius, lc.y - radius, lc.y + radius, nearDist, farDist);
        c.viewProj = c.proj * c.view;
        c.radius = radius;
        c.texelWorld = texel;
        c.depthRange = farDist - nearDist;
        out.push_back(c);
    }
    return out;
}

glm::vec3 cubeFaceForward(u32 face) {
    static const glm::vec3 kF[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    return kF[face % 6];
}

glm::vec3 cubeFaceUp(u32 face) {
    static const glm::vec3 kU[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    return kU[face % 6];
}

f32 cubeFaceFov(u32 resolution, f32 guardTexels) {
    const f32 r = f32(std::max(resolution, 8u));
    const f32 g = std::clamp(guardTexels, 0.0f, r * 0.25f);
    return 2.0f * std::atan(r / (r - 2.0f * g));
}

glm::mat4 cubeFaceViewProj(glm::vec3 position, u32 face, f32 fov, f32 nearPlane, f32 farPlane) {
    const glm::mat4 view = glm::lookAtRH(position, position + cubeFaceForward(face), cubeFaceUp(face));
    return perspectiveReversedZ(fov, 1.0f, nearPlane, farPlane) * view;
}

glm::mat4 spotViewProj(glm::vec3 position, glm::vec3 direction, f32 outerConeDegrees, f32 nearPlane, f32 farPlane) {
    const glm::vec3 d = glm::normalize(direction);
    const f32 fov = std::min(glm::radians(outerConeDegrees) * 2.0f + glm::radians(2.0f), glm::radians(170.0f));
    const glm::mat4 view = glm::lookAtRH(position, position + d, stableUp(d));
    return perspectiveReversedZ(fov, 1.0f, nearPlane, farPlane) * view;
}

// --- atlas ---

ShadowAtlasAllocator::ShadowAtlasAllocator(u32 atlasSize, u32 minTileSize) { reset(atlasSize, minTileSize); }

void ShadowAtlasAllocator::reset(u32 atlasSize, u32 minTileSize) {
    m_size = std::bit_ceil(std::max(atlasSize, 16u));
    m_minTile = std::bit_ceil(std::max(minTileSize, 8u));
    clear();
}

void ShadowAtlasAllocator::clear() {
    m_nodes.assign(1, Node{});
    m_used = 0;
}

std::optional<ShadowAtlasAllocator::Tile> ShadowAtlasAllocator::allocate(u32 size) {
    const u32 want = std::clamp(std::bit_ceil(std::max(size, 1u)), m_minTile, m_size);
    Tile t;
    if (!allocateIn(0, 0, 0, m_size, want, t)) return std::nullopt;
    m_used += u64(want) * want;
    return t;
}

bool ShadowAtlasAllocator::allocateIn(u32 node, u32 nx, u32 ny, u32 nsize, u32 want, Tile& out) {
    Node& nd = m_nodes[node];
    if (nd.state == 1) return false;
    if (nd.state == 0) {
        if (nsize == want) {
            m_nodes[node].state = 1;
            out = {nx, ny, nsize};
            return true;
        }
        if (nsize < want) return false;
        const u32 first = u32(m_nodes.size());
        m_nodes[node].state = 2;
        m_nodes[node].children = first;
        m_nodes.resize(m_nodes.size() + 4);
    }
    const u32 half = nsize / 2;
    if (half < want) return false;
    const u32 children = m_nodes[node].children;
    for (u32 i = 0; i < 4; ++i) {
        if (allocateIn(children + i, nx + (i & 1) * half, ny + (i >> 1) * half, half, want, out)) return true;
    }
    return false;
}

void ShadowAtlasAllocator::free(const Tile& tile) {
    if (freeIn(0, 0, 0, m_size, tile)) m_used -= u64(tile.size) * tile.size;
}

bool ShadowAtlasAllocator::freeIn(u32 node, u32 nx, u32 ny, u32 nsize, const Tile& t) {
    Node& nd = m_nodes[node];
    if (nsize == t.size) {
        if (nx == t.x && ny == t.y && nd.state == 1) {
            nd.state = 0;
            return true;
        }
        return false;
    }
    if (nd.state != 2) return false;
    const u32 half = nsize / 2;
    const u32 i = (t.x >= nx + half ? 1u : 0u) + (t.y >= ny + half ? 2u : 0u);
    const u32 children = nd.children;
    if (!freeIn(children + i, nx + (i & 1) * half, ny + (i >> 1) * half, half, t)) return false;
    // Merge when all four children are free leaves (the child nodes stay allocated in the pool; cleared on clear()).
    bool allFree = true;
    for (u32 c = 0; c < 4; ++c) allFree &= m_nodes[children + c].state == 0;
    if (allFree) m_nodes[node].state = 0;
    return true;
}

f32 shadowImportance(const ShadowRequest& r, const glm::vec3& cameraPos, f32 cameraFovY, f32 viewportHeight) {
    const f32 d = glm::length(r.position - cameraPos);
    const f32 tanHalf = std::tan(std::max(cameraFovY, 0.01f) * 0.5f);
    f32 pixels;
    if (d <= r.range) {
        pixels = viewportHeight;
    } else {
        const f32 projected = r.range / std::sqrt(std::max(d * d - r.range * r.range, 1e-6f));
        pixels = std::min(viewportHeight, 0.5f * viewportHeight * projected / tanHalf);
    }
    return pixels * std::max(r.priority, 0.0f);
}

std::vector<ShadowAllocation> allocateShadows(std::span<const ShadowRequest> requests, const glm::vec3& cameraPos,
                                              f32 cameraFovY, f32 viewportHeight, const ShadowBudget& budget,
                                              ShadowAtlasAllocator& atlas) {
    struct Ranked {
        u32 request;
        f32 importance;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(requests.size());
    for (u32 i = 0; i < requests.size(); ++i) {
        ranked.push_back({i, shadowImportance(requests[i], cameraPos, cameraFovY, viewportHeight)});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked& a, const Ranked& b) { return a.importance > b.importance; });

    atlas.reset(budget.atlasSize, std::min(budget.minResolution, budget.maxResolution));
    std::vector<ShadowAllocation> out;
    std::vector<ShadowAllocation> spots;
    u32 points = 0;
    for (const Ranked& rk : ranked) {
        if (out.size() + spots.size() >= budget.maxShadowedLights) break;
        const ShadowRequest& r = requests[rk.request];
        if (rk.importance <= 0.0f) continue;
        ShadowAllocation a;
        a.lightIndex = r.lightIndex;
        a.point = r.point;
        a.importance = rk.importance;
        if (r.point) {
            if (points >= budget.maxPointLights) continue;
            a.cubeSlot = points++;
            a.resolution = budget.pointResolution;
            out.push_back(a);
            continue;
        }
        u32 want = r.resolutionHint ? r.resolutionHint : u32(rk.importance);
        a.resolution = std::clamp(std::bit_ceil(std::max(want, 1u)), budget.minResolution, budget.maxResolution);
        spots.push_back(a);
    }
    // Fit the spot tiles into the atlas area: repeatedly halve the largest tile, least important first among equals.
    const u64 area = u64(atlas.atlasSize()) * atlas.atlasSize();
    auto total = [&] {
        u64 t = 0;
        for (const auto& s : spots) t += u64(s.resolution) * s.resolution;
        return t;
    };
    while (total() > area) {
        ShadowAllocation* victim = nullptr;
        for (auto& s : spots) {
            if (s.resolution <= budget.minResolution) continue;
            if (!victim || s.resolution > victim->resolution ||
                (s.resolution == victim->resolution && s.importance < victim->importance)) {
                victim = &s;
            }
        }
        if (!victim) {
            spots.pop_back(); // everything at the minimum: drop the least important light
            if (spots.empty()) break;
            continue;
        }
        victim->resolution /= 2;
    }
    // Power-of-two tiles whose total area fits always pack in a quadtree when placed largest first.
    std::vector<usize> order(spots.size());
    for (usize i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return spots[a].resolution > spots[b].resolution; });
    std::vector<bool> placed(spots.size(), false);
    for (usize i : order) {
        if (auto tile = atlas.allocate(spots[i].resolution)) {
            spots[i].tile = *tile;
            spots[i].resolution = tile->size;
            placed[i] = true;
        }
    }
    for (usize i = 0; i < spots.size(); ++i) {
        if (placed[i]) out.push_back(spots[i]);
    }
    std::stable_sort(out.begin(), out.end(), [](const ShadowAllocation& a, const ShadowAllocation& b) {
        return a.importance > b.importance;
    });
    return out;
}

} // namespace ox::render
