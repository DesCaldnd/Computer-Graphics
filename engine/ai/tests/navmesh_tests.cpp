#include <oxwald/ai/nav_crowd.hpp>
#include <oxwald/ai/nav_query.hpp>
#include <oxwald/ai/nav_tile_cache.hpp>
#include <oxwald/ai/navmesh.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace ox;
using namespace ox::ai;

namespace {

NavMeshInput plane(f32 minX, f32 maxX, f32 minZ = -10.f, f32 maxZ = 10.f, u8 area = NavArea::Ground) {
    NavMeshInput in;
    in.addQuad({minX, 0, minZ}, {minX, 0, maxZ}, {maxX, 0, maxZ}, {maxX, 0, minZ}, area);
    return in;
}

NavMeshBuildSettings fastSettings() {
    NavMeshBuildSettings s;
    s.cellSize = 0.2f;
    s.cellHeight = 0.1f;
    s.agentRadius = 0.4f;
    s.agentHeight = 1.8f;
    return s;
}

f32 minZ(const NavPath& p) {
    f32 z = 1e9f;
    for (const auto& pt : p.points) {
        z = std::min(z, pt.z);
    }
    return z;
}

f32 maxZ(const NavPath& p) {
    f32 z = -1e9f;
    for (const auto& pt : p.points) {
        z = std::max(z, pt.z);
    }
    return z;
}

} // namespace

TEST(NavMesh, PathGoesAroundWall) {
    NavMeshInput in = plane(-10, 10);
    in.addBox({-0.5f, 0.f, -6.f}, {0.5f, 3.f, 10.f}); // wall with a gap at z < -6
    auto mesh = NavMesh::build(in, fastSettings());
    ASSERT_NE(mesh, nullptr);
    EXPECT_GT(mesh->stats().polygons, 2u);
    NavQuery q(*mesh);
    ASSERT_TRUE(q.valid());

    const NavPath path = q.findPath({-5, 0, 5}, {5, 0, 5});
    ASSERT_EQ(path.status, PathStatus::Complete);
    ASSERT_GE(path.points.size(), 3u);
    EXPECT_NEAR(path.points.front().x, -5.f, 0.1f);
    EXPECT_NEAR(path.points.back().x, 5.f, 0.1f);
    EXPECT_LT(minZ(path), -6.f) << "path must go around the wall end";
    EXPECT_GT(path.length(), 2.f * std::sqrt(25.f + 121.f) - 1.f);
    EXPECT_FALSE(path.corridor.empty());

    // Path never crosses the wall: check every segment that changes side happens below z = -6.
    for (usize i = 1; i < path.points.size(); ++i) {
        const auto& a = path.points[i - 1];
        const auto& b = path.points[i];
        if ((a.x < 0.f) != (b.x < 0.f)) {
            const f32 t = a.x / (a.x - b.x);
            EXPECT_LT(a.z + (b.z - a.z) * t, -6.f);
        }
    }

    // Smooth path follows the surface in small steps and reaches the goal.
    const auto smooth = q.smoothPath({-5, 0, 5}, {5, 0, 5}, {}, 0.5f);
    ASSERT_GT(smooth.size(), 20u);
    EXPECT_LT(glm::distance(smooth.back(), glm::vec3(5, 0, 5)), 0.3f);
    for (usize i = 1; i < smooth.size(); ++i) {
        EXPECT_LT(glm::distance(smooth[i - 1], smooth[i]), 0.55f);
    }

    // Debug draw emits polygon edges.
    int lines = 0;
    mesh->debugDraw([&](glm::vec3, glm::vec3, glm::vec4) { ++lines; });
    EXPECT_GT(lines, 10);
}

TEST(NavMesh, UnreachableTargetReturnsPartialPath) {
    NavMeshInput in = plane(-10, -1);
    in.append(plane(1, 10));
    auto mesh = NavMesh::build(in, fastSettings());
    ASSERT_NE(mesh, nullptr);
    NavQuery q(*mesh);
    const NavPath path = q.findPath({-5, 0, 0}, {5, 0, 0});
    ASSERT_EQ(path.status, PathStatus::Partial);
    ASSERT_FALSE(path.points.empty());
    EXPECT_GT(path.points.back().x, -2.f) << "partial path ends at the closest reachable edge";
    EXPECT_LT(path.points.back().x, -1.f);

    // Far away from any polygon → failed.
    EXPECT_EQ(q.findPath({-5, 0, 0}, {100, 0, 100}).status, PathStatus::Failed);
}

