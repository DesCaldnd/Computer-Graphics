#include <oxwald/core/debug_draw.hpp>

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace ox;

TEST(DebugDraw, ColorPacking) {
    EXPECT_EQ(debug_color::kRed, 0xff0000ffu);
    EXPECT_EQ(debug_color::pack({1, 0, 0, 1}), debug_color::kRed);
    EXPECT_EQ(debug_color::pack({0, 0, 1, 0.5f}), debug_color::rgba(0, 0, 255, 128));
    EXPECT_EQ(debug_color::pack({2, -1, 0, 1}), debug_color::rgba(255, 0, 0, 255)); // clamped
    const glm::vec4 c = debug_color::unpack(debug_color::rgba(51, 102, 153, 204));
    EXPECT_NEAR(c.r, 0.2f, 1e-6f);
    EXPECT_NEAR(c.a, 0.8f, 1e-6f);
}

TEST(DebugDraw, VertexCountsPerShape) {
    DebugDraw dd;
    const auto countAfter = [&](auto submit) {
        dd.clear();
        submit();
        dd.flush(0.0f);
        return dd.depthTestedLines().size() + dd.overlayLines().size();
    };
    EXPECT_EQ(countAfter([&] { dd.line({0, 0, 0}, {1, 0, 0}); }), 2u);
    EXPECT_EQ(countAfter([&] { dd.ray({0, 0, 0}, {0, 0, 5}, 2.0f); }), 2u);
    EXPECT_EQ(countAfter([&] { dd.aabb(AABB::fromCenterExtents({0, 0, 0}, glm::vec3{1})); }), 24u);
    EXPECT_EQ(countAfter([&] { dd.aabb(AABB{}); }), 0u); // invalid box draws nothing
    EXPECT_EQ(countAfter([&] { dd.box({0, 0, 0}, {1, 2, 3}, glm::quat{1, 0, 0, 0}); }), 24u);
    EXPECT_EQ(countAfter([&] { dd.obb(OBB{}); }), 24u);
    EXPECT_EQ(countAfter([&] { dd.sphere({0, 0, 0}, 1.0f, {}, 0.0f, true, 16); }), 3u * 16u * 2u);
    EXPECT_EQ(countAfter([&] { dd.circle({0, 0, 0}, {0, 1, 0}, 1.0f, {}, 0.0f, true, 20); }), 40u);
    EXPECT_EQ(countAfter([&] { dd.cylinder({0, 0, 0}, {0, 2, 0}, 1.0f, {}, 0.0f, true, 8); }), (8u * 2u + 4u) * 2u);
    EXPECT_EQ(countAfter([&] { dd.capsule({0, 0, 0}, {0, 2, 0}, 0.5f, {}, 0.0f, true, 8); }),
              (8u * 2u + 4u + 4u * 4u) * 2u);
    EXPECT_EQ(countAfter([&] { dd.cone({0, 0, 0}, {0, 0, -1}, 2.0f, 0.5f, {}, 0.0f, true, 12); }), (12u + 4u) * 2u);
    EXPECT_EQ(countAfter([&] { dd.arrow({0, 0, 0}, {0, 0, 1}, 0.2f); }), 10u);
    EXPECT_EQ(countAfter([&] { dd.axes(Transform{}, 1.0f); }), 6u);
    EXPECT_EQ(countAfter([&] { dd.grid({0, 0, 0}, 1.0f, 10); }), 11u * 4u);
    EXPECT_EQ(countAfter([&] { dd.point({1, 2, 3}, 0.5f); }), 6u);
    const glm::mat4 vp = perspectiveReversedZ(1.0f, 1.0f, 0.1f, 10.0f);
    EXPECT_EQ(countAfter([&] { dd.frustum(glm::inverse(vp)); }), 24u);
}

TEST(DebugDraw, GeometryAndColors) {
    DebugDraw dd;
    dd.line({0, 0, 0}, {1, 2, 3}, glm::vec4{0, 1, 0, 1});
    dd.line({0, 0, 0}, {1, 0, 0}, debug_color::kYellow, 0.0f, false);
    dd.sphere({5, 0, 0}, 2.0f);
    dd.axes(Transform{{1, 1, 1}, glm::quat{1, 0, 0, 0}, glm::vec3{1}}, 2.0f);
    dd.flush(0.016f);

    auto depth = dd.depthTestedLines();
    auto overlay = dd.overlayLines();
    ASSERT_EQ(overlay.size(), 2u);
    EXPECT_EQ(overlay[0].color, debug_color::kYellow);
    ASSERT_GE(depth.size(), 2u);
    EXPECT_EQ(depth[0].color, debug_color::kGreen);
    EXPECT_EQ(depth[1].position, glm::vec3(1, 2, 3));
    // Every sphere vertex lies on the sphere.
    for (usize i = 2; i < 2 + 3 * 24 * 2; ++i) {
        EXPECT_NEAR(glm::length(depth[i].position - glm::vec3{5, 0, 0}), 2.0f, 1e-4f);
    }
    // Axes: X red to (3,1,1).
    const usize axesStart = 2 + 3 * 24 * 2;
    EXPECT_EQ(depth[axesStart].color, debug_color::kRed);
    EXPECT_TRUE(nearlyEqual(depth[axesStart + 1].position, glm::vec3{3, 1, 1}));
    EXPECT_EQ(depth[axesStart + 5].color, debug_color::kBlue);
}

