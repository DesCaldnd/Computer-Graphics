#include <oxwald/world/terrain_gen.hpp>
#include <oxwald/world/vegetation.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <set>

#if defined(OX_WORLD_TEST_PHYSICS)
#include <oxwald/world/physics_bridge.hpp>
#endif

using namespace ox;
using namespace ox::world;

namespace {

f32 minPairDistance(const std::vector<glm::vec2>& pts, f32 wrapPeriod = 0.f) {
    f32 best = 1e30f;
    for (usize i = 0; i < pts.size(); ++i) {
        for (usize j = i + 1; j < pts.size(); ++j) {
            glm::vec2 d = glm::abs(pts[i] - pts[j]);
            if (wrapPeriod > 0.f) {
                d = glm::min(d, glm::vec2(wrapPeriod) - d);
            }
            best = std::min(best, glm::length(d));
        }
    }
    return best;
}

// Half flat meadow (x < 0), half steep slope (x >= 0, ~56°).
Heightfield meadowAndCliff() {
    HeightfieldDesc d;
    d.resolution = 129;
    d.worldSize = 128.f;
    d.heightScale = 200.f;
    d.origin = {-64.f, -64.f};
    Heightfield hf(d);
    for (u32 z = 0; z < 129; ++z) {
        for (u32 x = 0; x < 129; ++x) {
            const f32 wx = hf.samplePosition(x, z).x;
            hf.setHeightAtSample(x, z, wx < 0.f ? 5.f : 5.f + 1.5f * wx);
        }
    }
    return hf;
}

VegetationLayer treeLayer() {
    VegetationLayer l;
    l.name = "pine";
    l.kind = VegetationKind::Tree;
    l.prototype = 3;
    l.minDistance = 4.f;
    l.maxSlopeDeg = 30.f;
    l.collider = true;
    l.colliderRadius = 0.4f;
    l.colliderHalfHeight = 3.f;
    l.boundingRadius = 5.f;
    l.seed = 11;
    return l;
}

VegetationLayer grassLayer() {
    VegetationLayer l;
    l.name = "grass";
    l.kind = VegetationKind::Grass;
    l.minDistance = 1.f;
    l.maxSlopeDeg = 70.f;
    l.alignToNormal = 1.f;
    l.tintA = {0.3f, 0.6f, 0.2f, 1.f};
    l.tintB = {0.5f, 0.7f, 0.1f, 1.f};
    l.lod.cullDistance = 60.f;
    l.lod.impostorDistance = 0.f;
    l.seed = 12;
    return l;
}

} // namespace

TEST(PoissonDisk, MinimumDistanceRespected) {
    const auto pts = PoissonDisk::generate({50.f, 30.f}, 2.f, 1);
    ASSERT_GT(pts.size(), 150u); // reasonably dense (max packing ~ 0.9 * area / r²)
    EXPECT_GE(minPairDistance(pts), 2.f - 1e-4f);
    for (const glm::vec2 p : pts) {
        EXPECT_TRUE(p.x >= 0.f && p.x < 50.f && p.y >= 0.f && p.y < 30.f);
    }
    // Deterministic per seed.
    EXPECT_EQ(PoissonDisk::generate({50.f, 30.f}, 2.f, 1), pts);
    EXPECT_NE(PoissonDisk::generate({50.f, 30.f}, 2.f, 2), pts);
}

TEST(PoissonDisk, TileablePatternWrapsSeamlessly) {
    const f32 period = 40.f;
    const auto pts = PoissonDisk::generateTileable(period, 1.5f, 7);
    ASSERT_GT(pts.size(), 300u);
    EXPECT_GE(minPairDistance(pts, period), 1.5f - 1e-4f);
}

TEST(Vegetation, ScatterRespectsRulesAndMinDistance) {
    const Heightfield hf = meadowAndCliff();
    const VegetationScatterer scatterer({treeLayer(), grassLayer()});
    ScatterContext ctx;
    ctx.heightfield = &hf;
    const VegetationChunk chunk = scatterer.scatter({-64.f, -64.f}, 128.f, ctx);
    std::vector<glm::vec2> trees, grassOnCliff;
    for (const VegetationInstance& v : chunk.instances) {
        const glm::vec2 xz(v.position.x, v.position.z);
        const f32 slope = glm::degrees(hf.sampleSlope(xz));
        if (v.layer == 0) {
            trees.push_back(xz);
            EXPECT_LE(slope, 30.f) << "tree on a steep slope";
            EXPECT_EQ(v.prototype, 3u);
            EXPECT_NEAR(v.position.y, hf.sampleHeight(xz), 1e-3f);
            // Upright (alignToNormal = 0).
            EXPECT_NEAR((v.rotation * glm::vec3(0, 1, 0)).y, 1.f, 1e-4f);
        } else {
            if (xz.x > 2.f) {
                grassOnCliff.push_back(xz);
                // Aligned to the terrain normal on the slope.
                EXPECT_GT(glm::dot(v.rotation * glm::vec3(0, 1, 0), hf.sampleNormal(xz)), 0.999f);
            }
        }
        EXPECT_GE(v.scale, 0.8f);
        EXPECT_LE(v.scale, 1.2f);
        EXPECT_GE(v.random, 0.f);
        EXPECT_LT(v.random, 1.f);
    }
    EXPECT_GT(trees.size(), 50u);
    EXPECT_GT(grassOnCliff.size(), 100u); // grass allows 70°
    for (const glm::vec2 t : trees) {
        EXPECT_LT(t.x, 1.f); // all trees on the meadow side
    }
    EXPECT_GE(minPairDistance(trees), 4.f - 1e-3f);
    // Colliders for trees only, standing on the ground.
    EXPECT_EQ(chunk.colliders.size(), trees.size());
    for (const VegetationCollider& c : chunk.colliders) {
        const VegetationInstance& inst = chunk.instances[c.instanceIndex];
        EXPECT_EQ(inst.layer, 0u);
        EXPECT_NEAR(c.position.y - (c.halfHeight + c.radius), inst.position.y, 1e-3f);
        EXPECT_NEAR(c.radius, 0.4f * inst.scale, 1e-5f);
    }
}

