#include <oxwald/world/time_of_day.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace ox::world {

namespace {

template <class Key>
void insertSorted(std::vector<Key>& keys, Key k) {
    auto it = std::upper_bound(keys.begin(), keys.end(), k.time, [](f32 t, const Key& a) { return t < a.time; });
    keys.insert(it, k);
}

// Segment lookup shared by Curve and Gradient. Returns false when t is outside a non-wrapping curve
// (then `clampIndex` holds the end key to return).
struct Segment {
    i32 i = 0;  // key index of the segment start (may need wrapping)
    f32 t = 0.f; // query time mapped into the key time domain
};

template <class Key>
Segment findSegment(const std::vector<Key>& keys, f32 t, bool wrap, f32 period) {
    const i32 n = i32(keys.size());
    if (wrap) {
        const f32 t0 = keys.front().time;
        t = t0 + std::fmod(std::fmod(t - t0, period) + period, period);
    }
    i32 i = 0;
    while (i + 1 < n && keys[usize(i + 1)].time <= t) {
        ++i;
    }
    return {i, t};
}

} // namespace

Curve::Curve(std::initializer_list<CurveKey> keys, CurveInterp in, bool w, f32 p) : interp(in), wrap(w), period(p) {
    for (const auto& k : keys) {
        addKey(k.time, k.value);
    }
}

void Curve::addKey(f32 time, f32 value) { insertSorted(m_keys, CurveKey{time, value}); }

f32 Curve::evaluate(f32 t) const {
    const i32 n = i32(m_keys.size());
    if (n == 0) {
        return 0.f;
    }
    if (n == 1) {
        return m_keys[0].value;
    }
    if (!wrap) {
        if (t <= m_keys.front().time) {
            return m_keys.front().value;
        }
        if (t >= m_keys.back().time) {
            return m_keys.back().value;
        }
    }
    const Segment s = findSegment(m_keys, t, wrap, period);
    // Virtual key access: wraps with period offsets, or clamps (extrapolating time by 1).
    auto key = [&](i32 i) -> CurveKey {
        if (wrap) {
            const i32 m = ((i % n) + n) % n;
            const i32 cycles = (i - m) / n;
            return {m_keys[usize(m)].time + f32(cycles) * period, m_keys[usize(m)].value};
        }
        if (i < 0) {
            return {m_keys[0].time - 1.f, m_keys[0].value};
        }
        if (i >= n) {
            return {m_keys[usize(n - 1)].time + 1.f, m_keys[usize(n - 1)].value};
        }
        return m_keys[usize(i)];
    };
    const CurveKey k1 = key(s.i), k2 = key(s.i + 1);
    const f32 span = k2.time - k1.time;
    if (span <= 1e-6f || interp == CurveInterp::Step) {
        return k1.value;
    }
    const f32 u = glm::clamp((s.t - k1.time) / span, 0.f, 1.f);
    if (interp == CurveInterp::Linear) {
        return glm::mix(k1.value, k2.value, u);
    }
    const CurveKey k0 = key(s.i - 1), k3 = key(s.i + 2);
    const f32 m1 = (k2.value - k0.value) / std::max(k2.time - k0.time, 1e-6f) * span;
    const f32 m2 = (k3.value - k1.value) / std::max(k3.time - k1.time, 1e-6f) * span;
    const f32 u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * k1.value + (u3 - 2 * u2 + u) * m1 + (-2 * u3 + 3 * u2) * k2.value + (u3 - u2) * m2;
}

Gradient::Gradient(std::initializer_list<GradientKey> keys, bool w, f32 p) : wrap(w), period(p) {
    for (const auto& k : keys) {
        addKey(k.time, k.color);
    }
}

void Gradient::addKey(f32 time, glm::vec4 color) { insertSorted(m_keys, GradientKey{time, color}); }

glm::vec4 Gradient::evaluate(f32 t) const {
    const i32 n = i32(m_keys.size());
    if (n == 0) {
        return glm::vec4(1.f);
    }
    if (n == 1) {
        return m_keys[0].color;
    }
    if (!wrap) {
        if (t <= m_keys.front().time) {
            return m_keys.front().color;
        }
        if (t >= m_keys.back().time) {
            return m_keys.back().color;
        }
    }
    const Segment s = findSegment(m_keys, t, wrap, period);
    const GradientKey& a = m_keys[usize(s.i)];
    const bool seam = s.i + 1 >= n;
    const GradientKey& b = m_keys[usize(seam ? 0 : s.i + 1)];
    const f32 tb = b.time + (seam ? period : 0.f);
    const f32 span = tb - a.time;
    return span > 1e-6f ? glm::mix(a.color, b.color, glm::clamp((s.t - a.time) / span, 0.f, 1.f)) : a.color;
}

