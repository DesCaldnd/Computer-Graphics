#include <oxwald/world/water.hpp>
#include <oxwald/world/weather.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#if defined(OX_HAS_SHADERC)
#include <shaderc/shaderc.hpp>
#endif

using namespace ox;
using namespace ox::world;

namespace {

GerstnerWaves testWaves() {
    const GerstnerWave w[] = {
        {{1.f, 0.f}, 20.f, 0.5f, 0.6f, 0.f, 1.f},
        {{0.7f, 0.7f}, 9.f, 0.2f, 0.5f, 1.3f, 1.f},
        {{-0.2f, 1.f}, 4.f, 0.08f, 0.4f, 2.1f, 1.f},
    };
    return GerstnerWaves(w, 1.5f);
}

} // namespace

TEST(Gerstner, EulerianHeightInvertsLagrangianDisplacement) {
    const GerstnerWaves waves = testWaves();
    EXPECT_EQ(waves.count(), 3u);
    for (int i = 0; i < 50; ++i) {
        const glm::vec2 x0(f32(i) * 3.7f - 90.f, f32(i) * -1.3f + 20.f);
        const f32 t = f32(i) * 0.37f;
        const glm::vec3 p = waves.positionAt(x0, t);
        EXPECT_NEAR(waves.heightAt({p.x, p.z}, t), p.y, 1e-3f);
        const glm::vec2 back = waves.findRestPoint({p.x, p.z}, t);
        EXPECT_NEAR(glm::distance(back, x0), 0.f, 1e-3f);
    }
}

TEST(Gerstner, NormalMatchesFiniteDifferences) {
    const GerstnerWaves waves = testWaves();
    f32 worst = 0.f;
    for (int i = 0; i < 60; ++i) {
        const glm::vec2 p(f32(i) * 2.3f - 50.f, f32(i) * 1.7f - 30.f);
        const f32 t = 0.5f + f32(i) * 0.1f;
        const f32 e = 0.01f;
        const f32 dx = (waves.heightAt(p + glm::vec2(e, 0.f), t, 12) - waves.heightAt(p - glm::vec2(e, 0.f), t, 12)) / (2.f * e);
        const f32 dz = (waves.heightAt(p + glm::vec2(0.f, e), t, 12) - waves.heightAt(p - glm::vec2(0.f, e), t, 12)) / (2.f * e);
        const glm::vec3 fd = glm::normalize(glm::vec3(-dx, 1.f, -dz));
        const glm::vec3 n = waves.normalAt(p, t, 12);
        worst = std::max(worst, glm::degrees(std::acos(glm::clamp(glm::dot(fd, n), -1.f, 1.f))));
    }
    EXPECT_LT(worst, 0.5f);
}

TEST(Gerstner, SineLimitDispersionAndLayout) {
    const GerstnerWave w{{0.f, 1.f}, 10.f, 0.3f, 0.f, 0.f, 1.f};
    const GerstnerWaves waves(std::span<const GerstnerWave>(&w, 1));
    const f32 k = 6.2831853f / 10.f;
    const f32 omega = std::sqrt(9.81f * k);
    const glm::vec3 d = waves.displacement({3.f, 2.f}, 1.f);
    EXPECT_FLOAT_EQ(d.x, 0.f);
    EXPECT_FLOAT_EQ(d.z, 0.f);
    EXPECT_NEAR(d.y, 0.3f * std::sin(k * 2.f - omega), 1e-5f);
    EXPECT_EQ(sizeof(GerstnerParamsGpu), 528u);
    const GerstnerParamsGpu g = waves.toGpu(2.f);
    EXPECT_EQ(g.info.x, 1.f);
    EXPECT_EQ(g.info.z, 2.f);
    EXPECT_NEAR(g.waves[0].dirK.w, omega, 1e-5f);

    const GerstnerWaves wind = GerstnerWaves::fromWind({1.f, 0.f}, 8.f, 6, 42, 0.6f, 30.f);
    EXPECT_EQ(wind.count(), 6u);
    for (const GerstnerWaveGpu& pw : wind.packed()) {
        EXPECT_GT(pw.dirK.x, std::cos(glm::radians(30.5f)));
        EXPECT_GT(pw.amp.x, 0.f);
    }
}

