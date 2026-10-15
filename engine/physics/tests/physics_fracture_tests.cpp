#include "test_helpers.hpp"

#include <oxwald/physics/fracture.hpp>

#include <chrono>
#include <cmath>

using namespace ox;
using namespace ox::physics;
using namespace ox::physics::test;

namespace {

FractureResult fractureBox(u32 chunks, u64 seed, glm::vec3 half = {1.f, 0.5f, 0.75f}) {
    FractureSettings s;
    s.chunkCount = chunks;
    s.seed = seed;
    return voronoiFracture(makeBoxFractureMesh(half), s);
}

f32 sumVolume(const FractureResult& r) {
    f32 v = 0.f;
    for (const auto& c : r.chunks) v += c.volume;
    return v;
}

} // namespace

TEST(Fracture, BoxMeshIsClosedWithExpectedVolume) {
    const FractureMesh box = makeBoxFractureMesh({1.f, 2.f, 3.f});
    EXPECT_TRUE(isClosedMesh(box.positions, box.indices));
    EXPECT_TRUE(isConvexMesh(box.positions, box.indices));
    EXPECT_NEAR(meshVolume(box.positions, box.indices), 48.f, 1e-4f);
    EXPECT_TRUE(pointInsideMesh({0.9f, 1.9f, -2.9f}, box.positions, box.indices));
    EXPECT_FALSE(pointInsideMesh({1.1f, 0.f, 0.f}, box.positions, box.indices));
}

TEST(Fracture, ChunksAreClosedAndVolumesSumToSource) {
    const glm::vec3 half{1.f, 0.5f, 0.75f};
    const FractureResult r = fractureBox(16, 7, half);
    ASSERT_EQ(r.chunks.size(), 16u);
    const f32 expected = 8.f * half.x * half.y * half.z;
    EXPECT_NEAR(r.sourceVolume, expected, 1e-4f);
    EXPECT_NEAR(sumVolume(r), expected, expected * 1e-3f);
    for (usize i = 0; i < r.chunks.size(); ++i) {
        const FractureChunk& c = r.chunks[i];
        SCOPED_TRACE(i);
        EXPECT_GT(c.volume, 0.f);
        EXPECT_TRUE(isClosedMesh(c.positions, c.indices));
        // Chunk geometry is relative to its centroid: the local volume equals the stored one and the local
        // centroid is the origin.
        EXPECT_NEAR(meshVolume(c.positions, c.indices), c.volume, c.volume * 1e-3f);
        EXPECT_LT(glm::length(meshCentroid(c.positions, c.indices)), 1e-3f);
        EXPECT_LT(c.interiorIndexStart, c.indices.size()); // every chunk has cut faces (inner cells have no outer ones)
        EXPECT_EQ(c.normals.size(), c.positions.size());
        EXPECT_EQ(c.uvs.size(), c.positions.size());
    }
}

TEST(Fracture, ChunksStayInsideSourceBounds) {
    const glm::vec3 half{1.f, 0.5f, 0.75f};
    const FractureResult r = fractureBox(24, 99, half);
    const f32 tol = 1e-4f;
    for (const auto& c : r.chunks) {
        for (const glm::vec3& p : c.positions) {
            const glm::vec3 w = p + c.centroid;
            EXPECT_LE(std::abs(w.x), half.x + tol);
            EXPECT_LE(std::abs(w.y), half.y + tol);
            EXPECT_LE(std::abs(w.z), half.z + tol);
        }
        EXPECT_GE(c.boundsMin.x, -half.x - tol);
        EXPECT_LE(c.boundsMax.x, half.x + tol);
    }
}

TEST(Fracture, DeterministicForSameSeed) {
    const FractureResult a = fractureBox(12, 1234);
    const FractureResult b = fractureBox(12, 1234);
    EXPECT_EQ(serializeFracture(a), serializeFracture(b));
    const FractureResult c = fractureBox(12, 4321);
    EXPECT_NE(serializeFracture(a), serializeFracture(c));
    FractureSettings s;
    s.chunkCount = 12;
    s.seed = 1234;
    const FractureMesh box = makeBoxFractureMesh({1.f, 0.5f, 0.75f});
    EXPECT_EQ(fractureKey(box, s), fractureKey(box, s));
    EXPECT_NE(fractureKey(box, FractureSettings{}), fractureKey(box, FractureSettings{.chunkCount = 3}));
}

TEST(Fracture, ConvexHullsAreValidJoltShapes) {
    const FractureResult r = fractureBox(10, 5);
    ASSERT_FALSE(r.chunks.empty());
    for (const auto& c : r.chunks) {
        EXPECT_TRUE(isConvexMesh(c.positions, c.indices, 1e-3f)); // Voronoi cells of a convex source are convex
        ASSERT_GE(c.hullPoints.size(), 4u);
        std::string error;
        ShapeDesc d = ShapeDesc::convexHull(c.hullPoints);
        d.convexRadius = 0.f;
        ShapeRef shape = createShape(d, &error);
        ASSERT_TRUE(shape.valid()) << error;
        EXPECT_NEAR(shape.volume(), c.volume, c.volume * 0.02f);
        // Hull points are relative to the centroid: the hull's centre of mass is (almost) the origin.
        EXPECT_LT(glm::length(shape.centerOfMass()), 0.02f);
    }
    // Chunks simulate: drop them on a floor, they come to rest above it.
    PhysicsWorld world(smallWorld(0));
    addFloor(world);
    std::vector<BodyHandle> bodies;
    for (const auto& c : r.chunks) {
        BodyDesc d;
        d.shape = createShape(ShapeDesc::convexHull(c.hullPoints));
        d.position = c.centroid + glm::vec3(0.f, 2.f, 0.f);
        bodies.push_back(world.createBody(d));
    }
    stepFor(world, 3.f);
    for (BodyHandle b : bodies) EXPECT_GT(world.getTransform(b).position.y, -0.05f);
}

