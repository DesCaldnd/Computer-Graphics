#include <oxwald/world/terrain_brush.hpp>
#include <oxwald/world/terrain_gen.hpp>
#include <oxwald/world/terrain_lod.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <bit>
#include <cmath>
#include <map>

#if defined(OX_WORLD_TEST_PHYSICS)
#include <oxwald/physics/physics.hpp>
#include <oxwald/world/physics_bridge.hpp>
#endif

using namespace ox;
using namespace ox::world;

namespace {

Heightfield makeTerrain(u32 res = 1025, f32 spacing = 1.f, f32 relief = 40.f) {
    HeightfieldDesc d;
    d.resolution = res;
    d.worldSize = spacing * f32(res - 1);
    d.heightScale = relief;
    d.origin = {-d.worldSize * 0.5f, -d.worldSize * 0.5f};
    Heightfield hf(d);
    TerrainNoiseSettings ns;
    ns.fractal.frequency = 1.f / 200.f;
    ns.fractal.octaves = 4;
    generateNoise(hf, ns);
    return hf;
}

TerrainLodSettings lodSettings() {
    TerrainLodSettings s;
    s.leafNodeSize = 16;
    s.lodCount = 7;
    s.viewDistance = 4000.f;
    return s;
}

struct DrawnQuad {
    glm::vec2 lo, hi;
    u32 lod;
    usize patch;
    glm::vec2 morph;
};

std::vector<DrawnQuad> drawnQuads(const TerrainSelection& sel) {
    std::vector<DrawnQuad> out;
    for (usize i = 0; i < sel.patches.size(); ++i) {
        const TerrainPatch& p = sel.patches[i];
        const f32 h = p.size * 0.5f;
        for (u32 q = 0; q < 4; ++q) {
            if (p.quadrantMask & (1u << q)) {
                const glm::vec2 lo = p.offset + glm::vec2(f32(q & 1), f32(q >> 1)) * h;
                out.push_back({lo, lo + glm::vec2(h), p.lod, i, p.morphRange});
            }
        }
    }
    return out;
}

const DrawnQuad* findQuad(const std::vector<DrawnQuad>& quads, glm::vec2 p) {
    for (const DrawnQuad& q : quads) {
        if (p.x >= q.lo.x && p.y >= q.lo.y && p.x < q.hi.x && p.y < q.hi.y) {
            return &q;
        }
    }
    return nullptr;
}

} // namespace

TEST(TerrainLod, RangesAreMonotonicAndMorphContinuous) {
    const LodRanges r = LodRanges::compute(lodSettings());
    ASSERT_EQ(r.range.size(), 7u);
    EXPECT_NEAR(r.range.back(), 4000.f, 1e-2f);
    for (usize l = 0; l < r.range.size(); ++l) {
        EXPECT_FLOAT_EQ(r.morph[l].y, r.range[l]);
        EXPECT_LT(r.morph[l].x, r.morph[l].y);
        if (l > 0) {
            EXPECT_GT(r.range[l], r.range[l - 1]);
            EXPECT_NEAR(r.range[l] - r.range[l - 1], 2.f * (r.range[l - 1] - (l > 1 ? r.range[l - 2] : 0.f)), 1e-2f);
            // Morph of LOD l starts after LOD l-1 started morphing: bands never overlap backwards.
            EXPECT_GT(r.morph[l].x, r.morph[l - 1].x);
        }
    }
    // Morph factor: 0 before start, 1 at the end, continuous.
    EXPECT_EQ(cdlodMorphFactor(r.morph[2].x - 1.f, r.morph[2]), 0.f);
    EXPECT_EQ(cdlodMorphFactor(r.morph[2].y, r.morph[2]), 1.f);
    EXPECT_NEAR(cdlodMorphFactor((r.morph[2].x + r.morph[2].y) * 0.5f, r.morph[2]), 0.5f, 1e-5f);
    // Fully morphed odd vertices land exactly on even ones.
    const glm::vec2 odd = cdlodMorphVertex({3.f / 16.f, 5.f / 16.f}, 16, 1.f);
    EXPECT_NEAR(odd.x, 2.f / 16.f, 1e-6f);
    EXPECT_NEAR(odd.y, 4.f / 16.f, 1e-6f);
    const glm::vec2 even = cdlodMorphVertex({4.f / 16.f, 8.f / 16.f}, 16, 1.f);
    EXPECT_NEAR(even.x, 4.f / 16.f, 1e-6f);
}