TEST(NavMesh, OffMeshLinkConnectsIslandsAndRespectsFlags) {
    NavMeshInput in = plane(-10, -1);
    in.append(plane(1, 10));
    in.offMeshLinks.push_back({.start = {-1.6f, 0, 0}, .end = {1.6f, 0, 0}, .radius = 0.5f});
    auto mesh = NavMesh::build(in, fastSettings());
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->stats().offMeshLinks, 1u);
    NavQuery q(*mesh);
    const NavPath path = q.findPath({-5, 0, 0}, {5, 0, 0});
    ASSERT_EQ(path.status, PathStatus::Complete);
    bool usesLink = false;
    for (u8 f : path.pointFlags) {
        usesLink |= (f & static_cast<u8>(PathPointFlag::OffMeshLink)) != 0;
    }
    EXPECT_TRUE(usesLink);

    const auto smooth = q.smoothPath({-5, 0, 0}, {5, 0, 0});
    ASSERT_FALSE(smooth.empty());
    EXPECT_LT(glm::distance(smooth.back(), glm::vec3(5, 0, 0)), 0.3f);

    NavQueryFilter noJump;
    noJump.excludeFlags |= NavFlags::Jump;
    EXPECT_EQ(q.findPath({-5, 0, 0}, {5, 0, 0}, noJump).status, PathStatus::Partial);
}

TEST(NavMesh, AreasFlagsAndCosts) {
    NavMeshInput in = plane(-10, 10);
    NavConvexVolume water;
    water.points = {{-1, 0, -11}, {-1, 0, 11}, {1, 0, 11}, {1, 0, -11}};
    water.minY = -1.f;
    water.maxY = 1.f;
    water.area = NavArea::Water;
    in.volumes.push_back(water);
    auto mesh = NavMesh::build(in, fastSettings());
    ASSERT_NE(mesh, nullptr);
    NavQuery q(*mesh);

    const auto onWater = q.nearestPoint({0, 0, 0});
    ASSERT_TRUE(onWater);
    EXPECT_EQ(mesh->polyArea(onWater->poly), NavArea::Water);
    EXPECT_EQ(mesh->polyFlags(onWater->poly), NavFlags::Swim);

    EXPECT_EQ(q.findPath({-5, 0, 0}, {5, 0, 0}).status, PathStatus::Complete);
    NavQueryFilter landOnly;
    landOnly.excludeFlags |= NavFlags::Swim;
    EXPECT_EQ(q.findPath({-5, 0, 0}, {5, 0, 0}, landOnly).status, PathStatus::Partial);

    // Runtime flag change: disabling the water polygons blocks the crossing too.
    const NavPath before = q.findPath({-5, 0, 0}, {5, 0, 0});
    for (NavPolyRef ref : before.corridor) {
        if (mesh->polyArea(ref) == NavArea::Water) {
            mesh->setPolyFlags(ref, NavFlags::Disabled);
        }
    }
    EXPECT_NE(q.findPath({-5, 0, 0}, {5, 0, 0}).status, PathStatus::Complete);
}

TEST(NavMesh, QueriesNearestRaycastRandom) {
    NavMeshInput in = plane(-10, 10);
    in.addBox({-0.5f, 0.f, -6.f}, {0.5f, 3.f, 10.f});
    auto mesh = NavMesh::build(in, fastSettings());
    ASSERT_NE(mesh, nullptr);
    NavQuery q(*mesh);

    const auto n = q.nearestPoint({3.f, 1.f, 3.f});
    ASSERT_TRUE(n);
    EXPECT_NEAR(n->position.y, 0.f, 0.2f);
    EXPECT_NEAR(n->position.x, 3.f, 1e-3f);
    EXPECT_FALSE(q.nearestPoint({50, 0, 50}).has_value());
    ASSERT_TRUE(q.heightAt({3, 0.5f, 3}));
    EXPECT_NEAR(*q.heightAt({3, 0.5f, 3}), 0.f, 0.2f);

    const NavRaycastHit blocked = q.raycast({-5, 0, 5}, {5, 0, 5});
    EXPECT_TRUE(blocked.hit);
    EXPECT_LT(blocked.position.x, -0.5f);
    EXPECT_GT(blocked.position.x, -1.5f);
    EXPECT_NEAR(std::abs(blocked.normal.x), 1.f, 1e-3f);
    const NavRaycastHit open = q.raycast({-5, 0, -8}, {5, 0, -8});
    EXPECT_FALSE(open.hit);

    const auto r1 = q.randomPoint(42), r2 = q.randomPoint(42);
    ASSERT_TRUE(r1 && r2);
    EXPECT_EQ(r1->position, r2->position) << "seeded random point is deterministic";
    for (u32 seed = 0; seed < 20; ++seed) {
        const auto r = q.randomPointInRadius({-5, 0, 5}, 2.f, seed);
        ASSERT_TRUE(r);
        // Detour picks polygons touching the circle; points can lie slightly outside.
        EXPECT_LT(glm::distance(glm::vec2(r->position.x, r->position.z), glm::vec2(-5, 5)), 6.f);
    }
}

