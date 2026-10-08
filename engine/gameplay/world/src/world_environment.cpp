#include "world_runtime_impl.hpp"

#include <oxwald/core/profile.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace ox::gameplay {

namespace {

bool samePreset(const world::WeatherPreset& a, const world::WeatherPreset& b) {
    return a.cloudCover == b.cloudCover && a.rain == b.rain && a.snow == b.snow && a.fogDensityBoost == b.fogDensityBoost &&
           a.windSpeed == b.windSpeed && a.gustStrength == b.gustStrength;
}

world::Curve makeCurve(const std::vector<world::CurveKey>& keys, world::CurveInterp interp, bool wrap) {
    world::Curve c;
    c.interp = interp;
    c.wrap = wrap;
    c.period = 24.f;
    for (const auto& k : keys) c.addKey(k.time, k.value);
    return c;
}

world::Gradient makeGradient(const std::vector<world::GradientKey>& keys, bool wrap) {
    world::Gradient g;
    g.wrap = wrap;
    g.period = 24.f;
    for (const auto& k : keys) g.addKey(k.time, k.color);
    return g;
}

template <class C>
std::vector<entt::entity> sortedView(entt::registry& r) {
    std::vector<entt::entity> list;
    for (auto e : r.view<C>()) list.push_back(e);
    std::sort(list.begin(), list.end());
    return list;
}

} // namespace

// ---- wind / weather ------------------------------------------------------------------------------------------

void WorldRuntime::Impl::syncWind(Entity e, WindState& st, f32 dt, bool isPlaying) {
    auto& c = world->registry().get<WindComponent>(e.handle());
    if (st.dirty) {
        if (c.weatherEnabled) {
            if (!st.weatherInitialized || !isPlaying) {
                // Edit mode (and first use) snaps to the preset; play mode blends towards changed targets.
                if (!st.weatherInitialized || !samePreset(st.target, c.weather)) st.weather = world::WeatherController(c.weather);
                st.weatherInitialized = true;
            } else if (!samePreset(st.target, c.weather)) {
                st.weather.setTarget(c.weather, std::max(0.f, c.transitionSeconds));
            }
            st.target = c.weather;
        } else {
            st.weatherInitialized = false;
        }
        st.dirty = false;
    }
    world::WindSettings base;
    base.direction = glm::length(c.direction) > 1e-6f ? c.direction : glm::vec2(1.f, 0.f);
    base.speed = c.speed;
    base.gustStrength = c.gustStrength;
    base.gustWavelength = std::max(0.01f, c.gustWavelength);
    base.turbulence = c.turbulence;
    base.turbulenceFrequency = c.turbulenceFrequency;
    if (c.weatherEnabled && st.weatherInitialized) {
        if (isPlaying) st.weather.update(dt, c.temperatureCelsius);
        st.field.setSettings(st.weather.wind(base));
        c.weatherState = st.weather.state();
    } else {
        st.field.setSettings(base);
        c.weatherState = {};
    }
}

// ---- water ---------------------------------------------------------------------------------------------------

std::optional<f32> WorldRuntime::Impl::waterHeightAt(glm::vec2 xz, f32 t, const WaterState** which) const {
    std::optional<f32> best;
    for (const auto& [e, st] : waters) {
        if (st.dirty) continue; // not built yet
        if (st.size.x > 0.f && st.size.y > 0.f) {
            const glm::vec2 d = glm::abs(xz - st.center);
            if (d.x > st.size.x * 0.5f || d.y > st.size.y * 0.5f) continue;
        }
        const f32 h = st.waves.count() > 0 ? st.waves.heightAt(xz, t) : st.waves.baseHeight;
        if (!best || h > *best) {
            best = h;
            if (which) *which = &st;
        }
    }
    return best;
}

// ---- time of day ---------------------------------------------------------------------------------------------

