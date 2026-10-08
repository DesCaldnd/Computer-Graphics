#include <oxwald/spline/path_follower.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <string>
#include <vector>

using namespace ox;
using namespace ox::spline;

namespace {

Spline straight(f32 len) {
    Spline s(SplineType::Linear);
    s.addPoint({0, 0, 0});
    s.addPoint({len * 0.1f, 0, 0}); // deliberately uneven spacing
    s.addPoint({len, 0, 0});
    return s;
}

struct Recorder {
    std::vector<std::string> names;
    std::vector<i32> dirs;
    PathFollower::EventCallback callback() {
        return [this](const PathEvent& e) {
            names.emplace_back(e.name);
            dirs.push_back(e.direction);
        };
    }
};

} // namespace

TEST(PathFollower, ConstantSpeedDespiteUnevenSpacing) {
    Spline s(SplineType::CatmullRom);
    s.addPoint({0, 0, 0});
    s.addPoint({0.2f, 0, -0.1f});
    s.addPoint({0.4f, 0, -0.1f});
    s.addPoint({10, 0, -3});
    s.addPoint({20, 0, 0});
    s.addPoint({20.5f, 0, 0.3f});
    FollowerSettings fs;
    fs.speed = 2.0f;
    fs.loopMode = LoopMode::Once;
    PathFollower f(fs);
    const f32 dt = 1.0f / 60.0f;
    FollowerPose prev = f.pose(s);
    int steps = 0;
    while (!f.finished() && steps < 100000) {
        f.advance(s, dt);
        const FollowerPose p = f.pose(s);
        if (!f.finished()) {
            EXPECT_NEAR(p.distance - prev.distance, fs.speed * dt, 1e-4f);
            EXPECT_NEAR(s.tToDistance(p.t), p.distance, 2e-3f);
            // Chord ≈ arc for small steps: world-space speed is constant.
            EXPECT_NEAR(glm::length(p.position - prev.position), fs.speed * dt, 2e-3f) << "step " << steps;
        }
        prev = p;
        ++steps;
    }
    EXPECT_TRUE(f.finished());
    EXPECT_NEAR(f.distance(), s.length(), 1e-5f);
    EXPECT_NEAR(static_cast<f32>(steps) * fs.speed * dt, s.length(), fs.speed * dt + 1e-3f);
}

TEST(PathFollower, OnceStopsAndFiresEnd) {
    const Spline s = straight(10.0f);
    FollowerSettings fs;
    fs.speed = 4.0f;
    fs.loopMode = LoopMode::Once;
    PathFollower f(fs);
    Recorder rec;
    f.setEventCallback(rec.callback());
    f.addEvent("start", 0.0f);
    f.addEvent("mid", 5.0f);
    f.addEvent("end", 10.0f);
    for (int i = 0; i < 10; ++i) {
        f.advance(s, 1.0f);
    }
    EXPECT_TRUE(f.finished());
    EXPECT_FLOAT_EQ(f.distance(), 10.0f);
    EXPECT_EQ(rec.names, (std::vector<std::string>{"start", "mid", "end"}));
}

TEST(PathFollower, LoopWrapsAndFiresEveryLap) {
    Spline s = straight(10.0f);
    s.addMarker("marker", 1.0f); // t=1 → distance 1
    FollowerSettings fs;
    fs.speed = 3.0f;
    fs.loopMode = LoopMode::Loop;
    PathFollower f(fs);
    Recorder rec;
    f.setEventCallback(rec.callback());
    f.addEvent("start", 0.0f);
    f.addEvent("e7", 7.0f);
    // 35 units = 3.5 laps in uneven steps, including one step spanning more than a lap.
    for (f32 dt : {1.0f, 2.5f, 4.0f, 0.1f, 4.0f}) {
        f.advance(s, dt);
    }
    EXPECT_FALSE(f.finished());
    EXPECT_NEAR(f.distance(), 34.8f - 30.0f, 1e-3f);
    int starts = 0, sevens = 0, markers = 0;
    for (const std::string& n : rec.names) {
        starts += n == "start";
        sevens += n == "e7";
        markers += n == "marker";
    }
    EXPECT_EQ(starts, 4); // initial + 3 wraps
    EXPECT_EQ(sevens, 3);
    EXPECT_EQ(markers, 4);

    // Backwards (negative speed) wraps from 0 to the end.
    PathFollower back(FollowerSettings{.speed = -3.0f, .loopMode = LoopMode::Loop});
    back.setDistance(2.0f);
    Recorder r2;
    back.setEventCallback(r2.callback());
    back.addEvent("e7", 7.0f);
    back.advance(s, 2.0f);
    EXPECT_NEAR(back.distance(), 6.0f, 1e-4f);
    EXPECT_EQ(r2.names, (std::vector<std::string>{"marker", "e7"}));
    EXPECT_EQ(r2.dirs, (std::vector<i32>{-1, -1}));
}

