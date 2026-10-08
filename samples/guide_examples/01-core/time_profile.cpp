// Глава 01: время, фиксированный шаг и макросы профилирования (docs/guide/01-core.md).
#include <oxwald/core/profile.hpp>
#include <oxwald/core/time.hpp>

#include <gtest/gtest.h>

namespace {
int simulateStep(double dt) {
    OX_PROFILE_ZONE(); // зона Tracy с именем функции; без Tracy — пустой макрос
    return dt > 0.0 ? 1 : 0;
}
} // namespace

TEST(GuideCoreTime, FixedTimestepAccumulator) {
    ox::FixedTimestep fixed(1.0 / 60.0, /*maxStepsPerFrame*/ 8);
    int steps = 0;
    // Три кадра по 25 мс = 75 мс = 4 шага по 16.67 мс и остаток.
    for (int frame = 0; frame < 3; ++frame) {
        OX_PROFILE_ZONE_N("Frame");
        const ox::u32 n = fixed.advance(0.025);
        for (ox::u32 i = 0; i < n; ++i) steps += simulateStep(fixed.fixedDt());
        OX_PROFILE_PLOT("Fixed steps", static_cast<int64_t>(n));
        OX_PROFILE_FRAME();
    }
    EXPECT_EQ(steps, 4);
    EXPECT_GT(fixed.alpha(), 0.0); // доля шага для интерполяции при отрисовке
    EXPECT_LT(fixed.alpha(), 1.0);

    // Защита от «спирали смерти»: после зависания на 1 с выполнится не больше 8 шагов.
    EXPECT_EQ(fixed.advance(1.0), 8u);
    EXPECT_GT(fixed.droppedTime(), 0.0);
}

TEST(GuideCoreTime, Stopwatch) {
    ox::Stopwatch sw(/*startNow*/ true);
    volatile double x = 0;
    for (int i = 0; i < 1000; ++i) x = x + i;
    sw.stop();
    EXPECT_GE(sw.elapsedMs(), 0.0);
}
