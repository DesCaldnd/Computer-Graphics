#include <oxwald/world/sky.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>

namespace ox::world {

namespace {

constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kDeg = kPi / 180.0;
inline f64 sind(f64 d) { return std::sin(d * kDeg); }
inline f64 cosd(f64 d) { return std::cos(d * kDeg); }
inline f64 tand(f64 d) { return std::tan(d * kDeg); }
inline f64 atan2d(f64 y, f64 x) { return std::atan2(y, x) / kDeg; }
inline f64 asind(f64 x) { return std::asin(std::clamp(x, -1.0, 1.0)) / kDeg; }
inline f64 wrap360(f64 a) {
    a = std::fmod(a, 360.0);
    return a < 0.0 ? a + 360.0 : a;
}
inline f64 wrap180(f64 a) {
    a = wrap360(a);
    return a > 180.0 ? a - 360.0 : a;
}

struct SunEcliptic {
    f64 appLongDeg, raDeg, declDeg, eqTimeMin, distanceAu;
};

SunEcliptic sunCoordinates(f64 jd) {
    const f64 T = (jd - 2451545.0) / 36525.0;
    const f64 L0 = wrap360(280.46646 + T * (36000.76983 + T * 0.0003032));
    const f64 M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    const f64 e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
    const f64 C = sind(M) * (1.914602 - T * (0.004817 + 0.000014 * T)) + sind(2 * M) * (0.019993 - 0.000101 * T) +
                  sind(3 * M) * 0.000289;
    const f64 trueLong = L0 + C;
    const f64 trueAnom = M + C;
    const f64 omega = 125.04 - 1934.136 * T;
    const f64 appLong = trueLong - 0.00569 - 0.00478 * sind(omega);
    const f64 meanObliq = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
    const f64 obliq = meanObliq + 0.00256 * cosd(omega);
    SunEcliptic s{};
    s.appLongDeg = wrap360(appLong);
    s.raDeg = wrap360(atan2d(cosd(obliq) * sind(appLong), cosd(appLong)));
    s.declDeg = asind(sind(obliq) * sind(appLong));
    const f64 y = tand(obliq / 2) * tand(obliq / 2);
    const f64 eq = y * sind(2 * L0) - 2 * e * sind(M) + 4 * e * y * sind(M) * cosd(2 * L0) -
                   0.5 * y * y * sind(4 * L0) - 1.25 * e * e * sind(2 * M);
    s.eqTimeMin = 4.0 * eq / kDeg;
    s.distanceAu = (1.000001018 * (1 - e * e)) / (1 + e * cosd(trueAnom));
    return s;
}

struct MoonEcliptic {
    f64 lonDeg, latDeg, distanceEr, raDeg, declDeg;
};

MoonEcliptic moonCoordinates(f64 jd) {
    const f64 d = jd - 2451543.5;
    const f64 N = wrap360(125.1228 - 0.0529538083 * d);
    const f64 i = 5.1454;
    const f64 w = wrap360(318.0634 + 0.1643573223 * d);
    const f64 a = 60.2666;
    const f64 e = 0.054900;
    const f64 M = wrap360(115.3654 + 13.0649929509 * d);
    f64 E = M + (e / kDeg) * sind(M) * (1.0 + e * cosd(M));
    for (int it = 0; it < 5; ++it) {
        E = E - (E - (e / kDeg) * sind(E) - M) / (1.0 - e * cosd(E));
    }
    const f64 xv = a * (cosd(E) - e), yv = a * std::sqrt(1.0 - e * e) * sind(E);
    const f64 v = atan2d(yv, xv);
    f64 r = std::sqrt(xv * xv + yv * yv);
    const f64 xh = r * (cosd(N) * cosd(v + w) - sind(N) * sind(v + w) * cosd(i));
    const f64 yh = r * (sind(N) * cosd(v + w) + cosd(N) * sind(v + w) * cosd(i));
    const f64 zh = r * (sind(v + w) * sind(i));
    f64 lon = atan2d(yh, xh), lat = atan2d(zh, std::sqrt(xh * xh + yh * yh));
    const f64 Ms = wrap360(356.0470 + 0.9856002585 * d);
    const f64 ws = 282.9404 + 4.70935e-5 * d;
    const f64 Ls = Ms + ws, Lm = N + w + M, Mm = M;
    const f64 D = Lm - Ls, F = Lm - N;
    lon += -1.274 * sind(Mm - 2 * D) + 0.658 * sind(2 * D) - 0.186 * sind(Ms) - 0.059 * sind(2 * Mm - 2 * D) -
           0.057 * sind(Mm - 2 * D + Ms) + 0.053 * sind(Mm + 2 * D) + 0.046 * sind(2 * D - Ms) + 0.041 * sind(Mm - Ms) -
           0.035 * sind(D) - 0.031 * sind(Mm + Ms) - 0.015 * sind(2 * F - 2 * D) + 0.011 * sind(Mm - 4 * D);
    lat += -0.173 * sind(F - 2 * D) - 0.055 * sind(Mm - F - 2 * D) - 0.046 * sind(Mm + F - 2 * D) + 0.033 * sind(F + 2 * D) +
           0.017 * sind(2 * Mm + F);
    r += -0.58 * cosd(Mm - 2 * D) - 0.46 * cosd(2 * D);
    const f64 ecl = 23.4393 - 3.563e-7 * d;
    const f64 xe = cosd(lat) * cosd(lon);
    const f64 ye = cosd(ecl) * cosd(lat) * sind(lon) - sind(ecl) * sind(lat);
    const f64 ze = sind(ecl) * cosd(lat) * sind(lon) + cosd(ecl) * sind(lat);
    return {wrap360(lon), lat, r, wrap360(atan2d(ye, xe)), atan2d(ze, std::sqrt(xe * xe + ye * ye))};
}

} // namespace

f64 julianDay(const DateTime& t) {
    i32 y = t.year, m = t.month;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    const i32 A = i32(std::floor(y / 100.0));
    const i32 B = 2 - A + i32(std::floor(A / 4.0));
    return std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + t.day + B - 1524.5 + t.hours / 24.0;
}

DateTime fromJulianDay(f64 jd) {
    const f64 z = std::floor(jd + 0.5);
    const f64 f = jd + 0.5 - z;
    f64 A = z;
    if (z >= 2299161.0) {
        const f64 alpha = std::floor((z - 1867216.25) / 36524.25);
        A = z + 1 + alpha - std::floor(alpha / 4.0);
    }
    const f64 B = A + 1524.0;
    const f64 C = std::floor((B - 122.1) / 365.25);
    const f64 D = std::floor(365.25 * C);
    const f64 E = std::floor((B - D) / 30.6001);
    DateTime r;
    r.day = i32(B - D - std::floor(30.6001 * E));
    r.month = i32(E < 14 ? E - 1 : E - 13);
    r.year = i32(r.month > 2 ? C - 4716 : C - 4715);
    r.hours = f * 24.0;
    return r;
}

DateTime normalized(const DateTime& t) {
    const f64 days = std::floor(t.hours / 24.0);
    const f64 hours = t.hours - days * 24.0;
    DateTime r = fromJulianDay(julianDay({t.year, t.month, t.day, 0.0}) + days);
    r.hours = hours;
    return r;
}

i32 daysInMonth(i32 year, i32 month) {
    static constexpr i32 kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[std::clamp(month, 1, 12) - 1];
}

f64 greenwichMeanSiderealTimeDeg(f64 jd) {
    const f64 T = (jd - 2451545.0) / 36525.0;
    return wrap360(280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * T * T - T * T * T / 38710000.0);
}

void equatorialToHorizontal(f64 ra, f64 dec, f64 lat, f64 lst, f64& elev, f64& az) {
    const f64 H = lst - ra;
    elev = asind(sind(lat) * sind(dec) + cosd(lat) * cosd(dec) * cosd(H));
    az = wrap360(atan2d(-sind(H) * cosd(dec), sind(dec) * cosd(lat) - cosd(dec) * cosd(H) * sind(lat)));
}

glm::vec3 horizontalToWorld(f64 elev, f64 az) {
    return glm::vec3(f32(cosd(elev) * sind(az)), f32(sind(elev)), f32(-cosd(elev) * cosd(az)));
}

f64 atmosphericRefractionDeg(f64 h) {
    if (h < -1.0) {
        return 0.0;
    }
    return 1.02 / tand(h + 10.3 / (h + 5.11)) / 60.0;
}

SolarPosition computeSunPosition(const GeoLocation& loc, const DateTime& utcIn) {
    const DateTime utc = normalized(utcIn);
    const f64 jd = julianDay(utc);
    const SunEcliptic s = sunCoordinates(jd);
    SolarPosition p;
    p.rightAscensionDeg = s.raDeg;
    p.declinationDeg = s.declDeg;
    p.eclipticLongitudeDeg = s.appLongDeg;
    p.distance = s.distanceAu;
    p.equationOfTimeMinutes = s.eqTimeMin;
    const f64 trueSolarMin = std::fmod(utc.hours * 60.0 + s.eqTimeMin + 4.0 * loc.longitudeDeg + 1440.0 * 4, 1440.0);
    p.hourAngleDeg = wrap180(trueSolarMin / 4.0 - 180.0);
    const f64 lat = loc.latitudeDeg, dec = s.declDeg, H = p.hourAngleDeg;
    p.elevationDeg = asind(sind(lat) * sind(dec) + cosd(lat) * cosd(dec) * cosd(H));
    p.azimuthDeg = wrap360(atan2d(-sind(H) * cosd(dec), sind(dec) * cosd(lat) - cosd(dec) * cosd(H) * sind(lat)));
    return p;
}

CelestialPosition computeMoonPosition(const GeoLocation& loc, const DateTime& utcIn) {
    const DateTime utc = normalized(utcIn);
    const f64 jd = julianDay(utc);
    const MoonEcliptic m = moonCoordinates(jd);
    CelestialPosition p;
    p.rightAscensionDeg = m.raDeg;
    p.declinationDeg = m.declDeg;
    p.eclipticLongitudeDeg = m.lonDeg;
    p.distance = m.distanceEr;
    const f64 lst = greenwichMeanSiderealTimeDeg(jd) + loc.longitudeDeg;
    p.hourAngleDeg = wrap180(lst - m.raDeg);
    f64 elev, az;
    equatorialToHorizontal(m.raDeg, m.declDeg, loc.latitudeDeg, lst, elev, az);
    // Topocentric parallax: the moon is ~60 Earth radii away, so observers see it up to ~1° lower.
    const f64 parallax = asind(1.0 / m.distanceEr);
    p.elevationDeg = elev - parallax * cosd(elev);
    p.azimuthDeg = az;
    return p;
}

MoonPhase computeMoonPhase(const DateTime& utcIn) {
    const f64 jd = julianDay(normalized(utcIn));
    const SunEcliptic s = sunCoordinates(jd);
    const MoonEcliptic m = moonCoordinates(jd);
    const f64 cosPsi = cosd(m.latDeg) * cosd(m.lonDeg - s.appLongDeg);
    const f64 psi = std::acos(std::clamp(cosPsi, -1.0, 1.0)) / kDeg;
    // Phase angle (Meeus 48.3) with sun distance in Earth radii.
    const f64 R = s.distanceAu * 23454.8;
    const f64 i = atan2d(R * sind(psi), m.distanceEr - R * cosd(psi));
    MoonPhase ph;
    ph.elongationDeg = f32(psi);
    ph.phaseAngleDeg = f32(i);
    ph.illuminatedFraction = f32((1.0 + cosd(i)) * 0.5);
    const f64 dl = wrap360(m.lonDeg - s.appLongDeg);
    ph.waxing = dl < 180.0;
    ph.ageDays = f32(dl / 12.1907);
    return ph;
}

glm::vec3 celestialDirection(f64 ra, f64 dec) {
    return glm::vec3(f32(cosd(dec) * cosd(ra)), f32(sind(dec)), f32(-cosd(dec) * sind(ra)));
}

glm::quat starsRotation(const GeoLocation& loc, const DateTime& utc) {
    const f64 lst = greenwichMeanSiderealTimeDeg(julianDay(normalized(utc))) + loc.longitudeDeg;
    auto world = [&](f64 ra, f64 dec) {
        f64 e, a;
        equatorialToHorizontal(ra, dec, loc.latitudeDeg, lst, e, a);
        return horizontalToWorld(e, a);
    };
    // Columns: images of the celestial basis vectors (+X: RA 0; +Y: pole; +Z: RA 270°).
    glm::mat3 m(world(0.0, 0.0), world(0.0, 90.0), world(270.0, 0.0));
    // Re-orthonormalise (float rounding) before converting to a quaternion.
    m[0] = glm::normalize(m[0]);
    m[2] = glm::normalize(glm::cross(m[0], m[1]));
    m[1] = glm::cross(m[2], m[0]);
    return glm::normalize(glm::quat_cast(m));
}

// --- light ----------------------------------------------------------------------------------

f64 relativeAirMass(f64 elevationDeg) {
    const f64 z = 90.0 - std::max(elevationDeg, -0.5);
    return 1.0 / (cosd(z) + 0.50572 * std::pow(std::max(96.07995 - z, 0.5), -1.6364));
}

glm::vec3 atmosphereTransmittance(f64 elevationDeg, f32 turbidity) {
    const f64 m = relativeAirMass(elevationDeg);
    const f64 lambda[3] = {0.680, 0.550, 0.440}; // µm, R G B
    const f64 beta = std::max(0.0, 0.04608 * turbidity - 0.04586); // Ångström turbidity coefficient
    glm::vec3 t;
    for (int c = 0; c < 3; ++c) {
        const f64 tauR = 0.008735 * std::pow(lambda[c], -4.08);
        const f64 tauA = beta * std::pow(lambda[c], -1.3);
        t[c] = f32(std::exp(-m * (tauR + tauA)));
    }
    return t;
}

namespace {
CelestialLight lightFrom(f64 elevationDeg, f32 turbidity, f64 topOfAtmosphereLux) {
    CelestialLight l;
    l.transmittance = atmosphereTransmittance(elevationDeg, turbidity);
    const f32 mx = std::max({l.transmittance.r, l.transmittance.g, l.transmittance.b, 1e-6f});
    l.color = l.transmittance / mx;
    const f32 lum = glm::dot(l.transmittance, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    // Disc sinks below the horizon over ~1° (radius 0.27° + refraction 0.57°).
    const f64 visible = std::clamp((elevationDeg + 0.833) / 1.0, 0.0, 1.0);
    l.illuminance = f32(topOfAtmosphereLux * lum * visible);
    return l;
}
} // namespace

CelestialLight sunLight(f64 elevationDeg, f32 turbidity) { return lightFrom(elevationDeg, turbidity, 128000.0); }

CelestialLight moonLight(f64 elevationDeg, const MoonPhase& phase, f32 turbidity) {
    // Allen's lunar magnitude vs phase angle; full moon ≈ 0.27 lux outside the atmosphere.
    const f64 i = std::abs(f64(phase.phaseAngleDeg));
    const f64 mag = 0.026 * i + 4e-9 * i * i * i * i;
    CelestialLight l = lightFrom(elevationDeg, turbidity, 0.27 * std::pow(10.0, -0.4 * mag));
    return l;
}

// --- Preetham ---------------------------------------------------------------------------------

glm::vec3 xyYToLinearSrgb(glm::vec3 c) {
    const f32 x = c.x, y = std::max(c.y, 1e-6f), Y = c.z;
    const f32 X = x / y * Y, Z = (1.f - x - y) / y * Y;
    return {3.2406f * X - 1.5372f * Y - 0.4986f * Z, -0.9689f * X + 1.8758f * Y + 0.0415f * Z,
            0.0557f * X - 0.2040f * Y + 1.0570f * Z};
}

namespace {
inline f32 perez(f32 cosTheta, f32 gamma, f32 cosGamma, f32 A, f32 B, f32 C, f32 D, f32 E) {
    return (1.f + A * std::exp(B / std::max(cosTheta, 0.01f))) * (1.f + C * std::exp(D * gamma) + E * cosGamma * cosGamma);
}
} // namespace

PreethamSky PreethamSky::compute(glm::vec3 sunDir, f32 T) {
    PreethamSky s;
    s.turbidity = T;
    glm::vec3 d = glm::normalize(sunDir);
    // The model is only valid for a sun above the horizon; clamp to ~1° elevation.
    if (d.y < 0.0175f) {
        const glm::vec2 h = glm::length(glm::vec2(d.x, d.z)) > 1e-6f ? glm::normalize(glm::vec2(d.x, d.z)) : glm::vec2(0.f, -1.f);
        d = glm::vec3(h.x * 0.99985f, 0.0175f, h.y * 0.99985f);
    }
    s.sunDirection = d;
    s.A = {0.1787f * T - 1.4630f, -0.0193f * T - 0.2592f, -0.0167f * T - 0.2608f};
    s.B = {-0.3554f * T + 0.4275f, -0.0665f * T + 0.0008f, -0.0950f * T + 0.0092f};
    s.C = {-0.0227f * T + 5.3251f, -0.0004f * T + 0.2125f, -0.0079f * T + 0.2102f};
    s.D = {0.1206f * T - 2.5771f, -0.0641f * T - 0.8989f, -0.0441f * T - 1.6537f};
    s.E = {-0.0670f * T + 0.3703f, -0.0033f * T + 0.0452f, -0.0109f * T + 0.0529f};
    const f32 ts = std::acos(glm::clamp(d.y, -1.f, 1.f));
    const f32 ts2 = ts * ts, ts3 = ts2 * ts, T2 = T * T;
    const f32 chi = (4.f / 9.f - T / 120.f) * (f32(kPi) - 2.f * ts);
    const f32 Yz = (4.0453f * T - 4.9710f) * std::tan(chi) - 0.2155f * T + 2.4192f;
    const f32 xz = T2 * (0.00166f * ts3 - 0.00375f * ts2 + 0.00209f * ts) +
                   T * (-0.02903f * ts3 + 0.06377f * ts2 - 0.03202f * ts + 0.00394f) +
                   (0.11693f * ts3 - 0.21196f * ts2 + 0.06052f * ts + 0.25886f);
    const f32 yz = T2 * (0.00275f * ts3 - 0.00610f * ts2 + 0.00317f * ts) +
                   T * (-0.04214f * ts3 + 0.08970f * ts2 - 0.04153f * ts + 0.00516f) +
                   (0.15346f * ts3 - 0.26756f * ts2 + 0.06670f * ts + 0.26688f);
    s.zenith = {std::max(Yz, 0.f), xz, yz};
    const f32 cts = std::cos(ts);
    for (int c = 0; c < 3; ++c) {
        s.normalization[c] = 1.f / perez(1.f, ts, cts, s.A[c], s.B[c], s.C[c], s.D[c], s.E[c]);
    }
    return s;
}

glm::vec3 PreethamSky::luminanceYxy(glm::vec3 v) const {
    v = glm::normalize(v);
    const f32 cosTheta = std::max(v.y, 0.001f); // horizon/below: clamp
    const f32 cosGamma = glm::clamp(glm::dot(v, sunDirection), -1.f, 1.f);
    const f32 gamma = std::acos(cosGamma);
    glm::vec3 r;
    for (int c = 0; c < 3; ++c) {
        r[c] = zenith[c] * perez(cosTheta, gamma, cosGamma, A[c], B[c], C[c], D[c], E[c]) * normalization[c];
    }
    // Stored as (Y, x, y) → return (x, y, Y) for xyYToLinearSrgb.
    return {r.y, r.z, r.x};
}

glm::vec3 PreethamSky::radianceRgb(glm::vec3 v) const {
    return glm::max(xyYToLinearSrgb(luminanceYxy(v)) * 1000.f, glm::vec3(0.f));
}

PreethamSky::Gpu PreethamSky::toGpu() const {
    return {glm::vec4(A, 0.f),          glm::vec4(B, 0.f), glm::vec4(C, 0.f), glm::vec4(D, 0.f), glm::vec4(E, 0.f),
            glm::vec4(zenith, turbidity), glm::vec4(normalization, 0.f), glm::vec4(sunDirection, 0.f)};
}

} // namespace ox::world