TEST(NavMesh, TiledBuildAndTileRebuildAfterObstacle) {
    NavMeshBuildSettings s = fastSettings();
    s.tiled = true;
    s.tileSize = 32;
    const NavMeshInput open = plane(-10, 10);
    auto mesh = NavMesh::build(open, s);
    ASSERT_NE(mesh, nullptr);
    EXPECT_TRUE(mesh->tiled());
    EXPECT_EQ(mesh->tileGridSize(), glm::ivec2(4, 4));
    EXPECT_GE(mesh->stats().tiles, 12u); // edge tiles may be fully eroded
    NavQuery q(*mesh);
    const NavPath before = q.findPath({-5, 0, 0}, {5, 0, 0});
    ASSERT_EQ(before.status, PathStatus::Complete);
    EXPECT_NEAR(before.length(), 10.f, 0.2f);

    NavMeshInput blocked = open;
    const glm::vec3 wmin(-0.5f, 0.f, -10.f), wmax(0.5f, 3.f, 6.f);
    blocked.addBox(wmin, wmax);
    const i32 rebuilt = mesh->rebuildTiles(blocked, wmin, wmax);
    EXPECT_GT(rebuilt, 0);
    EXPECT_LT(rebuilt, 16) << "only tiles touching the obstacle are rebuilt";
    const NavPath after = q.findPath({-5, 0, 0}, {5, 0, 0});
    ASSERT_EQ(after.status, PathStatus::Complete);
    EXPECT_GT(maxZ(after), 6.f);
    EXPECT_GT(after.length(), 15.f);

    // Remove the obstacle again.
    mesh->rebuildTiles(open, wmin, wmax);
    EXPECT_NEAR(q.findPath({-5, 0, 0}, {5, 0, 0}).length(), 10.f, 0.2f);
}

TEST(NavMesh, SerializationRoundTrip) {
    for (bool tiled : {false, true}) {
        NavMeshBuildSettings s = fastSettings();
        s.tiled = tiled;
        NavMeshInput in = plane(-10, 10);
        in.addBox({-0.5f, 0.f, -6.f}, {0.5f, 3.f, 10.f});
        auto mesh = NavMesh::build(in, s);
        ASSERT_NE(mesh, nullptr);
        const std::vector<u8> blob = mesh->serialize();
        ASSERT_GT(blob.size(), 100u);
        auto loaded = NavMesh::deserialize(blob);
        ASSERT_NE(loaded, nullptr) << "tiled=" << tiled;
        EXPECT_EQ(loaded->stats().polygons, mesh->stats().polygons);
        EXPECT_EQ(loaded->stats().tiles, mesh->stats().tiles);
        EXPECT_EQ(loaded->tiled(), tiled);

        NavQuery qa(*mesh), qb(*loaded);
        const NavPath a = qa.findPath({-5, 0, 5}, {5, 0, 5});
        const NavPath b = qb.findPath({-5, 0, 5}, {5, 0, 5});
        ASSERT_EQ(a.points.size(), b.points.size());
        for (usize i = 0; i < a.points.size(); ++i) {
            EXPECT_LT(glm::distance(a.points[i], b.points[i]), 1e-4f);
        }
        EXPECT_EQ(loaded->serialize(), blob) << "re-serialisation is byte identical";
    }
    std::vector<u8> garbage(64, 0xAB);
    EXPECT_EQ(NavMesh::deserialize(garbage), nullptr);
}

