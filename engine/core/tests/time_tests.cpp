#include <oxwald/core/time.hpp>

#include <gtest/gtest.h>

#include <thread>

using namespace ox;

TEST(Time, ClockIsMonotonic) {
    const f64 a = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const f64 b = Clock::now();
    EXPECT_GT(b, a);
    EXPECT_GE(b - a, 0.0015);
}

TEST(Time, Stopwatch) {
    Stopwatch sw;
    EXPECT_FALSE(sw.running());
    EXPECT_EQ(sw.elapsedSeconds(), 0.0);
    sw.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sw.stop();
    const f64 e = sw.elapsedSeconds();
    EXPECT_GE(e, 0.004);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(sw.elapsedSeconds(), e); // stopped: no progress
    EXPECT_NEAR(sw.elapsedMs(), e * 1000.0, 1e-9);
    sw.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_GT(sw.elapsedSeconds(), e); // accumulates
    sw.reset();
    EXPECT_EQ(sw.elapsedSeconds(), 0.0);

    Stopwatch laps(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const f64 l1 = laps.lap();
    const f64 l2 = laps.lap();
    EXPECT_GE(l1, 0.002);
    EXPECT_LT(l2, l1);
}

TEST(Time, FixedTimestepCountsSteps) {
    FixedTimestep fixed(1.0 / 60.0, 8);
    EXPECT_EQ(fixed.advance(1.0 / 120.0), 0u);
    EXPECT_NEAR(fixed.alpha(), 0.5, 1e-9);
    EXPECT_EQ(fixed.advance(1.0 / 120.0), 1u);
    EXPECT_NEAR(fixed.alpha(), 0.0, 1e-6);
    EXPECT_EQ(fixed.advance(3.0 / 60.0), 3u); // exact multiples are not lost to rounding
    EXPECT_EQ(fixed.totalSteps(), 4u);
    EXPECT_NEAR(fixed.simulatedTime(), 4.0 / 60.0, 1e-12);

    // Many small frames: total steps match total time.
    FixedTimestep f2(0.01, 8);
    u64 steps = 0;
    for (int i = 0; i < 1000; ++i) {
        steps += f2.advance(0.0037);
        ASSERT_GE(f2.alpha(), 0.0);
        ASSERT_LT(f2.alpha(), 1.0);
    }
    EXPECT_NEAR(static_cast<f64>(steps), 370.0, 1.0);
    EXPECT_EQ(f2.advance(-1.0), 0u);
    EXPECT_EQ(f2.advance(std::numeric_limits<f64>::quiet_NaN()), 0u);
}

TEST(Time, FixedTimestepSpiralOfDeathGuard) {
    FixedTimestep fixed(0.01, 4);
    EXPECT_EQ(fixed.advance(1.0055), 4u);
    EXPECT_NEAR(fixed.droppedTime(), 0.96, 1e-6);
    EXPECT_NEAR(fixed.accumulator(), 0.0055, 1e-6); // fractional remainder kept
    EXPECT_LT(fixed.alpha(), 1.0);
    EXPECT_EQ(fixed.advance(0.0), 0u);

    fixed.setMaxSteps(100);
    EXPECT_EQ(fixed.advance(0.5), 50u);
    fixed.setFixedDt(0.001);
    EXPECT_LT(fixed.accumulator(), 0.001);
    fixed.reset();
    EXPECT_EQ(fixed.totalSteps(), 0u);
    EXPECT_EQ(fixed.accumulator(), 0.0);
    EXPECT_EQ(fixed.droppedTime(), 0.0);
}

TEST(Time, FrameTimer) {
    FrameTimer timer(0.5, 0.1);
    timer.update(0.02);
    EXPECT_DOUBLE_EQ(timer.smoothedDelta(), 0.02);
    timer.update(0.04);
    EXPECT_DOUBLE_EQ(timer.smoothedDelta(), 0.03);
    EXPECT_NEAR(timer.fps(), 1.0 / 0.03, 1e-9);
    timer.update(5.0); // hitch clamped
    EXPECT_DOUBLE_EQ(timer.deltaSeconds(), 0.1);
    EXPECT_DOUBLE_EQ(timer.rawDeltaSeconds(), 5.0);
    EXPECT_EQ(timer.frameCount(), 3u);
    EXPECT_NEAR(timer.totalTime(), 0.16, 1e-12);

    FrameTimer live;
    EXPECT_EQ(live.tick(), 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_GT(live.tick(), 0.0);
    live.reset();
    EXPECT_EQ(live.frameCount(), 0u);
}