TEST(Fracture, LinksConnectNeighbouringChunks) {
    const FractureResult r = fractureBox(8, 3);
    ASSERT_EQ(r.chunks.size(), 8u);
    ASSERT_FALSE(r.links.empty());
    // The link graph of a solid box is connected.
    const auto adj = r.adjacency();
    std::vector<bool> seen(r.chunks.size(), false);
    std::vector<u32> stack{0};
    seen[0] = true;
    while (!stack.empty()) {
        const u32 i = stack.back();
        stack.pop_back();
        for (u32 j : adj[i]) {
            if (!seen[j]) {
                seen[j] = true;
                stack.push_back(j);
            }
        }
    }
    for (bool s : seen) EXPECT_TRUE(s);
    for (const auto& l : r.links) {
        EXPECT_LT(l.a, l.b);
        EXPECT_GT(l.area, 0.f);
    }
}

TEST(Fracture, ConvexHullSourceAndFocus) {
    // Octahedron given as points.
    const std::vector<glm::vec3> pts = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0.1f, 0.1f, 0.1f}};
    const FractureMesh hull = makeConvexHullFractureMesh(pts);
    ASSERT_EQ(hull.indices.size(), 8u * 3u);
    EXPECT_TRUE(isClosedMesh(hull.positions, hull.indices));
    EXPECT_NEAR(meshVolume(hull.positions, hull.indices), 4.f / 3.f, 1e-4f);
    FractureSettings s;
    s.chunkCount = 10;
    s.seed = 11;
    s.focus = glm::vec3(0.5f, 0.f, 0.f);
    s.focusRadius = 0.2f;
    const FractureResult r = voronoiFracture(hull, s);
    EXPECT_EQ(r.chunks.size(), 10u);
    EXPECT_NEAR(sumVolume(r), 4.f / 3.f, 4.f / 3.f * 1e-3f);
    u32 nearFocus = 0;
    for (const auto& c : r.chunks) {
        EXPECT_TRUE(isClosedMesh(c.positions, c.indices));
        if (glm::length(c.seed - *s.focus) <= 0.2f + 1e-4f) ++nearFocus;
    }
    EXPECT_GE(nearFocus, 5u);
}

TEST(Fracture, NonConvexClosedMeshKeepsVolume) {
    // L-shaped prism (non-convex, closed): two boxes merged by hand would need welding, so build it from a hull-free
    // extrusion of an L outline.
    const std::vector<glm::vec2> outline = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}}; // CCW
    FractureMesh m;
    const f32 h = 0.5f;
    auto vtx = [&](glm::vec2 p, f32 z) {
        m.positions.push_back({p.x, p.y, z});
        return u32(m.positions.size() - 1);
    };
    std::vector<u32> bottom, top;
    for (auto p : outline) bottom.push_back(vtx(p, -h));
    for (auto p : outline) top.push_back(vtx(p, h));
    // Caps: L outline = two quads (0,1,2,3)+(0,3,4,5), triangulated; top faces +z, bottom -z.
    const u32 quads[2][4] = {{0, 1, 2, 3}, {0, 3, 4, 5}};
    for (const auto& q : quads) {
        m.indices.insert(m.indices.end(), {top[q[0]], top[q[1]], top[q[2]], top[q[0]], top[q[2]], top[q[3]]});
        m.indices.insert(m.indices.end(), {bottom[q[0]], bottom[q[2]], bottom[q[1]], bottom[q[0]], bottom[q[3]], bottom[q[2]]});
    }
    for (usize i = 0; i < outline.size(); ++i) {
        const usize j = (i + 1) % outline.size();
        m.indices.insert(m.indices.end(), {bottom[i], bottom[j], top[j], bottom[i], top[j], top[i]});
    }
    ASSERT_TRUE(isClosedMesh(m.positions, m.indices));
    const f32 vol = meshVolume(m.positions, m.indices);
    ASSERT_NEAR(vol, 3.f, 1e-4f);
    FractureSettings s;
    s.chunkCount = 12;
    s.seed = 2;
    const FractureResult r = voronoiFracture(m, s);
    EXPECT_GE(r.chunks.size(), 10u);
    EXPECT_NEAR(sumVolume(r), vol, vol * 2e-3f);
    for (const auto& c : r.chunks) EXPECT_TRUE(isClosedMesh(c.positions, c.indices));
}

TEST(Fracture, SerializationRoundTripAndCorruption) {
    const FractureResult r = fractureBox(6, 77);
    const std::vector<u8> bytes = serializeFracture(r);
    auto back = deserializeFracture(bytes);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(serializeFracture(*back), bytes);
    EXPECT_EQ(back->chunks.size(), r.chunks.size());
    EXPECT_EQ(back->links.size(), r.links.size());
    std::vector<u8> broken = bytes;
    broken[broken.size() / 2] ^= 0x5A;
    EXPECT_FALSE(deserializeFracture(broken).has_value());
    EXPECT_FALSE(deserializeFracture(std::span<const u8>(bytes.data(), 10)).has_value());
}

TEST(Fracture, PerformanceBudget) {
    // 32 chunks of a box must stay well within a load-time budget (also a regression guard for the plane skip).
    const auto t0 = std::chrono::steady_clock::now();
    const FractureResult r = fractureBox(32, 9);
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(r.chunks.size(), 32u);
    EXPECT_LT(ms, 2000.0);
}
