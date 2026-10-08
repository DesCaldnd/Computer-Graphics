#include <oxwald/world/sky.hpp>
#include <oxwald/world/time_of_day.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>
#include <vector>

using namespace ox;
using namespace ox::world;

namespace {

f64 maxElevationOfDay(const GeoLocation& loc, i32 y, i32 m, i32 d) {
    f64 best = -90.0;
    for (f64 h = 0.0; h < 24.0; h += 1.0 / 120.0) {
        best = std::max(best, computeSunPosition(loc, {y, m, d, h}).elevationDeg);
    }
    return best;
}

f32 angleDeg(glm::vec3 a, glm::vec3 b) { return glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.f, 1.f))); }

} // namespace

TEST(Astronomy, JulianDayReferenceValues) {
    EXPECT_DOUBLE_EQ(julianDay({2000, 1, 1, 12.0}), 2451545.0);
    EXPECT_NEAR(julianDay({1957, 10, 4, 0.81 * 24.0}), 2436116.31, 1e-6); // Meeus example 7.a
    // NREL SPA reference: 2003-10-17 12:30:30 local (UTC-7).
    EXPECT_NEAR(julianDay({2003, 10, 17, 19.0 + 30.0 / 60.0 + 30.0 / 3600.0}), 2452930.312847, 1e-6);
    const DateTime d = fromJulianDay(2451545.25);
    EXPECT_EQ(d.year, 2000);
    EXPECT_EQ(d.month, 1);
    EXPECT_EQ(d.day, 1);
    EXPECT_NEAR(d.hours, 18.0, 1e-6);
    const DateTime n = normalized({2023, 12, 31, 25.5});
    EXPECT_EQ(n.year, 2024);
    EXPECT_EQ(n.month, 1);
    EXPECT_EQ(n.day, 1);
    EXPECT_NEAR(n.hours, 1.5, 1e-9);
    const DateTime back = normalized({2024, 3, 1, -1.0});
    EXPECT_EQ(back.month, 2);
    EXPECT_EQ(back.day, 29); // leap year
    EXPECT_EQ(daysInMonth(2023, 2), 28);
    EXPECT_EQ(daysInMonth(2000, 2), 29);
}

TEST(Astronomy, SunMatchesMeeusExample) {
    // Meeus, Astronomical Algorithms, example 25.a / 28.a: 1992 October 13.0.
    const SolarPosition s = computeSunPosition({0.0, 0.0}, {1992, 10, 13, 0.0});
    EXPECT_NEAR(s.declinationDeg, -7.78507, 0.01);
    EXPECT_NEAR(s.rightAscensionDeg, 198.38083, 0.01);
    EXPECT_NEAR(s.equationOfTimeMinutes, 13.7094, 0.05);
    EXPECT_NEAR(s.distance, 0.99766, 1e-3);
}

TEST(Astronomy, SunMatchesNrelSpaReference) {
    // NREL Solar Position Algorithm paper (Reda & Andreas 2004) test case: Golden, CO,
    // 2003-10-17 12:30:30 MST. Topocentric zenith 50.11162°, azimuth 194.34024° (with refraction ~0.013°).
    const GeoLocation golden{39.742476, -105.1786};
    const SolarPosition s = computeSunPosition(golden, {2003, 10, 17, 19.0 + 30.5 / 60.0});
    const f64 apparentElevation = s.elevationDeg + atmosphericRefractionDeg(s.elevationDeg) * (820.0 / 1010.0) * (283.0 / 284.15);
    EXPECT_NEAR(90.0 - apparentElevation, 50.11162, 0.05);
    EXPECT_NEAR(s.azimuthDeg, 194.34024, 0.05);
}

TEST(Astronomy, NoonElevationsAtEquinoxAndSolstice) {
    // Equinox at the equator: the sun passes (almost) through the zenith.
    EXPECT_GT(maxElevationOfDay({0.0, 0.0}, 2024, 3, 20), 89.5);
    // Tropic of Cancer at the June solstice.
    EXPECT_GT(maxElevationOfDay({23.44, 45.0}, 2024, 6, 20), 89.5);
    // Greenwich at the June solstice: 90 - 51.4779 + 23.44.
    EXPECT_NEAR(maxElevationOfDay({51.4779, 0.0}, 2024, 6, 20), 61.96, 0.1);
    // Polar night at the north pole in December.
    EXPECT_LT(maxElevationOfDay({89.9, 0.0}, 2024, 12, 21), 0.0);
    // Morning sun is in the east, evening in the west, noon in the south (northern hemisphere).
    const GeoLocation paris{48.8566, 2.3522};
    EXPECT_NEAR(computeSunPosition(paris, {2024, 3, 20, 6.0}).azimuthDeg, 90.0, 10.0);
    EXPECT_NEAR(computeSunPosition(paris, {2024, 3, 20, 18.0}).azimuthDeg, 270.0, 10.0);
    // World direction convention: south = +Z, east = +X.
    const SolarPosition noon = computeSunPosition(paris, {2024, 3, 20, 11.95});
    const glm::vec3 d = horizontalToWorld(noon.elevationDeg, noon.azimuthDeg);
    EXPECT_GT(d.z, 0.5f);
    EXPECT_NEAR(glm::length(d), 1.f, 1e-5f);
    EXPECT_NEAR(horizontalToWorld(0.0, 90.0).x, 1.f, 1e-6f);
    EXPECT_NEAR(horizontalToWorld(0.0, 0.0).z, -1.f, 1e-6f);
}

