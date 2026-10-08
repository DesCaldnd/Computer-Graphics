// Built-in procedural vegetation (used when a layer's prototype has no VegetationPrototypeDesc): a broadleaf tree
// (bark trunk + alpha-tested leaf cards with spherical normals), a grass clump of curved blades and a bush, each in
// three LODs, plus their textures and materials. Wind masks follow the convention of VegetationPrototypeDesc.
#include "world_geometry.hpp"

#if OX_RENDER_HAS_WORLD

#include <oxwald/render/mesh_primitives.hpp>

#include <cmath>
#include <mutex>

namespace ox::render::worldfx {

namespace {

struct MeshBuilder {
    assets::MeshData mesh;
    struct Part {
        u32 firstIndex = 0;
        u32 materialSlot = 0;
    };
    std::vector<Part> parts;

    void beginPart(u32 slot) { parts.push_back({u32(mesh.indices.size()), slot}); }
    u32 vertex(glm::vec3 p, glm::vec3 n, glm::vec2 uv, glm::vec4 color) {
        mesh.positions.push_back(p);
        assets::VertexAttributes a;
        a.normal = glm::normalize(n);
        a.uv0 = uv;
        a.color = assets::packColor(color);
        mesh.attributes.push_back(a);
        return u32(mesh.positions.size() - 1);
    }
    void tri(u32 a, u32 b, u32 c) { mesh.indices.insert(mesh.indices.end(), {a, b, c}); }
    void quad(u32 a, u32 b, u32 c, u32 d) {
        tri(a, b, c);
        tri(a, c, d);
    }
    assets::MeshData finish(const char* name, std::vector<Uuid> materials) {
        mesh.name = name;
        for (usize i = 0; i < parts.size(); ++i) {
            assets::Submesh sm;
            sm.name = std::string(name) + "." + std::to_string(i);
            sm.materialSlot = parts[i].materialSlot;
            sm.vertexOffset = 0;
            sm.vertexCount = u32(mesh.positions.size());
            assets::MeshLod lod;
            lod.indexOffset = parts[i].firstIndex;
            lod.indexCount = (i + 1 < parts.size() ? parts[i + 1].firstIndex : u32(mesh.indices.size())) - parts[i].firstIndex;
            sm.lods.push_back(lod);
            mesh.submeshes.push_back(sm);
        }
        for (usize i = 0; i < materials.size(); ++i) mesh.materials.push_back({"slot" + std::to_string(i), materials[i]});
        computeTangents(mesh);
        computeBounds(mesh);
        return std::move(mesh);
    }
};

// Tapered trunk (open cylinder), wind masks 0 (sways with the whole tree).
void trunk(MeshBuilder& b, f32 height, f32 r0, f32 r1, u32 segments, u32 rings) {
    const u32 first = u32(b.mesh.positions.size());
    for (u32 y = 0; y <= rings; ++y) {
        const f32 t = f32(y) / f32(rings);
        const f32 r = glm::mix(r0, r1, t);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 a = 6.2831853f * f32(s) / f32(segments);
            const glm::vec3 n(std::cos(a), 0.15f, std::sin(a));
            b.vertex({std::cos(a) * r, t * height, std::sin(a) * r}, n, {f32(s) / f32(segments) * 2.0f, t * height * 0.6f},
                     {0.0f, 0.0f, 0.0f, 1.0f});
        }
    }
    for (u32 y = 0; y < rings; ++y) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 i0 = first + y * (segments + 1) + s;
            const u32 i1 = i0 + segments + 1;
            b.quad(i0, i1, i1 + 1, i0 + 1);
        }
    }
}