AtmosphereCurves AtmosphereCurves::defaults() {
    AtmosphereCurves c;
    c.driver = CurveDriver::SunElevation;
    c.fogDensity = Curve({{-18.f, 0.015f}, {-4.f, 0.025f}, {2.f, 0.02f}, {15.f, 0.006f}, {60.f, 0.004f}}, CurveInterp::Smooth);
    c.ambientIntensity = Curve({{-18.f, 0.02f}, {-6.f, 0.08f}, {0.f, 0.35f}, {10.f, 0.8f}, {30.f, 1.f}}, CurveInterp::Smooth);
    // Night needs a large positive exposure compensation: moonlight is ~5e-6 of sunlight.
    c.exposureCompensation = Curve({{-18.f, 4.f}, {-6.f, 2.5f}, {0.f, 1.f}, {15.f, 0.f}}, CurveInterp::Smooth);
    c.starsIntensity = Curve({{-18.f, 1.f}, {-8.f, 0.6f}, {-2.f, 0.f}}, CurveInterp::Linear);
    c.ambientColor = Gradient({{-18.f, {0.05f, 0.07f, 0.15f, 1.f}},
                               {-4.f, {0.35f, 0.3f, 0.45f, 1.f}},
                               {2.f, {0.9f, 0.6f, 0.45f, 1.f}},
                               {15.f, {0.6f, 0.75f, 1.f, 1.f}}});
    c.fogColor = Gradient({{-18.f, {0.02f, 0.03f, 0.06f, 1.f}},
                           {-4.f, {0.3f, 0.25f, 0.35f, 1.f}},
                           {2.f, {0.95f, 0.65f, 0.45f, 1.f}},
                           {15.f, {0.7f, 0.8f, 0.95f, 1.f}}});
    return c;
}

AtmosphereParams AtmosphereCurves::evaluate(f32 sunElevationDeg, f32 localHour) const {
    const f32 t = driver == CurveDriver::SunElevation ? sunElevationDeg : localHour;
    AtmosphereParams p;
    p.fogDensity = std::max(0.f, fogDensity.evaluate(t));
    p.ambientIntensity = std::max(0.f, ambientIntensity.evaluate(t));
    p.exposureCompensation = exposureCompensation.evaluate(t);
    p.starsIntensity = glm::clamp(starsIntensity.evaluate(t), 0.f, 1.f);
    p.ambientColor = ambientColor.evaluate(t);
    p.fogColor = fogColor.evaluate(t);
    return p;
}

// --- TimeOfDay -------------------------------------------------------------------------------

namespace {
constexpr f64 kHorizonDeg = -0.833; // apparent sunrise/sunset: refraction + solar radius

DateTime localToUtc(const TimeOfDaySettings& s) {
    return normalized({s.year, s.month, s.day, s.localHours - s.utcOffsetHours});
}
} // namespace

TimeOfDay::TimeOfDay(const TimeOfDaySettings& s) : m_settings(s) {
    setLocalTime(s.localHours);
}

SkyState TimeOfDay::evaluate(const TimeOfDaySettings& s, const AtmosphereCurves& curves) {
    SkyState st;
    st.local = normalized({s.year, s.month, s.day, s.localHours});
    st.utc = localToUtc(s);
    st.sun = computeSunPosition(s.location, st.utc);
    st.moon = computeMoonPosition(s.location, st.utc);
    st.moonPhase = computeMoonPhase(st.utc);
    st.sunDirection = horizontalToWorld(st.sun.elevationDeg, st.sun.azimuthDeg);
    st.moonDirection = horizontalToWorld(st.moon.elevationDeg, st.moon.azimuthDeg);
    st.sunLight = sunLight(st.sun.elevationDeg, s.turbidity);
    st.moonLight = moonLight(st.moon.elevationDeg, st.moonPhase, s.turbidity);
    st.starsRotation = starsRotation(s.location, st.utc);
    st.preetham = PreethamSky::compute(st.sunDirection, s.turbidity);
    st.atmosphere = curves.evaluate(f32(st.sun.elevationDeg), f32(st.local.hours));
    st.isDay = st.sun.elevationDeg > kHorizonDeg;
    const bool useSun = st.sunLight.illuminance >= st.moonLight.illuminance;
    st.mainLightDirection = useSun ? st.sunDirection : st.moonDirection;
    st.mainLightColor = useSun ? st.sunLight.color : st.moonLight.color;
    st.mainLightIlluminance = useSun ? st.sunLight.illuminance : st.moonLight.illuminance;
    return st;
}