TEST(Vegetation, SeamlessAcrossChunks) {
    const Heightfield hf = meadowAndCliff();
    const VegetationScatterer scatterer({treeLayer()});
    ScatterContext ctx;
    ctx.heightfield = &hf;
    const VegetationChunk whole = scatterer.scatter({-64.f, -64.f}, 64.f, ctx);
    std::vector<VegetationInstance> parts;
    for (int z = 0; z < 2; ++z) {
        for (int x = 0; x < 2; ++x) {
            const auto c = scatterer.scatter({-64.f + 32.f * f32(x), -64.f + 32.f * f32(z)}, 32.f, ctx);
            parts.insert(parts.end(), c.instances.begin(), c.instances.end());
        }
    }
    ASSERT_EQ(parts.size(), whole.instances.size());
    auto key = [](const VegetationInstance& v) { return std::make_pair(v.position.x, v.position.z); };
    std::set<std::pair<f32, f32>> a, b;
    for (const auto& v : whole.instances) {
        a.insert(key(v));
    }
    for (const auto& v : parts) {
        b.insert(key(v));
    }
    EXPECT_EQ(a, b);
    // Min distance holds across the chunk borders.
    std::vector<glm::vec2> pts;
    for (const auto& v : parts) {
        pts.emplace_back(v.position.x, v.position.z);
    }
    EXPECT_GE(minPairDistance(pts), 4.f - 1e-3f);
}

TEST(Vegetation, DensityExclusionAndSplatRules) {
    const Heightfield hf = meadowAndCliff();
    VegetationLayer g = grassLayer();
    g.densityMapIndex = 0;
    g.splatLayer = 1;
    g.minSplatWeight = 0.5f;
    const VegetationScatterer scatterer({g});
    DensityMap dm;
    dm.resolution = 2;
    dm.origin = {-64.f, -64.f};
    dm.worldSize = 128.f;
    dm.values = {1.f, 1.f, 1.f, 1.f};
    SplatMap splat(65, 2, {-64.f, -64.f}, 128.f);
    for (u32 z = 0; z < 65; ++z) {
        for (u32 x = 0; x < 65; ++x) {
            if (z >= 32) { // southern half painted with layer 1
                splat.setWeight(x, z, 0, 0);
                splat.setWeight(x, z, 1, 255);
            }
        }
    }
    const ExclusionZone road{ExclusionZone::Shape::Rect, {-30.f, 20.f}, {5.f, 100.f}};
    ScatterContext ctx;
    ctx.heightfield = &hf;
    ctx.splat = &splat;
    ctx.densityMaps = std::span<const DensityMap>(&dm, 1);
    ctx.exclusions = std::span<const ExclusionZone>(&road, 1);
    const auto chunk = scatterer.scatter({-64.f, -64.f}, 128.f, ctx);
    ASSERT_GT(chunk.instances.size(), 100u);
    for (const auto& v : chunk.instances) {
        EXPECT_GT(v.position.z, -1.f); // splat layer 1 only in the south
        EXPECT_FALSE(road.contains({v.position.x, v.position.z}));
    }
    // Zero density → nothing.
    dm.values = {0.f, 0.f, 0.f, 0.f};
    EXPECT_TRUE(scatterer.scatter({-64.f, -64.f}, 128.f, ctx).instances.empty());
    // Custom density halves the count roughly.
    dm.values = {1.f, 1.f, 1.f, 1.f};
    ctx.customDensity = [](glm::vec2, u32) { return 0.5f; };
    const auto half = scatterer.scatter({-64.f, -64.f}, 128.f, ctx);
    EXPECT_NEAR(f32(half.instances.size()) / f32(chunk.instances.size()), 0.5f, 0.1f);
}