// Leaf cards: two crossed quads per cluster, normals pointing away from the canopy centre (soft, volumetric shading).
void leafCards(MeshBuilder& b, u32 count, glm::vec3 canopyCenter, glm::vec3 canopyRadius, f32 cardSize, u32 seed) {
    Random rng(seed);
    for (u32 i = 0; i < count; ++i) {
        // Even-ish distribution inside the ellipsoid (rejection sampling, biased to the shell).
        glm::vec3 d;
        do {
            d = {rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f)};
        } while (glm::dot(d, d) > 1.0f || glm::dot(d, d) < 0.15f);
        const glm::vec3 c = canopyCenter + d * canopyRadius;
        const glm::vec3 outward = glm::normalize(d + glm::vec3(0.0f, 0.25f, 0.0f));
        const f32 yaw = rng.range(0.0f, 6.2831853f);
        const f32 size = cardSize * rng.range(0.75f, 1.2f);
        const f32 branch = glm::clamp(glm::length(glm::vec2(c.x, c.z)) / std::max(canopyRadius.x, 0.1f), 0.2f, 1.0f);
        const f32 phase = rng.nextFloat();
        for (u32 plane = 0; plane < 2; ++plane) {
            const f32 a = yaw + f32(plane) * 1.5707963f;
            const glm::vec3 right(std::cos(a), 0.0f, std::sin(a));
            const glm::vec3 up = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) + outward * 0.35f);
            const glm::vec3 r = right * size * 0.5f, u = up * size * 0.5f;
            const glm::vec4 col(branch, 1.0f, phase, 1.0f);
            const u32 v0 = b.vertex(c - r - u, outward - right * 0.3f - up * 0.3f, {0.0f, 1.0f}, col);
            const u32 v1 = b.vertex(c + r - u, outward + right * 0.3f - up * 0.3f, {1.0f, 1.0f}, col);
            const u32 v2 = b.vertex(c + r + u, outward + right * 0.3f + up * 0.3f, {1.0f, 0.0f}, col);
            const u32 v3 = b.vertex(c - r + u, outward - right * 0.3f + up * 0.3f, {0.0f, 0.0f}, col);
            b.quad(v0, v1, v2, v3);
        }
    }
}

// Curved grass blades (geometry, no alpha): 3 segments, tip at the top, G = 1 at the tip.
void grassBlades(MeshBuilder& b, u32 count, f32 height, f32 width, f32 spread, u32 seed) {
    Random rng(seed);
    for (u32 i = 0; i < count; ++i) {
        // Two statements: the evaluation order of function arguments is unspecified (MSVC goes right to left).
        const f32 baseX = rng.range(-spread, spread);
        const f32 baseZ = rng.range(-spread, spread);
        const glm::vec2 base(baseX, baseZ);
        const f32 yaw = rng.range(0.0f, 6.2831853f);
        const glm::vec3 side(std::cos(yaw), 0.0f, std::sin(yaw));
        const glm::vec3 lean = glm::vec3(-side.z, 0.0f, side.x) * rng.range(0.1f, 0.35f);
        const f32 h = height * rng.range(0.7f, 1.15f);
        const f32 phase = rng.nextFloat();
        const f32 shade = rng.range(0.85f, 1.0f);
        u32 prevL = 0, prevR = 0;
        constexpr u32 kSegs = 3;
        for (u32 s = 0; s <= kSegs; ++s) {
            const f32 t = f32(s) / f32(kSegs);
            const glm::vec3 c = glm::vec3(base.x, 0.0f, base.y) + glm::vec3(0.0f, t * h, 0.0f) + lean * (t * t) * h;
            const f32 w = width * (1.0f - t * 0.85f) * 0.5f;
            const glm::vec3 n = glm::normalize(glm::cross(side, glm::vec3(0.0f, 1.0f, 0.0f) + lean * 2.0f * t) + glm::vec3(0.0f, 0.6f, 0.0f));
            const glm::vec4 col(0.0f, t, phase, shade);
            const u32 l = b.vertex(c - side * w, n, {0.0f, 1.0f - t}, col);
            const u32 r = b.vertex(c + side * w, n, {1.0f, 1.0f - t}, col);
            if (s > 0) b.quad(prevL, prevR, r, l);
            prevL = l;
            prevR = r;
        }
    }
}

std::vector<std::byte> toBytes(const std::vector<u8>& v) {
    std::vector<std::byte> out(v.size());
    std::memcpy(out.data(), v.data(), v.size());
    return out;
}