void WorldRuntime::Impl::syncTimeOfDay(Entity e, TimeOfDayState& st) {
    const auto& c = e.get<TimeOfDayComponent>();
    world::TimeOfDaySettings s;
    s.location = {c.latitudeDeg, c.longitudeDeg};
    s.year = c.year;
    s.month = std::clamp(c.month, 1, 12);
    s.day = std::clamp(c.day, 1, 31);
    s.localHours = c.localHours;
    s.utcOffsetHours = c.utcOffsetHours;
    s.timeScale = c.timeScale;
    s.paused = c.paused;
    s.turbidity = c.turbidity;
    st.tod = std::make_unique<world::TimeOfDay>(s);
    world::AtmosphereCurves& curves = st.tod->curves();
    curves.driver = c.curveDriver;
    const bool wrap = c.curveDriver == world::CurveDriver::LocalHour;
    if (!c.fogDensityCurve.empty()) curves.fogDensity = makeCurve(c.fogDensityCurve, c.curveInterp, wrap);
    if (!c.ambientIntensityCurve.empty()) curves.ambientIntensity = makeCurve(c.ambientIntensityCurve, c.curveInterp, wrap);
    if (!c.exposureCurve.empty()) curves.exposureCompensation = makeCurve(c.exposureCurve, c.curveInterp, wrap);
    if (!c.starsCurve.empty()) curves.starsIntensity = makeCurve(c.starsCurve, c.curveInterp, wrap);
    if (!c.ambientColorGradient.empty()) curves.ambientColor = makeGradient(c.ambientColorGradient, wrap);
    if (!c.fogColorGradient.empty()) curves.fogColor = makeGradient(c.fogColorGradient, wrap);
    st.tod->setLocalTime(c.localHours); // recompute with the curves
    st.tod->addListener([this, h = e.handle()](world::TimeOfDayEvent ev, const world::SkyState& sky) {
        if (!world || !world->valid(h)) return;
        WorldTimeEvent we{world->wrap(h), ev, sky.local.hours, sky.isDay};
        self.onTimeOfDayEvent.emit(we);
        if (bus) bus->publish(we);
    });
    st.dirty = false;
}

void WorldRuntime::Impl::applyTimeOfDay(Entity e, const world::SkyState& s, const TimeOfDayComponent& c) {
    entt::registry& r = world->registry();
    const world::AtmosphereParams& atm = s.atmosphere;
    if (c.driveLight) {
        Entity sun = c.sun.valid() ? world->resolve(c.sun) : Entity{};
        if (!sun.valid()) {
            if (const auto* env = e.tryGet<EnvironmentComponent>(); env && env->sun.valid()) sun = world->resolve(env->sun);
        }
        if (!sun.valid()) {
            entt::entity best{entt::null};
            for (auto [le, l] : r.view<LightComponent>().each()) {
                if (l.type != LightType::Directional) continue;
                if (best == entt::null || entt::to_integral(le) < entt::to_integral(best)) best = le;
            }
            if (best != entt::null) sun = world->wrap(best);
        }
        if (auto* light = sun.valid() ? sun.tryGet<LightComponent>() : nullptr) {
            // The light shines along its -Z: point it away from the light source.
            const glm::vec3 forward = -glm::normalize(s.mainLightDirection);
            const glm::vec3 up = std::abs(forward.y) > 0.999f ? glm::vec3(0.f, 0.f, -1.f) : glm::vec3(0.f, 1.f, 0.f);
            const glm::vec3 current = sun.worldRotation() * glm::vec3(0.f, 0.f, -1.f);
            if (glm::dot(current, forward) < 0.9999999f) sun.setWorldRotation(glm::quatLookAt(forward, up));
            light->color = s.mainLightColor;
            light->intensity = s.mainLightIlluminance * c.illuminanceScale;
        }
    }
    if (c.driveEnvironment) {
        EnvironmentComponent* env = e.tryGet<EnvironmentComponent>();
        if (!env) {
            entt::entity best{entt::null};
            for (auto ee : r.view<EnvironmentComponent>()) {
                if (best == entt::null || entt::to_integral(ee) < entt::to_integral(best)) best = ee;
            }
            if (best != entt::null) env = &r.get<EnvironmentComponent>(best);
        }
        if (env) {
            f32 boost = 0.f;
            if (globalWind != entt::null && world->valid(globalWind)) {
                if (const auto* wc = r.try_get<WindComponent>(globalWind); wc && wc->weatherEnabled) {
                    boost = wc->weatherState.current.fogDensityBoost;
                }
            }
            env->fogDensity = atm.fogDensity + boost;
            env->fogColor = glm::vec3(atm.fogColor);
            env->ambientIntensity = atm.ambientIntensity;
        }
    }
    if (c.driveExposure) {
        if (Entity cam = primaryCamera(); cam.valid()) cam.get<CameraComponent>().exposureCompensation = atm.exposureCompensation;
    }
}