TEST(TerrainLod, SelectionCoversTerrainOnceWithDetailNearCamera) {
    const Heightfield hf = makeTerrain();
    const TerrainQuadtree tree(hf, lodSettings());
    TerrainSelectParams p;
    p.cameraPosition = {37.f, hf.sampleHeight({37.f, -81.f}) + 2.f, -81.f};
    TerrainSelection sel;
    tree.select(p, sel);
    ASSERT_FALSE(sel.patches.empty());
    EXPECT_FALSE(sel.truncated);
    const auto quads = drawnQuads(sel);

    // Every point of the terrain (all within view distance) is covered by exactly one drawn quadrant.
    for (f32 z = -510.f; z < 510.f; z += 7.3f) {
        for (f32 x = -510.f; x < 510.f; x += 6.1f) {
            int n = 0;
            for (const DrawnQuad& q : quads) {
                n += x >= q.lo.x && z >= q.lo.y && x < q.hi.x && z < q.hi.y;
            }
            ASSERT_EQ(n, 1) << x << "," << z;
        }
    }
    // The camera's own node is LOD 0, and each LOD lies within its distance band.
    EXPECT_EQ(findQuad(quads, {p.cameraPosition.x, p.cameraPosition.z})->lod, 0u);
    const LodRanges& r = tree.ranges();
    for (const DrawnQuad& q : quads) {
        const Aabb box{{q.lo.x, sel.patches[q.patch].minY, q.lo.y}, {q.hi.x, sel.patches[q.patch].maxY, q.hi.y}};
        const f32 d = std::sqrt(distanceSq(box, p.cameraPosition));
        // The node is within its LOD range (a quadrant of a partial node may extend beyond it; its
        // vertices are then fully morphed to the next LOD, matching the coarser neighbour).
        const TerrainPatch& patch = sel.patches[q.patch];
        const Aabb node{{patch.offset.x, patch.minY, patch.offset.y}, {patch.offset.x + patch.size, patch.maxY, patch.offset.y + patch.size}};
        EXPECT_LE(std::sqrt(distanceSq(node, p.cameraPosition)), r.range[q.lod] + 1e-3f);
        if (q.lod > 0) {
            EXPECT_GT(d, r.range[q.lod - 1] - 1e-3f);
        }
    }
    // Bounded: far fewer patches than leaves, and LOD distribution grows outward.
    EXPECT_LT(sel.patches.size(), 300u);
    EXPECT_LT(sel.visitedNodes, 1200u);
    EXPECT_GT(sel.patchesPerLod[0], 0u);
}

TEST(TerrainLod, NeighbouringLodsAreCrackFree) {
    const Heightfield hf = makeTerrain();
    const TerrainQuadtree tree(hf, lodSettings());
    const u32 grid = tree.settings().leafNodeSize;
    for (const glm::vec3 cam : {glm::vec3(0.f, 30.f, 0.f), glm::vec3(-300.f, 80.f, 211.f), glm::vec3(480.f, 20.f, -490.f)}) {
        TerrainSelectParams p;
        p.cameraPosition = cam;
        TerrainSelection sel;
        tree.select(p, sel);
        const auto quads = drawnQuads(sel);
        u32 transitions = 0;
        for (const DrawnQuad& q : quads) {
            const f32 spacing = (q.hi.x - q.lo.x) / f32(grid / 2);
            // Walk the 4 edges of the quadrant at this LOD's vertex spacing.
            for (u32 e = 0; e < 4; ++e) {
                for (u32 i = 0; i <= grid / 2; ++i) {
                    glm::vec2 v, out;
                    const f32 t = f32(i) * spacing;
                    switch (e) {
                    case 0: v = {q.lo.x + t, q.lo.y}; out = {0.f, -1.f}; break;
                    case 1: v = {q.lo.x + t, q.hi.y}; out = {0.f, 1.f}; break;
                    case 2: v = {q.lo.x, q.lo.y + t}; out = {-1.f, 0.f}; break;
                    default: v = {q.hi.x, q.lo.y + t}; out = {1.f, 0.f}; break;
                    }
                    // Probe the neighbour just outside, at the middle of the edge segment side.
                    const glm::vec2 probe = v + out * 0.25f + glm::vec2(out.y, out.x) * 0.f;
                    const DrawnQuad* n = findQuad(quads, probe);
                    if (!n) {
                        continue; // terrain border
                    }
                    ASSERT_LE(std::abs(i32(n->lod) - i32(q.lod)), 1);
                    const glm::vec3 wp(v.x, hf.sampleHeight(v), v.y);
                    const f32 dist = glm::distance(wp, cam);
                    if (n->lod == q.lod + 1) {
                        ++transitions;
                        EXPECT_GT(cdlodMorphFactor(dist, q.morph), 0.999f) << "fine side not fully morphed";
                    } else if (n->lod + 1 == q.lod) {
                        EXPECT_EQ(cdlodMorphFactor(dist, q.morph), 0.f) << "coarse side already morphing";
                    }
                }
            }
        }
        EXPECT_GT(transitions, 0u);
    }
}