// RGBA8 texture with a CPU box-filtered mip chain.
assets::TextureData makeTexture(u32 size, assets::TextureFormat format, const std::function<glm::vec4(glm::vec2)>& fn) {
    assets::TextureData t;
    t.format = format;
    t.width = t.height = size;
    std::vector<u8> level(usize(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const glm::vec4 c = glm::clamp(fn({(f32(x) + 0.5f) / f32(size), (f32(y) + 0.5f) / f32(size)}), 0.0f, 1.0f);
            for (u32 k = 0; k < 4; ++k) level[(usize(y) * size + x) * 4 + k] = u8(c[k] * 255.0f + 0.5f);
        }
    }
    u32 s = size;
    while (true) {
        t.mips.push_back({s, s, toBytes(level)});
        if (s == 1) break;
        const u32 n = s / 2;
        std::vector<u8> next(usize(n) * n * 4);
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                for (u32 k = 0; k < 4; ++k) {
                    const u32 sum = level[((2 * y) * s + 2 * x) * 4 + k] + level[((2 * y) * s + 2 * x + 1) * 4 + k] +
                                    level[((2 * y + 1) * s + 2 * x) * 4 + k] + level[((2 * y + 1) * s + 2 * x + 1) * 4 + k];
                    next[(usize(y) * n + x) * 4 + k] = u8((sum + 2) / 4);
                }
            }
        }
        level = std::move(next);
        s = n;
    }
    t.mipCount = u32(t.mips.size());
    return t;
}

f32 hash2(glm::vec2 p) {
    const f32 h = std::sin(glm::dot(p, glm::vec2(127.1f, 311.7f))) * 43758.5453f;
    return h - std::floor(h);
}

} // namespace

