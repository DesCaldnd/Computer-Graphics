#include <oxwald/spline/spline_mesh.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <cmath>

using namespace ox;
using namespace ox::spline;

namespace {

Spline curve(bool closed) {
    Spline s(SplineType::CatmullRom, closed);
    s.addPoint({0, 0, 0});
    s.addPoint({4, 0, -3});
    s.addPoint({8, 0, 0});
    s.addPoint({12, 0, -4});
    return s;
}

void checkIndices(const ExtrudedMesh& m) {
    ASSERT_EQ(m.indices.size() % 3, 0u);
    for (u32 i : m.indices) {
        ASSERT_LT(i, m.vertices.size());
    }
}

} // namespace

TEST(SplineMesh, PipeCountsAndCaps) {
    const Spline s = curve(false);
    ExtrusionSettings es;
    es.spacing = 0.5f;
    const u32 rings = static_cast<u32>(std::lround(s.length() / es.spacing)) + 1;
    const ExtrusionProfile circle = makeCircleProfile(0.5f, 12);

    const ExtrudedMesh pipe = extrude(s, circle, es);
    EXPECT_EQ(pipe.vertices.size(), rings * 13u);
    EXPECT_EQ(pipe.indices.size(), (rings - 1) * 12u * 6u);
    checkIndices(pipe);
    // Normals point away from the centre line.
    const SplineSample mid = s.evaluate(1.5f);
    for (const SplineVertex& v : pipe.vertices) {
        EXPECT_NEAR(glm::length(v.normal), 1.0f, 1e-4f);
    }
    (void)mid;
    for (u32 c = 0; c < 12; ++c) {
        const SplineVertex& v = pipe.vertices[c];
        const glm::vec3 radial = glm::normalize(v.position - s.position(0.0f));
        EXPECT_GT(glm::dot(radial, v.normal), 0.999f);
    }
    // Last ring v = length.
    EXPECT_NEAR(pipe.vertices.back().uv.y, s.length(), 1e-3f);
    EXPECT_NEAR(pipe.vertices.back().uv.x, 1.0f, 1e-6f);

    es.capStart = es.capEnd = true;
    const ExtrudedMesh capped = extrude(s, circle, es);
    EXPECT_EQ(capped.vertices.size(), rings * 13u + 2u * 13u);
    EXPECT_EQ(capped.indices.size(), (rings - 1) * 12u * 6u + 2u * 12u * 3u);
    checkIndices(capped);
}

TEST(SplineMesh, TriangleWindingFacesOutward) {
    const Spline s = curve(false);
    const ExtrudedMesh pipe = extrude(s, makeCircleProfile(0.5f, 16), {});
    for (usize i = 0; i < pipe.indices.size(); i += 3) {
        const SplineVertex& a = pipe.vertices[pipe.indices[i]];
        const SplineVertex& b = pipe.vertices[pipe.indices[i + 1]];
        const SplineVertex& c = pipe.vertices[pipe.indices[i + 2]];
        const glm::vec3 faceN = glm::cross(b.position - a.position, c.position - a.position);
        EXPECT_GT(glm::dot(faceN, a.normal + b.normal + c.normal), 0.0f) << "triangle " << i / 3;
    }
}

TEST(SplineMesh, RoadStripOnClosedLoop) {
    const Spline s = curve(true);
    ExtrusionSettings es;
    es.spacing = 1.0f;
    es.capStart = true; // ignored: open profile on closed spline
    const ExtrudedMesh road = extrude(s, makeStripProfile(3.0f), es);
    const u32 rings = static_cast<u32>(std::lround(s.length())) + 1;
    EXPECT_EQ(road.vertices.size(), rings * 2u);
    EXPECT_EQ(road.indices.size(), (rings - 1) * 6u);
    checkIndices(road);
    for (const SplineVertex& v : road.vertices) {
        EXPECT_GT(v.normal.y, 0.999f); // planar loop in XZ: road faces up
    }
    // Seam ring duplicates the first one.
    EXPECT_LT(glm::length(road.vertices[0].position - road.vertices[(rings - 1) * 2].position), 1e-3f);
    EXPECT_LT(glm::length(road.vertices[1].position - road.vertices[(rings - 1) * 2 + 1].position), 1e-3f);
}

TEST(SplineMesh, RectangleHardEdges) {
    const Spline s = curve(false);
    const ExtrudedMesh box = extrude(s, makeRectangleProfile(2.0f, 1.0f), {});
    const SplineSample s0 = s.evaluate(0.0f);
    // First two profile points are the right side: normals equal the binormal.
    EXPECT_GT(glm::dot(box.vertices[0].normal, s0.binormal), 0.999f);
    EXPECT_GT(glm::dot(box.vertices[1].normal, s0.binormal), 0.999f);
    EXPECT_GT(glm::dot(box.vertices[2].normal, s0.normal), 0.999f);
}
