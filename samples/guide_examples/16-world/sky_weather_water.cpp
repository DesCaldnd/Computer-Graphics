// Guide chapter 16 «Открытый мир»: sun/moon, Preetham sky, time of day, wind & weather, Gerstner water.
#include <oxwald/world/time_of_day.hpp>
#include <oxwald/world/water.hpp>
#include <oxwald/world/weather.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/quaternion.hpp>

#include <utility>
#include <vector>

using namespace ox;
using namespace ox::world;

TEST(GuideWorldSky, SunPositionAndLight) {
    // Париж, 21 июня 2024, 10:30 UTC.
    const SolarPosition sun = computeSunPosition({.latitudeDeg = 48.85, .longitudeDeg = 2.35}, {2024, 6, 21, 10.5});
    EXPECT_GT(sun.elevationDeg, 55.0);
    EXPECT_LT(sun.elevationDeg, 65.0);

    // Мировая конвенция: Y вверх, север = −Z, восток = +X; азимут от севера по часовой.
    const glm::vec3 toSun = horizontalToWorld(sun.elevationDeg, sun.azimuthDeg);
    EXPECT_GT(toSun.y, 0.8f);

    const CelestialLight light = sunLight(sun.elevationDeg, /*turbidity*/ 2.5f); // люксы + цвет после атмосферы
    EXPECT_GT(light.illuminance, 80000.f);

    // Небо Preetham: коэффициенты для шейдера world/preetham.glsl (UBO 128 байт).
    const PreethamSky sky = PreethamSky::compute(toSun, 2.5f);
    const glm::vec3 zenith = sky.radianceRgb({0.f, 1.f, 0.f});
    EXPECT_GT(zenith.b, zenith.r) << "днём зенит голубой";
    EXPECT_EQ(sizeof(PreethamSky::Gpu), 128u);

    const MoonPhase phase = computeMoonPhase({2024, 4, 23, 23.8}); // полнолуние 23.04.2024
    EXPECT_GT(phase.illuminatedFraction, 0.98f);
}

TEST(GuideWorldSky, TimeOfDayFiresEvents) {
    TimeOfDaySettings s;
    s.location = {52.37, 4.90}; // Амстердам
    s.year = 2024;
    s.month = 6;
    s.day = 21;
    s.localHours = 0.0;
    s.utcOffsetHours = 2.0; // летнее время, UTC+2
    s.timeScale = 3600.0;   // час игрового времени за секунду реального
    TimeOfDay tod(s);

    std::vector<std::pair<TimeOfDayEvent, f64>> events;
    tod.addListener([&](TimeOfDayEvent e, const SkyState& st) { events.emplace_back(e, st.local.hours); });
    // Туман по высоте солнца: плотный ночью, редкий днём.
    tod.curves().fogDensity = Curve({{-10.f, 0.02f}, {10.f, 0.004f}}, CurveInterp::Smooth);

    for (int i = 0; i < 24 * 4; ++i) {
        tod.update(0.25); // реальные секунды × timeScale
    }
    ASSERT_EQ(events.size(), 4u); // Sunrise, Noon, Sunset, Midnight
    EXPECT_EQ(events[0].first, TimeOfDayEvent::Sunrise);
    EXPECT_NEAR(events[0].second, 5.3, 0.2); // ~05:18 по местному
    EXPECT_EQ(tod.settings().day, 22) << "сутки прошли — дата сменилась";

    // Чистая функция без событий — для превью в редакторе.
    s.localHours = 13.7;
    const SkyState noon = TimeOfDay::evaluate(s, tod.curves());
    EXPECT_TRUE(noon.isDay);
    EXPECT_EQ(noon.mainLightDirection, noon.sunDirection); // днём главный свет — солнце
    EXPECT_NEAR(noon.atmosphere.fogDensity, 0.004f, 1e-4f);
    s.localHours = 1.5;
    const SkyState night = TimeOfDay::evaluate(s, tod.curves());
    EXPECT_FALSE(night.isDay);
    EXPECT_GT(night.atmosphere.fogDensity, noon.atmosphere.fogDensity);
}

TEST(GuideWorldWeather, WindAndWeatherTransitions) {
    WindSettings base;
    base.direction = {1.f, 0.f}; // дует на восток
    base.speed = 6.f;
    base.gustStrength = 0.5f;
    const WindField wind(base);
    const glm::vec3 v = wind.sample({10.f, 0.f, 5.f}, /*time*/ 3.f);
    EXPECT_GT(v.x, 0.f);
    EXPECT_EQ(v.y, 0.f); // ветер горизонтальный
    EXPECT_EQ(sizeof(WindGpu), 32u); // те же формулы в world/wind.glsl

    WeatherController weather(WeatherPreset::clear());
    weather.setTarget(WeatherPreset::rainy(), /*seconds*/ 10.f); // плавный переход (smoothstep)
    weather.update(5.f);
    EXPECT_TRUE(weather.transitioning());
    EXPECT_GT(weather.state().current.rain, 0.1f);
    for (int i = 0; i < 60; ++i) {
        weather.update(1.f, /*°C*/ 15.f);
    }
    EXPECT_FALSE(weather.transitioning());
    EXPECT_GT(weather.state().wetness, 0.9f); // поверхности намокли

    // Скорость/порывы ветра берутся из текущего пресета погоды.
    const WindSettings stormy = weather.wind(base);
    EXPECT_EQ(stormy.direction, base.direction);
    EXPECT_EQ(stormy.speed, WeatherPreset::rainy().windSpeed);
}

TEST(GuideWorldWater, GerstnerWavesAndBuoyancy) {
    GerstnerWaves waves = GerstnerWaves::fromWind({1.f, 0.f}, /*wind m/s*/ 7.f, /*count*/ 8, /*seed*/ 3);
    waves.baseHeight = 2.f;
    const f32 t = 4.f;

    // Высота поверхности в мировой точке (Эйлеров запрос, обращает горизонтальный сдвиг волн).
    const glm::vec3 surface = waves.positionAt({10.f, 5.f}, t);
    EXPECT_NEAR(waves.heightAt({surface.x, surface.z}, t), surface.y, 1e-3f);
    EXPECT_GT(waves.normalAt({0.f, 0.f}, t).y, 0.5f);
    EXPECT_EQ(sizeof(GerstnerParamsGpu), 528u); // UBO для world/gerstner.glsl

    // Плавучесть: тело приближено набором точек; сила Архимеда + сопротивление.
    const BuoyancySettings hull = BuoyancySettings::fromBox({1.5f, 0.4f, 4.f}, 4);
    const glm::vec3 com{0.f, waves.heightAt({0.f, 0.f}, t), 0.f}; // центр масс на уровне воды
    const BuoyancyResult r = computeBuoyancy(hull, com, glm::quat(1, 0, 0, 0), /*linVel*/ {}, /*angVel*/ {},
                                             [&](glm::vec2 xz) { return waves.heightAt(xz, t); });
    EXPECT_GT(r.force.y, 0.f);
    EXPECT_GT(r.submergedFraction, 0.2f);
    EXPECT_LT(r.submergedFraction, 0.8f);
}