const BuiltinVegetation& registerBuiltinVegetation(GpuResourceCache& cache) {
    static BuiltinVegetation b;
    static std::mutex m;
    std::lock_guard lock(m);
    const Uuid leafTex = Uuid::fromName("ox.render.world.leafTexture");
    const Uuid barkTex = Uuid::fromName("ox.render.world.barkTexture");
    const Uuid leafMat = Uuid::fromName("ox.render.world.leafMaterial");
    const Uuid barkMat = Uuid::fromName("ox.render.world.barkMaterial");
    const Uuid grassMat = Uuid::fromName("ox.render.world.grassMaterial");
    const Uuid bushMat = Uuid::fromName("ox.render.world.bushMaterial");
    for (u32 l = 0; l < 3; ++l) {
        b.treeLods[l] = Uuid::fromName("ox.render.world.tree.lod" + std::to_string(l));
        b.grassLods[l] = Uuid::fromName("ox.render.world.grass.lod" + std::to_string(l));
        b.bushLods[l] = Uuid::fromName("ox.render.world.bush.lod" + std::to_string(l));
    }
    if (cache.state(b.treeLods[0]) == ResourceState::Ready) return b;

    // Leaf atlas: a cluster of ~30 small leaves on a transparent background, slightly varied greens.
    struct Leaf {
        glm::vec2 c;
        f32 angle, size, shade;
    };
    static const std::vector<Leaf> leaves = [] {
        std::vector<Leaf> l;
        Random rng(5);
        for (u32 i = 0; i < 34; ++i) {
            glm::vec2 c;
            do {
                c = {rng.range(0.1f, 0.9f), rng.range(0.1f, 0.9f)};
            } while (glm::length(c - glm::vec2(0.5f)) > 0.42f);
            l.push_back({c, rng.range(0.0f, 6.2831853f), rng.range(0.07f, 0.1f), rng.nextFloat()});
        }
        return l;
    }();
    cache.addTexture(leafTex, makeTexture(256, assets::TextureFormat::RGBA8Srgb, [](glm::vec2 uv) {
        f32 alpha = 0.0f;
        glm::vec3 col(0.0f);
        for (const Leaf& lf : leaves) {
            glm::vec2 d = uv - lf.c;
            d = glm::vec2(d.x * std::cos(lf.angle) - d.y * std::sin(lf.angle), d.x * std::sin(lf.angle) + d.y * std::cos(lf.angle));
            // Pointed leaf: ellipse narrowing towards the tip.
            const f32 w = lf.size * 0.45f * (1.0f - 0.6f * glm::clamp(d.x / lf.size, 0.0f, 1.0f));
            const f32 e = (d.x * d.x) / (lf.size * lf.size) + (d.y * d.y) / (w * w);
            if (e < 1.0f) {
                alpha = 1.0f;
                const f32 vein = std::abs(d.y) < 0.004f ? 0.8f : 1.0f;
                col = glm::mix(glm::vec3(0.07f, 0.2f, 0.035f), glm::vec3(0.2f, 0.34f, 0.07f), lf.shade) * vein *
                      (0.8f + 0.2f * (1.0f - e));
            }
        }
        return glm::vec4(glm::pow(col, glm::vec3(1.0f / 2.2f)), alpha);
    }));
    cache.addTexture(barkTex, makeTexture(128, assets::TextureFormat::RGBA8Srgb, [](glm::vec2 uv) {
        const f32 stripes = 0.5f + 0.5f * std::sin(uv.x * 6.2831853f * 9.0f + std::sin(uv.y * 23.0f) * 1.5f);
        const f32 n = hash2(glm::floor(uv * glm::vec2(32.0f, 64.0f)));
        const glm::vec3 c = glm::mix(glm::vec3(0.12f, 0.08f, 0.05f), glm::vec3(0.30f, 0.22f, 0.15f), stripes * 0.7f + n * 0.3f);
        return glm::vec4(glm::pow(c, glm::vec3(1.0f / 2.2f)), 1.0f);
    }));
    {
        assets::MaterialAsset leaf;
        leaf.albedoTexture = leafTex;
        leaf.blendMode = assets::BlendMode::AlphaTest;
        leaf.alphaCutoff = 0.5f;
        leaf.doubleSided = true;
        leaf.roughness = 0.65f;
        cache.addMaterial(leafMat, leaf);
        assets::MaterialAsset bark;
        bark.albedoTexture = barkTex;
        bark.roughness = 0.9f;
        cache.addMaterial(barkMat, bark);
        assets::MaterialAsset grass;
        grass.baseColor = {0.16f, 0.32f, 0.07f, 1.0f};
        grass.doubleSided = true;
        grass.roughness = 0.7f;
        grass.subsurface = 0.8f;
        cache.addMaterial(grassMat, grass);
        assets::MaterialAsset bush = leaf;
        bush.baseColor = {0.8f, 0.95f, 0.7f, 1.0f};
        cache.addMaterial(bushMat, bush);
    }
    const u32 treeCards[3] = {70, 34, 16};
    const u32 trunkSegs[3] = {8, 6, 4};
    for (u32 l = 0; l < 3; ++l) {
        MeshBuilder mb;
        mb.beginPart(0);
        trunk(mb, 6.0f, 0.22f, 0.07f, trunkSegs[l], l == 0 ? 6 : 3);
        mb.beginPart(1);
        leafCards(mb, treeCards[l], {0.0f, 5.0f, 0.0f}, {2.6f, 2.8f, 2.6f}, 1.7f * (l == 0 ? 1.0f : l == 1 ? 1.35f : 1.8f), 11);
        cache.addMesh(b.treeLods[l], mb.finish("ox.tree", {barkMat, leafMat}));
    }
    const u32 blades[3] = {14, 8, 4};
    for (u32 l = 0; l < 3; ++l) {
        MeshBuilder mb;
        mb.beginPart(0);
        grassBlades(mb, blades[l], 0.5f, l == 0 ? 0.07f : l == 1 ? 0.09f : 0.13f, 0.18f, 7);
        cache.addMesh(b.grassLods[l], mb.finish("ox.grass", {grassMat}));
    }
    const u32 bushCards[3] = {22, 12, 6};
    for (u32 l = 0; l < 3; ++l) {
        MeshBuilder mb;
        mb.beginPart(0);
        leafCards(mb, bushCards[l], {0.0f, 0.6f, 0.0f}, {0.8f, 0.55f, 0.8f}, 0.8f * (l == 0 ? 1.0f : 1.4f), 23);
        cache.addMesh(b.bushLods[l], mb.finish("ox.bush", {bushMat}));
    }
    return b;
}

} // namespace ox::render::worldfx

#endif // OX_RENDER_HAS_WORLD