TEST(Astronomy, MoonPhaseAtKnownDates) {
    const MoonPhase full = computeMoonPhase({2024, 4, 23, 23.82});
    EXPECT_GT(full.illuminatedFraction, 0.99f);
    const MoonPhase fresh = computeMoonPhase({2024, 4, 8, 18.35});
    EXPECT_LT(fresh.illuminatedFraction, 0.01f);
    const MoonPhase firstQuarter = computeMoonPhase({2024, 4, 15, 19.22});
    EXPECT_NEAR(firstQuarter.illuminatedFraction, 0.5f, 0.05f);
    EXPECT_TRUE(firstQuarter.waxing);
    EXPECT_NEAR(firstQuarter.ageDays, 7.4f, 1.f);
    const MoonPhase lastQuarter = computeMoonPhase({2024, 5, 1, 11.45});
    EXPECT_NEAR(lastQuarter.illuminatedFraction, 0.5f, 0.05f);
    EXPECT_FALSE(lastQuarter.waxing);
}

TEST(Astronomy, MoonCoversSunDuringEclipse) {
    // Total solar eclipse 2024-04-08, Dallas TX, totality ~18:42 UTC.
    const GeoLocation dallas{32.7767, -96.7970};
    const DateTime t{2024, 4, 8, 18.0 + 42.0 / 60.0};
    const SolarPosition sun = computeSunPosition(dallas, t);
    const CelestialPosition moon = computeMoonPosition(dallas, t);
    EXPECT_LT(angleDeg(horizontalToWorld(sun.elevationDeg, sun.azimuthDeg), horizontalToWorld(moon.elevationDeg, moon.azimuthDeg)), 1.f);
    EXPECT_GT(sun.elevationDeg, 50.0);
    EXPECT_NEAR(moon.distance, 56.5, 3.0); // perigee-ish, Earth radii
}

TEST(Astronomy, StarsRotationIsConsistentWithSunAndPole) {
    const GeoLocation loc{48.8566, 2.3522};
    const DateTime t{2024, 6, 1, 15.25};
    const glm::quat q = starsRotation(loc, t);
    // North celestial pole: due north at an elevation equal to the latitude.
    const glm::vec3 pole = q * glm::vec3(0.f, 1.f, 0.f);
    EXPECT_LT(angleDeg(pole, horizontalToWorld(loc.latitudeDeg, 0.0)), 0.01f);
    // The sun's RA/Dec rotated into the world agrees with the NOAA-derived direction.
    const SolarPosition s = computeSunPosition(loc, t);
    const glm::vec3 viaStars = q * celestialDirection(s.rightAscensionDeg, s.declinationDeg);
    EXPECT_LT(angleDeg(viaStars, horizontalToWorld(s.elevationDeg, s.azimuthDeg)), 0.05f);
    // Sidereal rotation: after one sidereal day (23h56m4s) the sky is back in place.
    const glm::quat later = starsRotation(loc, {2024, 6, 1, 15.25 + 23.9344696});
    EXPECT_LT(angleDeg(later * glm::vec3(1, 0, 0), q * glm::vec3(1, 0, 0)), 0.05f);
}