TEST(Vegetation, GpuInstanceLayout) {
    VegetationInstance v;
    v.position = {10.f, 2.f, -5.f};
    v.rotation = glm::angleAxis(glm::radians(90.f), glm::vec3(0, 1, 0));
    v.scale = 2.f;
    v.tint = packRgba8({1.f, 0.5f, 0.f, 1.f});
    v.random = 0.25f;
    v.layer = 4;
    v.prototype = 9;
    const VegetationLayer layer = treeLayer();
    const VegetationInstanceGpu g = toGpu(v, layer);
    static_assert(sizeof(g) == 64);
    // Row-major 3x4: world = rows · (local, 1).
    const glm::vec4 local(1.f, 0.f, 0.f, 1.f);
    const glm::vec3 world(glm::dot(g.transform[0], local), glm::dot(g.transform[1], local), glm::dot(g.transform[2], local));
    const glm::vec3 expected = glm::vec3(instanceMatrix(v) * local);
    EXPECT_NEAR(glm::distance(world, expected), 0.f, 1e-5f);
    EXPECT_NEAR(world.z, -5.f - 2.f, 1e-5f); // +X rotated 90° about Y → -Z, scaled by 2
    EXPECT_EQ(g.prototypeLayerFlags & 0xFFFFu, 9u);
    EXPECT_EQ((g.prototypeLayerFlags >> 16) & 0xFFu, 4u);
    EXPECT_TRUE(g.prototypeLayerFlags & kVegFlagTree);
    EXPECT_FLOAT_EQ(g.boundingRadius, 10.f);
    EXPECT_NEAR(unpackRgba8(g.tint).g, 0.5f, 1.f / 255.f);
}

TEST(Vegetation, LodBandsAndCellCulling) {
    VegetationLodSettings s;
    s.lodDistances[0] = 20.f;
    s.lodDistances[1] = 50.f;
    s.impostorDistance = 100.f;
    s.cullDistance = 300.f;
    s.fadeRange = 5.f;
    EXPECT_EQ(selectVegetationLod(s, 10.f).lod, 0u);
    EXPECT_FLOAT_EQ(selectVegetationLod(s, 10.f).fade, 1.f);
    EXPECT_NEAR(selectVegetationLod(s, 17.5f).fade, 0.5f, 1e-5f);
    EXPECT_EQ(selectVegetationLod(s, 30.f).lod, 1u);
    EXPECT_EQ(selectVegetationLod(s, 80.f).lod, 2u);
    EXPECT_EQ(selectVegetationLod(s, 150.f).lod, VegetationLodResult::kImpostor);
    EXPECT_EQ(selectVegetationLod(s, 301.f).lod, VegetationLodResult::kCulled);
    s.impostorDistance = 0.f;
    EXPECT_EQ(selectVegetationLod(s, 150.f).lod, 2u);

    const Heightfield hf = meadowAndCliff();
    const std::vector<VegetationLayer> layers{treeLayer(), grassLayer()};
    const VegetationScatterer scatterer(layers);
    ScatterContext ctx;
    ctx.heightfield = &hf;
    const auto chunk = scatterer.scatter({-64.f, -64.f}, 128.f, ctx, 16.f);
    ASSERT_GT(chunk.cells.size(), 20u);
    usize total = 0;
    for (const VegetationCell& c : chunk.cells) {
        total += c.count;
        for (u32 i = c.first; i < c.first + c.count; ++i) {
            EXPECT_EQ(chunk.instances[i].layer, c.layer);
            EXPECT_GE(chunk.instances[i].position.x, c.bounds.min.x);
            EXPECT_LE(chunk.instances[i].position.x, c.bounds.max.x);
        }
    }
    EXPECT_EQ(total, chunk.instances.size());

    std::vector<VisibleVegetationCell> all, culled;
    cullVegetationCells(chunk, layers, Frustum::infinite(), {-60.f, 10.f, -60.f}, all);
    const glm::mat4 vp = glm::perspective(glm::radians(60.f), 1.f, 0.1f, 1000.f) *
                         glm::lookAt(glm::vec3(-60.f, 10.f, -60.f), glm::vec3(-60.f, 10.f, -61.f), glm::vec3(0, 1, 0));
    cullVegetationCells(chunk, layers, Frustum::fromViewProjection(vp), {-60.f, 10.f, -60.f}, culled);
    EXPECT_LT(culled.size(), all.size());
    for (const auto& v : all) {
        if (chunk.cells[v.cell].layer == 1) {
            EXPECT_LE(v.distance, 60.f); // grass culled beyond 60 m
        }
    }
    EXPECT_LT(std::count_if(all.begin(), all.end(), [&](const auto& v) { return chunk.cells[v.cell].layer == 1; }),
              std::count_if(chunk.cells.begin(), chunk.cells.end(), [](const auto& c) { return c.layer == 1; }));
}

#if defined(OX_WORLD_TEST_PHYSICS)
TEST(Vegetation, ColliderToPhysicsShape) {
    VegetationCollider c;
    c.radius = 0.5f;
    c.halfHeight = 2.f;
    const physics::ShapeDesc d = toShapeDesc(c);
    EXPECT_EQ(d.type, physics::ShapeType::Capsule);
    EXPECT_FLOAT_EQ(d.radius, 0.5f);
    EXPECT_FLOAT_EQ(d.halfHeight, 2.f);
}
#endif