TEST(Buoyancy, ForceMagnitudeAndDirection) {
    const BuoyancySettings box = BuoyancySettings::fromBox(glm::vec3(0.5f), 4);
    EXPECT_NEAR(box.totalVolume(), 1.f, 1e-5f);
    const WaterHeightFn flat = [](glm::vec2) { return 0.f; };
    const glm::quat id(1.f, 0.f, 0.f, 0.f);
    const BuoyancyResult deep = computeBuoyancy(box, {0.f, -5.f, 0.f}, id, {}, {}, flat);
    EXPECT_NEAR(deep.force.y, 1000.f * 9.81f, 1.f);
    EXPECT_NEAR(deep.force.x, 0.f, 1e-3f);
    EXPECT_NEAR(glm::length(deep.torque), 0.f, 1e-2f);
    EXPECT_NEAR(deep.submergedFraction, 1.f, 1e-5f);
    const BuoyancyResult half = computeBuoyancy(box, {0.f, 0.f, 0.f}, id, {}, {}, flat);
    EXPECT_NEAR(half.submergedFraction, 0.5f, 1e-3f);
    EXPECT_NEAR(half.force.y, 0.5f * 9810.f, 5.f);
    const BuoyancyResult above = computeBuoyancy(box, {0.f, 2.f, 0.f}, id, {}, {}, flat);
    EXPECT_EQ(above.force, glm::vec3(0.f));
    // Drag opposes motion relative to the water.
    const BuoyancyResult sinking = computeBuoyancy(box, {0.f, -5.f, 0.f}, id, {0.f, -2.f, 0.f}, {}, flat);
    EXPECT_GT(sinking.force.y, deep.force.y);
    const BuoyancyResult current = computeBuoyancy(box, {0.f, -5.f, 0.f}, id, {}, {}, flat, {1.f, 0.f, 0.f});
    EXPECT_GT(current.force.x, 0.f); // a current pushes the body along
    const BuoyancyResult spinning = computeBuoyancy(box, {0.f, -5.f, 0.f}, id, {}, {0.f, 3.f, 0.f}, flat);
    EXPECT_LT(spinning.torque.y, 0.f);
}

TEST(Buoyancy, TiltedRaftGetsRestoringTorqueOnWaves) {
    const BuoyancySettings raft = BuoyancySettings::fromBox({2.f, 0.25f, 1.f}, 6);
    const WaterHeightFn flat = [](glm::vec2) { return 0.f; };
    const glm::quat tilt = glm::angleAxis(glm::radians(15.f), glm::vec3(0, 0, 1)); // +X side up
    const BuoyancyResult r = computeBuoyancy(raft, {0.f, 0.f, 0.f}, tilt, {}, {}, flat);
    EXPECT_LT(r.torque.z, 0.f);
    EXPECT_GT(r.force.y, 0.f);
    // Waves as the water surface: the same function the GPU displaces with.
    const GerstnerWaves waves = testWaves();
    const WaterHeightFn sea = [&](glm::vec2 xz) { return waves.heightAt(xz, 3.f); };
    const BuoyancyResult onWaves = computeBuoyancy(raft, {0.f, waves.heightAt({0.f, 0.f}, 3.f), 0.f}, glm::quat(1, 0, 0, 0), {}, {}, sea);
    EXPECT_GT(onWaves.submergedFraction, 0.2f);
    EXPECT_LT(onWaves.submergedFraction, 0.8f);
}

TEST(Wind, GustsTravelDownwindAndAverageToBaseSpeed) {
    WindSettings s;
    s.direction = {3.f, 4.f};
    s.speed = 6.f;
    s.gustStrength = 0.5f;
    const WindField wind(s);
    const glm::vec2 dir(0.6f, 0.8f);
    const glm::vec3 p(12.f, 0.f, -40.f);
    for (f32 t = 0.f; t < 5.f; t += 0.7f) {
        const glm::vec3 q = p + glm::vec3(dir.x, 0.f, dir.y) * (s.speed * 2.f);
        EXPECT_NEAR(wind.gustFactor(p, t), wind.gustFactor(q, t + 2.f), 1e-3f);
    }
    glm::vec3 mean(0.f);
    const int n = 4000;
    for (int i = 0; i < n; ++i) {
        mean += wind.sample(p, f32(i) * 0.137f);
    }
    mean /= f32(n);
    EXPECT_NEAR(glm::length(mean), s.speed, 0.4f);
    EXPECT_GT(glm::dot(glm::normalize(glm::vec2(mean.x, mean.z)), dir), 0.99f);
    EXPECT_EQ(wind.sample(p, 1.f).y, 0.f);
    const WindGpu g = wind.toGpu(7.f);
    EXPECT_EQ(sizeof(g), 32u);
    EXPECT_NEAR(g.dirSpeedTime.x, 0.6f, 1e-6f);
    EXPECT_EQ(g.dirSpeedTime.w, 7.f);
    int lines = 0;
    wind.debugDraw([&](glm::vec3, glm::vec3, glm::vec4) { ++lines; }, glm::vec3(0.f), 50.f, 0.f, 4);
    EXPECT_EQ(lines, 25);
}