TEST(SkyLight, SunIlluminanceAndColourByElevation) {
    const CelestialLight zenith = sunLight(90.0, 2.5f);
    EXPECT_GT(zenith.illuminance, 90000.f);
    EXPECT_LT(zenith.illuminance, 125000.f);
    f32 prev = zenith.illuminance;
    for (f64 e = 80.0; e >= 0.0; e -= 10.0) {
        const f32 l = sunLight(e).illuminance;
        EXPECT_LT(l, prev);
        prev = l;
    }
    EXPECT_EQ(sunLight(-2.0).illuminance, 0.f);
    const CelestialLight low = sunLight(3.0), high = sunLight(60.0);
    EXPECT_LT(low.color.b / low.color.r, high.color.b / high.color.r); // reddish sunsets
    EXPECT_FLOAT_EQ(std::max({high.color.r, high.color.g, high.color.b}), 1.f);
    EXPECT_GT(relativeAirMass(0.0), 30.0);
    EXPECT_NEAR(relativeAirMass(90.0), 1.0, 1e-3);
    // Hazier air transmits less.
    EXPECT_LT(sunLight(30.0, 8.f).illuminance, sunLight(30.0, 2.f).illuminance);
    // Moonlight: ~0.1-0.3 lux at full moon high in the sky, much dimmer at quarter.
    MoonPhase full;
    full.phaseAngleDeg = 0.f;
    MoonPhase quarter;
    quarter.phaseAngleDeg = 90.f;
    const f32 fullLux = moonLight(60.0, full).illuminance;
    EXPECT_GT(fullLux, 0.1f);
    EXPECT_LT(fullLux, 0.3f);
    EXPECT_LT(moonLight(60.0, quarter).illuminance, fullLux * 0.2f);
}

TEST(SkyLight, PreethamSkyIsPlausible) {
    const glm::vec3 sunDir = glm::normalize(glm::vec3(0.f, 1.f, 1.f)); // 45° elevation, south
    const PreethamSky sky = PreethamSky::compute(sunDir, 2.5f);
    const glm::vec3 zenith = sky.radianceRgb({0.f, 1.f, 0.f});
    EXPECT_GT(zenith.b, zenith.r); // blue sky
    EXPECT_GT(zenith.g, 0.f);
    const glm::vec3 nearSun = sky.radianceRgb(glm::normalize(sunDir + glm::vec3(0.05f, 0.f, 0.f)));
    const glm::vec3 opposite = sky.radianceRgb(glm::normalize(glm::vec3(0.f, 0.5f, -1.f)));
    EXPECT_GT(nearSun.g, opposite.g * 2.f);
    // Zenith luminance from the model matches the analytic zenith value.
    EXPECT_NEAR(sky.luminanceYxy({0.f, 1.f, 0.f}).z, sky.zenith.x, sky.zenith.x * 1e-4f);
    // Order of magnitude: clear sky zenith ~ 2-10 kcd/m².
    EXPECT_GT(sky.zenith.x, 1.f);
    EXPECT_LT(sky.zenith.x, 20.f);
    const auto gpu = sky.toGpu();
    EXPECT_EQ(sizeof(gpu), 128u);
    EXPECT_EQ(gpu.zenith.w, 2.5f);
    // Sun below the horizon is clamped (no NaNs).
    const PreethamSky night = PreethamSky::compute({0.f, -0.5f, 1.f}, 3.f);
    const glm::vec3 r = night.radianceRgb({0.f, 1.f, 0.f});
    EXPECT_TRUE(std::isfinite(r.r) && std::isfinite(r.g) && std::isfinite(r.b));
}

TEST(Curves, InterpolationModesAndWrap) {
    Curve lin({{0.f, 0.f}, {10.f, 10.f}, {20.f, 0.f}});
    EXPECT_FLOAT_EQ(lin.evaluate(5.f), 5.f);
    EXPECT_FLOAT_EQ(lin.evaluate(15.f), 5.f);
    EXPECT_FLOAT_EQ(lin.evaluate(-5.f), 0.f);
    EXPECT_FLOAT_EQ(lin.evaluate(25.f), 0.f);
    Curve step = lin;
    step.interp = CurveInterp::Step;
    EXPECT_FLOAT_EQ(step.evaluate(9.9f), 0.f);
    EXPECT_FLOAT_EQ(step.evaluate(10.f), 10.f);
    Curve smooth = lin;
    smooth.interp = CurveInterp::Smooth;
    EXPECT_FLOAT_EQ(smooth.evaluate(10.f), 10.f); // passes through keys
    EXPECT_NEAR(smooth.evaluate(10.001f), smooth.evaluate(9.999f), 1e-2f);
    // Wrapping over 24 h: night value across midnight.
    Curve night({{6.f, 0.f}, {18.f, 0.f}, {22.f, 1.f}, {2.f, 1.f}}, CurveInterp::Linear, true, 24.f);
    EXPECT_FLOAT_EQ(night.evaluate(0.f), 1.f);
    EXPECT_FLOAT_EQ(night.evaluate(4.f), 0.5f);
    EXPECT_FLOAT_EQ(night.evaluate(20.f), 0.5f);
    EXPECT_FLOAT_EQ(night.evaluate(24.f + 4.f), 0.5f);
    EXPECT_FLOAT_EQ(night.evaluate(-20.f), 0.5f);
    Gradient g({{0.f, {0.f, 0.f, 0.f, 1.f}}, {12.f, {1.f, 1.f, 1.f, 1.f}}}, true, 24.f);
    EXPECT_FLOAT_EQ(g.evaluate(6.f).r, 0.5f);
    EXPECT_FLOAT_EQ(g.evaluate(18.f).r, 0.5f); // wraps back to black at 24
    EXPECT_EQ(Curve().evaluate(3.f), 0.f);
}