void WorldRuntime::updateEnvironment(f32 dt, bool playing) {
    OX_PROFILE_ZONE();
    if (!m->world) return;
    entt::registry& r = m->world->registry();

    // Wind / weather first: the time of day adds the weather's fog boost.
    m->globalWind = entt::null;
    for (auto e : sortedView<WindComponent>(r)) {
        m->syncWind(m->world->wrap(e), m->winds[e], dt, playing);
        if (m->globalWind == entt::null && r.get<WindComponent>(e).active) m->globalWind = e;
    }

    for (auto [e, c] : r.view<WaterComponent>().each()) {
        WaterState& st = m->waters[e];
        const glm::vec3 pos = m->world->wrap(e).worldPosition();
        if (st.dirty) {
            if (!c.waves.empty()) {
                const usize n = std::min<usize>(c.waves.size(), world::kMaxGerstnerWaves);
                st.waves = world::GerstnerWaves(std::span<const world::GerstnerWave>(c.waves.data(), n), pos.y);
            } else if (c.waveCount > 0) {
                const glm::vec2 dir = glm::length(c.windDirection) > 1e-6f ? c.windDirection : glm::vec2(1.f, 0.f);
                st.waves = world::GerstnerWaves::fromWind(dir, c.windSpeed, std::min(c.waveCount, world::kMaxGerstnerWaves),
                                                          c.seed, c.steepness, 35.f, pos.y);
            } else {
                st.waves = world::GerstnerWaves{};
            }
            st.dirty = false;
        }
        st.waves.baseHeight = pos.y;
        st.center = {pos.x, pos.z};
        st.size = c.size;
        st.density = c.fluidDensity;
        st.current = c.current;
    }

    for (auto e : sortedView<TimeOfDayComponent>(r)) {
        const Entity ent = m->world->wrap(e);
        TimeOfDayState& st = m->timeOfDays[e];
        if (st.dirty || !st.tod) m->syncTimeOfDay(ent, st);
        if (playing) {
            st.tod->update(static_cast<f64>(dt));
            // Event handlers may have removed the component (or patched it: rebuilt next frame).
            if (!m->world->valid(e) || !r.all_of<TimeOfDayComponent>(e) || !m->timeOfDays.contains(e)) continue;
        }
        auto& c = r.get<TimeOfDayComponent>(e);
        if (playing) {
            const world::TimeOfDaySettings& s = st.tod->settings();
            c.localHours = s.localHours;
            c.year = s.year;
            c.month = s.month;
            c.day = s.day;
        }
        const world::SkyState& sky = st.tod->state();
        c.sunElevationDeg = static_cast<f32>(sky.sun.elevationDeg);
        c.isDay = sky.isDay;
        c.moonIllumination = sky.moonPhase.illuminatedFraction;
        m->applyTimeOfDay(ent, sky, c);
    }
}

} // namespace ox::gameplay
