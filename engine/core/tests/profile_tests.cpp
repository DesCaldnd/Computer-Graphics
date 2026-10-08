#include <oxwald/core/profile.hpp>

#include <gtest/gtest.h>

#include <cstdlib>

namespace {

int profiledWork(int n) {
    OX_PROFILE_ZONE();
    int sum = 0;
    for (int i = 0; i < n; ++i) {
        OX_PROFILE_ZONE_N("inner");
        sum += i;
    }
    return sum;
}

} // namespace

TEST(Profile, MacrosCompileAndAreHarmless) {
    OX_PROFILE_THREAD_NAME("test-main");
    {
        OX_PROFILE_ZONE_C("colored", 0xff8800);
        EXPECT_EQ(profiledWork(10), 45);
    }
    // Variables used only by the macros must not trigger unused warnings in the no-op build.
    const double plotValue = 3.5;
    OX_PROFILE_PLOT("value", plotValue);
    const char message[] = "hello";
    OX_PROFILE_MESSAGE(message, sizeof(message) - 1);
    void* p = std::malloc(64);
    const std::size_t size = 64;
    OX_PROFILE_ALLOC(p, size);
    OX_PROFILE_FREE(p);
    std::free(p);
    OX_PROFILE_FRAME();
    OX_PROFILE_FRAME_N("secondary");
}