TEST(PathFollower, PingPongBothDirections) {
    const Spline s = straight(10.0f);
    FollowerSettings fs;
    fs.speed = 4.0f;
    fs.loopMode = LoopMode::PingPong;
    PathFollower f(fs);
    Recorder rec;
    f.setEventCallback(rec.callback());
    f.addEvent("e3", 3.0f);
    f.addEvent("end", 10.0f);
    f.addEvent("start", 0.0f);
    f.advance(s, 3.0f); // 0 → 10 → 8
    EXPECT_NEAR(f.distance(), 8.0f, 1e-4f);
    EXPECT_EQ(f.direction(), -1);
    f.advance(s, 3.0f); // 8 → 0 → 4
    EXPECT_NEAR(f.distance(), 4.0f, 1e-4f);
    EXPECT_EQ(f.direction(), 1);
    EXPECT_EQ(rec.names, (std::vector<std::string>{"start", "e3", "end", "e3", "start", "e3"}));
    EXPECT_EQ(rec.dirs, (std::vector<i32>{1, 1, 1, -1, -1, 1}));
}

TEST(PathFollower, OrientationFollowsPath) {
    const Spline s = straight(10.0f); // along +X
    PathFollower f;
    f.setDistance(5.0f);
    const FollowerPose p = f.pose(s);
    const glm::vec3 fwd = p.rotation * glm::vec3(0, 0, -1);
    const glm::vec3 up = p.rotation * glm::vec3(0, 1, 0);
    EXPECT_NEAR(glm::dot(fwd, glm::vec3(1, 0, 0)), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::dot(up, glm::vec3(0, 1, 0)), 1.0f, 1e-4f);

    // Moving backwards turns around; a custom +X forward axis maps onto the travel direction.
    FollowerSettings fs;
    fs.speed = -1.0f;
    fs.forwardAxis = {1, 0, 0};
    PathFollower b(fs);
    b.setDistance(5.0f);
    const FollowerPose pb = b.pose(s);
    EXPECT_NEAR(glm::dot(pb.rotation * glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0)), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::dot(pb.rotation * glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)), 1.0f, 1e-4f);
}

TEST(PathFollower, ClosedLoopIsSeamless) {
    Spline s(SplineType::CatmullRom, true);
    s.addPoint({0, 0, 0});
    s.addPoint({5, 1, -5});
    s.addPoint({0, 2, -10});
    s.addPoint({-5, 0, -5});
    FollowerSettings fs;
    fs.speed = 1.0f;
    PathFollower f(fs);
    const f32 dt = 0.05f;
    FollowerPose prev = f.pose(s);
    const int steps = static_cast<int>(2.5f * s.length() / (fs.speed * dt));
    for (int i = 0; i < steps; ++i) {
        f.advance(s, dt);
        const FollowerPose p = f.pose(s);
        EXPECT_NEAR(glm::length(p.position - prev.position), fs.speed * dt, 2e-3f);
        EXPECT_GT(glm::dot(p.rotation * glm::vec3(0, 1, 0), prev.rotation * glm::vec3(0, 1, 0)), 0.99f);
        prev = p;
    }
}
