#pragma once

// Astronomy (sun/moon/stars) and analytic sky parameters.
// World convention: +Y up, north = -Z, east = +X (so south = +Z, west = -X).
// Azimuth is measured from north, clockwise (towards east), in degrees.

#include <oxwald/core/types.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace ox::world {

struct GeoLocation {
    f64 latitudeDeg = 0.0;  // +north
    f64 longitudeDeg = 0.0; // +east
};

// Civil date + time of day in UTC (hours may be fractional and outside [0,24) — it is normalised).
struct DateTime {
    i32 year = 2000;
    i32 month = 1; // 1..12
    i32 day = 1;   // 1..31
    f64 hours = 12.0;
};

f64 julianDay(const DateTime& utc);
DateTime fromJulianDay(f64 jd);
DateTime normalized(const DateTime& t);   // carries hours outside [0,24) into the date
i32 daysInMonth(i32 year, i32 month);
f64 greenwichMeanSiderealTimeDeg(f64 jd); // [0,360)

struct CelestialPosition {
    f64 elevationDeg = 0.0;      // geometric (no refraction); topocentric for the moon
    f64 azimuthDeg = 0.0;        // from north, clockwise
    f64 rightAscensionDeg = 0.0; // apparent, equinox of date
    f64 declinationDeg = 0.0;
    f64 hourAngleDeg = 0.0;      // local, (-180, 180]
    f64 eclipticLongitudeDeg = 0.0;
    f64 distance = 0.0;          // sun: AU, moon: Earth radii
};

struct SolarPosition : CelestialPosition {
    f64 equationOfTimeMinutes = 0.0;
};

// NOAA solar calculator algorithm (Meeus, "Astronomical Algorithms" ch. 25 low-precision); ~0.01°.
SolarPosition computeSunPosition(const GeoLocation& loc, const DateTime& utc);
// Low-precision lunar theory (P. Schlyter) with the main perturbations + topocentric parallax; ~0.3°.
CelestialPosition computeMoonPosition(const GeoLocation& loc, const DateTime& utc);

// Equatorial (RA/Dec, degrees) → horizontal for a given local sidereal time.
void equatorialToHorizontal(f64 raDeg, f64 decDeg, f64 latitudeDeg, f64 lstDeg, f64& elevationDeg, f64& azimuthDeg);
// Unit vector pointing to the object in world space.
glm::vec3 horizontalToWorld(f64 elevationDeg, f64 azimuthDeg);
// Standard atmospheric refraction (Saemundsson) to add to a geometric elevation, degrees.
f64 atmosphericRefractionDeg(f64 geometricElevationDeg);

struct MoonPhase {
    f32 illuminatedFraction = 0.f; // 0 new .. 1 full
    f32 phaseAngleDeg = 180.f;     // sun-moon-earth angle, 0 = full
    f32 elongationDeg = 0.f;       // sun-earth-moon angle
    bool waxing = true;
    f32 ageDays = 0.f;             // approx days since new moon (elongation / 12.19°)
};
MoonPhase computeMoonPhase(const DateTime& utc);

// Rotation taking celestial directions to world space. Celestial frame: +X = vernal equinox (RA 0,
// Dec 0), +Y = north celestial pole, +Z = RA 18h (right-handed). For a star with (RA α, Dec δ):
// world = starsRotation * vec3(cosδ cosα, sinδ, -cosδ sinα). Use it to rotate the star cubemap.
glm::quat starsRotation(const GeoLocation& loc, const DateTime& utc);
glm::vec3 celestialDirection(f64 raDeg, f64 decDeg);

// --- light from sun / moon ------------------------------------------------------------------
struct CelestialLight {
    glm::vec3 color{1.f};   // linear sRGB, max component = 1 (chromaticity after the atmosphere)
    f32 illuminance = 0.f;  // lux on a surface perpendicular to the light (at sea level)
    glm::vec3 transmittance{1.f}; // atmosphere transmittance per RGB channel
};
f64 relativeAirMass(f64 elevationDeg); // Kasten & Young 1989
// Simple atmospheric transmittance: Rayleigh + Ångström aerosol (turbidity ~2 clear .. 10 hazy).
glm::vec3 atmosphereTransmittance(f64 elevationDeg, f32 turbidity);
CelestialLight sunLight(f64 elevationDeg, f32 turbidity = 2.5f);
CelestialLight moonLight(f64 elevationDeg, const MoonPhase& phase, f32 turbidity = 2.5f);

// --- Preetham analytic daylight (Preetham, Shirley, Smits 1999) -----------------------------
// GPU: upload PreethamSky::Gpu in a UBO and evaluate engine/shaders/world/preetham.glsl
// (Y = Yz * F(θ,γ) / F(0,θs) per channel in xyY, then XYZ → linear sRGB; scale to cd/m²).
// For night/twilight blend towards an ambient night colour driven by TimeOfDay curves.
struct PreethamSky {
    f32 turbidity = 2.5f;
    glm::vec3 sunDirection{0.f, 1.f, 0.f}; // clamped to >= ~1° above the horizon for the model
    glm::vec3 A{0.f}, B{0.f}, C{0.f}, D{0.f}, E{0.f}; // Perez coefficients for (Y, x, y)
    glm::vec3 zenith{0.f};        // (Y [kcd/m²], x, y) at the zenith
    glm::vec3 normalization{1.f}; // 1 / F(0, θs) per channel

    static PreethamSky compute(glm::vec3 sunDirection, f32 turbidity);
    [[nodiscard]] glm::vec3 luminanceYxy(glm::vec3 viewDirection) const; // returns (x, y, Y), Y in kcd/m²
    [[nodiscard]] glm::vec3 radianceRgb(glm::vec3 viewDirection) const;  // linear sRGB, cd/m²

    struct Gpu { // std140-friendly, 128 bytes
        glm::vec4 A, B, C, D, E;  // xyz = (Y, x, y) coefficients
        glm::vec4 zenith;         // xyz = zenith Yxy, w = turbidity
        glm::vec4 normalization;  // xyz = 1/F(0,θs)
        glm::vec4 sunDirection;   // xyz, w = unused
    };
    [[nodiscard]] Gpu toGpu() const;
};
glm::vec3 xyYToLinearSrgb(glm::vec3 xyY);

} // namespace ox::world