void TimeOfDay::recompute() { m_state = evaluate(m_settings, m_curves); }

void TimeOfDay::setLocalTime(f64 hours) {
    const DateTime d = normalized({m_settings.year, m_settings.month, m_settings.day, hours});
    m_settings.year = d.year;
    m_settings.month = d.month;
    m_settings.day = d.day;
    m_settings.localHours = d.hours;
    recompute();
}

void TimeOfDay::setDate(i32 year, i32 month, i32 day) {
    m_settings.year = year;
    m_settings.month = month;
    m_settings.day = day;
    recompute();
}

void TimeOfDay::setLocation(const GeoLocation& loc) {
    m_settings.location = loc;
    recompute();
}

u32 TimeOfDay::addListener(Listener l) {
    const u32 id = m_nextListener++;
    m_listeners.emplace_back(id, std::move(l));
    return id;
}

void TimeOfDay::removeListener(u32 id) {
    std::erase_if(m_listeners, [id](const auto& p) { return p.first == id; });
}

void TimeOfDay::fire(TimeOfDayEvent e) {
    for (auto& [id, l] : m_listeners) {
        l(e, m_state);
    }
}

void TimeOfDay::update(f64 realDeltaSeconds) {
    if (m_settings.paused || realDeltaSeconds <= 0.0) {
        recompute();
        return;
    }
    advanceGameTime(realDeltaSeconds * m_settings.timeScale);
}

void TimeOfDay::advanceGameTime(f64 gameSeconds) {
    f64 remaining = gameSeconds / 3600.0;
    constexpr f64 kMaxStepHours = 1.0 / 6.0;
    constexpr int kMaxSteps = 24 * 6 * 31; // events are only tracked for jumps up to ~a month
    if (remaining > kMaxStepHours * kMaxSteps) {
        setLocalTime(m_settings.localHours + remaining);
        return;
    }
    SolarPosition prev = computeSunPosition(m_settings.location, localToUtc(m_settings));
    while (remaining > 0.0) {
        const f64 step = std::min(remaining, kMaxStepHours);
        remaining -= step;
        const i32 prevDay = m_settings.day;
        const DateTime d = normalized({m_settings.year, m_settings.month, m_settings.day, m_settings.localHours + step});
        m_settings.year = d.year;
        m_settings.month = d.month;
        m_settings.day = d.day;
        m_settings.localHours = d.hours;
        const SolarPosition cur = computeSunPosition(m_settings.location, localToUtc(m_settings));
        const bool crossedMidnight = d.day != prevDay;
        const bool rise = prev.elevationDeg <= kHorizonDeg && cur.elevationDeg > kHorizonDeg;
        const bool set = prev.elevationDeg > kHorizonDeg && cur.elevationDeg <= kHorizonDeg;
        const bool noon = prev.hourAngleDeg < 0.0 && cur.hourAngleDeg >= 0.0 && cur.hourAngleDeg - prev.hourAngleDeg < 180.0;
        if (crossedMidnight || rise || set || noon) {
            recompute();
            if (crossedMidnight) {
                fire(TimeOfDayEvent::Midnight);
            }
            if (rise) {
                fire(TimeOfDayEvent::Sunrise);
            }
            if (noon) {
                fire(TimeOfDayEvent::Noon);
            }
            if (set) {
                fire(TimeOfDayEvent::Sunset);
            }
        }
        prev = cur;
    }
    recompute();
}

void TimeOfDay::debugDraw(const DebugLineFn& line, glm::vec3 origin, f32 length) const {
    if (!line) {
        return;
    }
    line(origin, origin + m_state.sunDirection * length, glm::vec4(1.f, 0.9f, 0.2f, 1.f));
    line(origin, origin + m_state.moonDirection * length * 0.7f, glm::vec4(0.6f, 0.7f, 1.f, 1.f));
    const glm::vec3 pole = m_state.starsRotation * glm::vec3(0.f, 1.f, 0.f);
    line(origin, origin + pole * length * 0.5f, glm::vec4(1.f, 1.f, 1.f, 1.f));
    // Compass: north (-Z) red, east (+X) green.
    line(origin, origin + glm::vec3(0.f, 0.f, -length * 0.3f), glm::vec4(1.f, 0.f, 0.f, 1.f));
    line(origin, origin + glm::vec3(length * 0.3f, 0.f, 0.f), glm::vec4(0.f, 1.f, 0.f, 1.f));
}

} // namespace ox::world