TEST(TimeOfDay, EventsOverADayInAmsterdam) {
    TimeOfDaySettings s; // Amsterdam, 2024-06-21, UTC+2
    s.localHours = 0.0;
    s.timeScale = 3600.0; // one game hour per real second
    TimeOfDay tod(s);
    std::vector<std::pair<TimeOfDayEvent, f64>> events;
    tod.addListener([&](TimeOfDayEvent e, const SkyState& st) { events.emplace_back(e, st.local.hours); });
    for (int i = 0; i < 24 * 4; ++i) {
        tod.update(0.25);
    }
    auto find = [&](TimeOfDayEvent e) {
        std::vector<f64> v;
        for (const auto& [ev, h] : events) {
            if (ev == e) {
                v.push_back(h);
            }
        }
        return v;
    };
    const auto rise = find(TimeOfDayEvent::Sunrise), set = find(TimeOfDayEvent::Sunset), noon = find(TimeOfDayEvent::Noon),
               midnight = find(TimeOfDayEvent::Midnight);
    ASSERT_EQ(rise.size(), 1u);
    ASSERT_EQ(set.size(), 1u);
    ASSERT_EQ(noon.size(), 1u);
    ASSERT_EQ(midnight.size(), 1u);
    EXPECT_NEAR(rise[0], 5.0 + 18.0 / 60.0, 0.2);   // 05:18 CEST
    EXPECT_NEAR(set[0], 22.0 + 6.0 / 60.0, 0.2);    // 22:06 CEST
    EXPECT_NEAR(noon[0], 13.0 + 42.0 / 60.0, 0.2);  // 13:42 CEST
    EXPECT_EQ(tod.settings().day, 22);
    EXPECT_NEAR(tod.localHours(), 0.0, 1e-6);
}

TEST(TimeOfDay, MidnightSunHasNoSunset) {
    TimeOfDaySettings s;
    s.location = {69.65, 18.96}; // Tromsø
    s.localHours = 0.0;
    TimeOfDay tod(s);
    int riseOrSet = 0;
    tod.addListener([&](TimeOfDayEvent e, const SkyState&) { riseOrSet += e == TimeOfDayEvent::Sunrise || e == TimeOfDayEvent::Sunset; });
    tod.advanceGameTime(24.0 * 3600.0);
    EXPECT_EQ(riseOrSet, 0);
    EXPECT_TRUE(tod.state().isDay);
}

TEST(TimeOfDay, StateDrivesLightAndCurves) {
    TimeOfDaySettings s;
    s.localHours = 13.7;
    const SkyState day = TimeOfDay::evaluate(s, AtmosphereCurves::defaults());
    EXPECT_TRUE(day.isDay);
    EXPECT_GT(day.sun.elevationDeg, 55.0);
    EXPECT_EQ(day.mainLightDirection, day.sunDirection);
    EXPECT_GT(day.mainLightIlluminance, 80000.f);
    EXPECT_EQ(day.atmosphere.starsIntensity, 0.f);
    EXPECT_NEAR(day.atmosphere.exposureCompensation, 0.f, 1e-4f);
    s.localHours = 1.5;
    const SkyState night = TimeOfDay::evaluate(s, AtmosphereCurves::defaults());
    EXPECT_FALSE(night.isDay);
    EXPECT_LT(night.atmosphere.ambientIntensity, day.atmosphere.ambientIntensity);
    EXPECT_GT(night.atmosphere.exposureCompensation, 1.f);
    // Pause and time scale.
    TimeOfDay tod(s);
    tod.setPaused(true);
    tod.update(100.0);
    EXPECT_NEAR(tod.localHours(), 1.5, 1e-9);
    tod.setPaused(false);
    tod.setTimeScale(60.0);
    tod.update(60.0); // one game hour
    EXPECT_NEAR(tod.localHours(), 2.5, 1e-6);
    // Debug draw emits sun/moon/pole/compass lines.
    int lines = 0;
    tod.debugDraw([&](glm::vec3, glm::vec3, glm::vec4) { ++lines; }, glm::vec3(0.f));
    EXPECT_EQ(lines, 5);
}