TEST(Weather, TransitionsAndAccumulation) {
    WeatherController w(WeatherPreset::clear());
    w.setTarget(WeatherPreset::rainy(), 10.f);
    EXPECT_TRUE(w.transitioning());
    w.update(5.f);
    EXPECT_GT(w.state().current.rain, 0.1f);
    EXPECT_LT(w.state().current.rain, 0.7f);
    w.update(5.f);
    EXPECT_FALSE(w.transitioning());
    EXPECT_FLOAT_EQ(w.state().current.rain, WeatherPreset::rainy().rain);
    for (int i = 0; i < 60; ++i) {
        w.update(1.f);
    }
    EXPECT_GT(w.state().wetness, 0.9f);
    w.setTarget(WeatherPreset::clear(), 0.f);
    w.update(60.f);
    EXPECT_LT(w.state().wetness, 0.9f);
    w.setTarget(WeatherPreset::snowy(), 0.f);
    for (int i = 0; i < 100; ++i) {
        w.update(1.f, -5.f);
    }
    EXPECT_GT(w.state().snowCover, 0.5f);
    const f32 snow = w.state().snowCover;
    w.update(10.f, 10.f);
    EXPECT_LT(w.state().snowCover, snow);
    WindSettings base;
    base.direction = {0.f, 1.f};
    const WindSettings ws = w.wind(base);
    EXPECT_EQ(ws.direction, base.direction);
    EXPECT_EQ(ws.speed, WeatherPreset::snowy().windSpeed);
}

TEST(WorldShaders, IncludesCompile) {
    const std::string root = OX_WORLD_SHADER_DIR;
    std::string code = "#version 460\n";
    for (const char* f : {"world/gerstner.glsl", "world/wind.glsl", "world/preetham.glsl", "world/cdlod.glsl"}) {
        std::ifstream in(root + "/" + f);
        ASSERT_TRUE(in) << f;
        std::stringstream ss;
        ss << in.rdbuf();
        code += ss.str() + "\n";
    }
    code += R"(
layout(local_size_x = 64) in;
layout(std140, binding = 0) uniform Params { OxGerstnerParams gerstner; OxWind wind; OxPreetham sky; };
layout(std430, binding = 1) buffer Out { vec4 results[]; };
void main() {
    vec2 p = vec2(gl_GlobalInvocationID.xy);
    float t = gerstner.info.z;
    vec3 d = oxGerstnerDisplacement(gerstner, p, t);
    vec3 n = oxGerstnerNormal(gerstner, p, t);
    float h = oxGerstnerHeight(gerstner, p, t, 4);
    vec3 w = oxWindSample(wind, vec3(p.x, 0.0, p.y));
    vec3 s = oxPreethamRadiance(sky, normalize(vec3(p.x, 1.0, p.y)));
    vec2 uv = oxCdlodMorphVertex(p / 64.0, 32.0, oxCdlodMorphFactor(length(p), vec4(10.0, 20.0, 0.1, 15.0)));
    results[gl_GlobalInvocationID.x] = vec4(d + n + w + s, h + uv.x);
}
)";
#if defined(OX_HAS_SHADERC)
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    const auto result = compiler.CompileGlslToSpv(code, shaderc_compute_shader, "world_includes.comp", options);
    ASSERT_EQ(result.GetCompilationStatus(), shaderc_compilation_status_success) << result.GetErrorMessage();
#else
    GTEST_SKIP() << "shaderc not available";
#endif
}