TEST(TerrainLod, FrustumCullingAndBoundsUpdate) {
    Heightfield hf = makeTerrain(513);
    TerrainLodSettings s = lodSettings();
    s.lodCount = 6;
    TerrainQuadtree tree(hf, s);
    TerrainSelectParams p;
    p.cameraPosition = {0.f, 50.f, 0.f};
    TerrainSelection all, culled;
    tree.select(p, all);
    const glm::mat4 view = glm::lookAt(p.cameraPosition, p.cameraPosition + glm::vec3(0.f, -0.2f, -1.f), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspective(glm::radians(60.f), 16.f / 9.f, 0.1f, 5000.f);
    p.frustum = Frustum::fromViewProjection(proj * view);
    tree.select(p, culled);
    EXPECT_LT(culled.patches.size(), all.patches.size());
    usize quadrants = 0, lines = 0;
    for (const TerrainPatch& patch : all.patches) {
        quadrants += usize(std::popcount(u32(patch.quadrantMask)));
    }
    tree.debugDraw(all, [&](glm::vec3, glm::vec3, glm::vec4) { ++lines; });
    EXPECT_EQ(lines, quadrants * 4);
    for (const TerrainPatch& patch : culled.patches) {
        EXPECT_LT(patch.offset.y, 1.f); // everything drawn lies towards -Z (the view direction)
    }

    // Raise a mountain; min/max bounds of the nodes containing it follow.
    BrushSettings b;
    b.radius = 20.f;
    b.strength = 500.f;
    const IRect dirty = applyBrush(hf, {100.f, 100.f}, b);
    tree.updateBounds(hf, dirty);
    const glm::vec2 s0 = hf.worldToSample({100.f, 100.f});
    const Aabb leaf = tree.nodeBounds(0, u32(s0.x) / 16, u32(s0.y) / 16);
    EXPECT_GE(leaf.max.y, hf.heightAtSample(i32(s0.x), i32(s0.y)) - 1e-3f);
    const Aabb root = tree.nodeBounds(5, 0, 0);
    EXPECT_NEAR(root.max.y, hf.maxHeight(), 1e-3f);
}

TEST(TerrainLod, PatchGpuLayout) {
    TerrainPatch p;
    p.offset = {10.f, -20.f};
    p.size = 64.f;
    p.lod = 2;
    p.morphRange = {100.f, 150.f};
    p.quadrantMask = 0b0101;
    const TerrainPatchGpu g = toGpu(p);
    EXPECT_EQ(g.offsetSizeLod, glm::vec4(10.f, -20.f, 64.f, 2.f));
    EXPECT_FLOAT_EQ(g.morph.z, 1.f / 50.f);
    EXPECT_EQ(g.morph.w, 5.f);
}

TEST(TerrainGrid, MeshLayoutQuadrantsAndWinding) {
    const u32 n = 16;
    const TerrainGridMesh m = generateTerrainGrid(n, true);
    EXPECT_EQ(m.vertices.size(), usize((n + 1) * (n + 1) + 4 * (n + 1)));
    u32 next = 0;
    for (const IndexRange& q : m.quadrants) {
        EXPECT_EQ(q.first, next);
        next += q.count;
        // Each quadrant: (n/2)^2 quads + 2 edges × n/2 skirt quads, 6 indices each.
        EXPECT_EQ(q.count, (n / 2) * (n / 2) * 6 + 2 * (n / 2) * 6);
    }
    EXPECT_EQ(next, m.indices.size());
    auto pos = [&](u32 i) {
        const TerrainGridVertex& v = m.vertices[i];
        return glm::vec3(v.u, -v.skirt, v.v); // skirt vertices pushed down by 1 for the test
    };
    for (usize t = 0; t < m.indices.size(); t += 3) {
        const glm::vec3 a = pos(m.indices[t]), b = pos(m.indices[t + 1]), c = pos(m.indices[t + 2]);
        const glm::vec3 nrm = glm::cross(b - a, c - a);
        const bool skirt = m.vertices[m.indices[t]].skirt + m.vertices[m.indices[t + 1]].skirt + m.vertices[m.indices[t + 2]].skirt > 0.f;
        if (!skirt) {
            EXPECT_GT(nrm.y, 0.f); // CCW seen from above
        } else {
            const glm::vec3 centre = (a + b + c) / 3.f - glm::vec3(0.5f, 0.f, 0.5f);
            EXPECT_GT(glm::dot(glm::vec3(nrm.x, 0.f, nrm.z), glm::vec3(centre.x, 0.f, centre.z)), 0.f); // outward
        }
    }
    const TerrainGridMesh plain = generateTerrainGrid(8, false);
    EXPECT_EQ(plain.indices.size(), 8u * 8u * 6u);
}

TEST(TerrainPhysics, TilesMatchHeightfield) {
    Heightfield hf = makeTerrain(129, 2.f);
    hf.setHole(70, 70, true);
    const auto tiles = buildPhysicsTiles(hf, 64);
    ASSERT_EQ(tiles.size(), 4u);
    const PhysicsHeightfieldTile& t = tiles[3]; // tile (1,1)
    EXPECT_EQ(t.tileX, 1);
    EXPECT_EQ(t.tileZ, 1);
    EXPECT_EQ(t.sampleCount, 65u);
    EXPECT_EQ(t.scale, glm::vec3(2.f, 1.f, 2.f));
    for (u32 z = 0; z < 65; z += 9) {
        for (u32 x = 0; x < 65; x += 7) {
            const glm::vec3 w = t.offset + t.scale * glm::vec3(f32(x), 0.f, f32(z));
            const glm::vec3 ref = hf.samplePosition(64 + x, 64 + z);
            EXPECT_EQ(w.x, ref.x);
            EXPECT_EQ(w.z, ref.z);
            if (!(x == 6 && z == 6)) {
                EXPECT_EQ(t.heights[z * 65 + x], ref.y);
            }
        }
    }
    EXPECT_EQ(t.heights[6 * 65 + 6], kPhysicsHeightHole);
    // Shared edges are identical between neighbouring tiles.
    for (u32 z = 0; z < 65; ++z) {
        EXPECT_EQ(tiles[0].heights[z * 65 + 64], tiles[1].heights[z * 65 + 0]);
    }
    // A dirty rect on the shared edge touches both tiles; clamped to the map.
    const auto touched = physicsTilesOverlapping({64, 10, 65, 11}, 64, 129);
    ASSERT_EQ(touched.size(), 2u);
    EXPECT_EQ(touched[0], glm::ivec2(0, 0));
    EXPECT_EQ(touched[1], glm::ivec2(1, 0));
    EXPECT_EQ(physicsTilesOverlapping({120, 120, 129, 129}, 64, 129).size(), 1u);
}

#if defined(OX_WORLD_TEST_PHYSICS)
TEST(TerrainPhysics, JoltHeightfieldMatchesTerrainHeight) {
    const Heightfield hf = makeTerrain(129, 1.f, 20.f);
    const PhysicsHeightfieldTile tile = buildPhysicsTile(hf, 0, 0, 64);
    physics::PhysicsWorldDesc wd;
    wd.workerThreads = 1;
    physics::PhysicsWorld world(wd);
    physics::BodyDesc bd;
    std::string err;
    bd.shape = physics::createShape(toShapeDesc(tile), &err);
    ASSERT_TRUE(bd.shape) << err;
    bd.motionType = physics::MotionType::Static;
    world.createBody(bd);
    world.step(1.f / 60.f);
    for (const glm::vec2 xz : {glm::vec2(-50.3f, -40.7f), glm::vec2(-10.1f, -3.3f), glm::vec2(-63.f, -20.5f)}) {
        const auto hit = world.raycast({xz.x, 100.f, xz.y}, {0.f, -1.f, 0.f}, 200.f);
        ASSERT_TRUE(hit) << xz.x << "," << xz.y;
        // Jolt triangulates quads differently from bilinear sampling and quantises heights: ~cm error.
        EXPECT_NEAR(hit->point.y, hf.sampleHeight(xz), 0.1f);
    }
}
#endif