TEST(NavCrowd, AgentsReachGoalsWithoutOverlapping) {
    auto mesh = NavMesh::build(plane(-10, 10), fastSettings());
    ASSERT_NE(mesh, nullptr);
    NavCrowd crowd(*mesh, 16, 1.f);
    ASSERT_TRUE(crowd.valid());
    NavAgentParams p;
    p.radius = 0.4f;
    p.maxSpeed = 3.f;
    std::vector<NavAgent> agents;
    std::vector<glm::vec3> goals;
    for (int i = 0; i < 4; ++i) {
        const f32 z = -3.f + 2.f * static_cast<f32>(i);
        agents.push_back(crowd.addAgent({-6, 0, z}, p));
        goals.push_back({6, 0, z});
        agents.push_back(crowd.addAgent({6, 0, z + 0.5f}, p));
        goals.push_back({-6, 0, z + 0.5f});
    }
    EXPECT_EQ(crowd.agentCount(), 8);
    for (usize i = 0; i < agents.size(); ++i) {
        ASSERT_TRUE(agents[i].valid());
        ASSERT_TRUE(crowd.setTarget(agents[i], goals[i]));
    }
    f32 minDist = 1e9f;
    for (int step = 0; step < 800; ++step) {
        crowd.update(0.05f);
        for (usize a = 0; a < agents.size(); ++a) {
            for (usize b = a + 1; b < agents.size(); ++b) {
                const glm::vec3 pa = crowd.position(agents[a]), pb = crowd.position(agents[b]);
                minDist = std::min(minDist, glm::length(glm::vec2(pa.x - pb.x, pa.z - pb.z)));
            }
        }
    }
    for (usize i = 0; i < agents.size(); ++i) {
        EXPECT_TRUE(crowd.reachedTarget(agents[i], 0.6f))
            << "agent " << i << " at " << crowd.position(agents[i]).x << "," << crowd.position(agents[i]).z;
        EXPECT_EQ(crowd.state(agents[i]), NavAgentState::Walking);
    }
    EXPECT_GT(minDist, 0.6f * 2.f * p.radius) << "agents should not overlap much";

    int lines = 0;
    crowd.debugDraw([&](glm::vec3, glm::vec3, glm::vec4) { ++lines; });
    EXPECT_GT(lines, 8 * 16);
    crowd.removeAgent(agents[0]);
    EXPECT_EQ(crowd.agentCount(), 7);
}

TEST(NavTileCache, DynamicObstacleChangesPath) {
    NavTileCacheSettings s;
    s.build = fastSettings();
    s.build.tileSize = 32;
    auto cache = NavTileCache::build(plane(-10, 10), s);
    ASSERT_NE(cache, nullptr);
    NavQuery q(cache->navMesh());
    const NavPath before = q.findPath({-5, 0, 0}, {5, 0, 0});
    ASSERT_EQ(before.status, PathStatus::Complete);
    EXPECT_NEAR(before.length(), 10.f, 0.2f);

    const NavObstacleId box = cache->addBox({-0.5f, -0.5f, -7.f}, {0.5f, 2.f, 7.f});
    ASSERT_NE(box, 0u);
    cache->flush();
    EXPECT_EQ(cache->obstacleCount(), 1);
    const NavPath blocked = q.findPath({-5, 0, 0}, {5, 0, 0});
    ASSERT_EQ(blocked.status, PathStatus::Complete);
    EXPECT_GT(blocked.length(), 15.f);

    const NavObstacleId cyl = cache->addCylinder({-5.f, -0.5f, 0.f}, 1.f, 2.f);
    ASSERT_NE(cyl, 0u);
    cache->flush();
    const auto n = q.nearestPoint({-5, 0, 0});
    ASSERT_TRUE(n);
    EXPECT_GT(glm::distance(n->position, glm::vec3(-5, 0, 0)), 0.9f) << "cylinder carves the navmesh";

    EXPECT_TRUE(cache->removeObstacle(box));
    EXPECT_TRUE(cache->removeObstacle(cyl));
    cache->flush();
    EXPECT_EQ(cache->obstacleCount(), 0);
    EXPECT_NEAR(q.findPath({-5, 0, 0}, {5, 0, 0}).length(), 10.f, 0.2f);
}