TEST(DebugDraw, LifetimeExpiry) {
    DebugDraw dd;
    dd.line({0, 0, 0}, {1, 0, 0});                // one flush
    dd.line({0, 0, 0}, {0, 1, 0}, {}, 0.05f);      // ~3 flushes at 20 ms
    dd.text3D({0, 0, 0}, "hello", debug_color::kCyan, 0.03f);
    EXPECT_TRUE(dd.depthTestedLines().empty()); // nothing visible before the first flush

    dd.flush(0.02f);
    EXPECT_EQ(dd.depthTestedLines().size(), 4u);
    ASSERT_EQ(dd.texts().size(), 1u);
    EXPECT_EQ(dd.texts()[0].text, "hello");
    EXPECT_EQ(dd.texts()[0].color, debug_color::kCyan);

    dd.flush(0.02f); // 0.02 elapsed
    EXPECT_EQ(dd.depthTestedLines().size(), 2u);
    EXPECT_EQ(dd.texts().size(), 1u);
    dd.flush(0.02f); // 0.04 elapsed
    EXPECT_EQ(dd.depthTestedLines().size(), 2u);
    EXPECT_TRUE(dd.texts().empty());
    dd.flush(0.02f); // 0.06 elapsed
    EXPECT_TRUE(dd.depthTestedLines().empty());

    // Zero-duration items survive a zero-dt flush exactly once.
    dd.line({0, 0, 0}, {1, 0, 0});
    dd.flush(0.0f);
    EXPECT_EQ(dd.depthTestedLines().size(), 2u);
    dd.flush(0.0f);
    EXPECT_TRUE(dd.depthTestedLines().empty());
}

TEST(DebugDraw, EnableAndClear) {
    DebugDraw dd;
    dd.setEnabled(false);
    dd.line({0, 0, 0}, {1, 0, 0});
    dd.text3D({0, 0, 0}, "x");
    dd.flush(0.0f);
    EXPECT_TRUE(dd.depthTestedLines().empty());
    EXPECT_TRUE(dd.texts().empty());
    dd.setEnabled(true);
    dd.line({0, 0, 0}, {1, 0, 0}, {}, 10.0f);
    dd.flush(0.0f);
    EXPECT_EQ(dd.depthTestedLines().size(), 2u);
    dd.clear();
    EXPECT_TRUE(dd.depthTestedLines().empty());
    dd.flush(0.0f);
    EXPECT_TRUE(dd.depthTestedLines().empty());
}

TEST(DebugDraw, ConcurrentSubmission) {
    DebugDraw dd;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&dd, t] {
            for (int i = 0; i < kPerThread; ++i) {
                if (i % 2 == 0) {
                    dd.line({0, 0, 0}, {static_cast<f32>(t), static_cast<f32>(i), 0});
                } else {
                    dd.aabb(AABB::fromCenterExtents({0, 0, 0}, glm::vec3{1}), debug_color::kRed, 0.0f, false);
                }
                if (i % 100 == 0) {
                    dd.text3D({0, 0, 0}, "t");
                }
            }
        });
    }
    // Flush concurrently with submission: items land in this flush or the next one, never lost.
    usize depth = 0, overlay = 0, texts = 0;
    for (int f = 0; f < 3; ++f) {
        dd.flush(0.0f);
        depth += dd.depthTestedLines().size();
        overlay += dd.overlayLines().size();
        texts += dd.texts().size();
    }
    for (auto& t : threads) {
        t.join();
    }
    dd.flush(0.0f);
    depth += dd.depthTestedLines().size();
    overlay += dd.overlayLines().size();
    texts += dd.texts().size();
    EXPECT_EQ(depth, static_cast<usize>(kThreads * kPerThread / 2 * 2));
    EXPECT_EQ(overlay, static_cast<usize>(kThreads * kPerThread / 2 * 24));
    EXPECT_EQ(texts, static_cast<usize>(kThreads * 5));
}
