#include <oxwald/world/weather.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace ox::world {

namespace {
constexpr f32 kTwoPi = 6.28318530718f;
glm::vec2 safeDir(glm::vec2 d) { return glm::dot(d, d) > 1e-12f ? glm::normalize(d) : glm::vec2(1.f, 0.f); }
} // namespace

// Keep in sync with engine/shaders/world/wind.glsl.
f32 WindField::gustFactor(glm::vec3 p, f32 t) const {
    const WindSettings& s = m_settings;
    const glm::vec2 dir = safeDir(s.direction);
    const glm::vec2 perp(-dir.y, dir.x);
    const glm::vec2 xz(p.x, p.z);
    const f32 lambda = std::max(s.gustWavelength, 1e-3f);
    const f32 along = (glm::dot(xz, dir) - s.speed * t) / lambda; // gust fronts travel downwind
    const f32 across = glm::dot(xz, perp) / lambda;
    const f32 g = 0.5f * std::sin(kTwoPi * along + 0.7f * std::sin(kTwoPi * across * 0.31f)) +
                  0.3f * std::sin(kTwoPi * along * 2.71f + 1.7f) + 0.2f * std::sin(kTwoPi * (along * 6.13f + across * 1.37f) + 4.1f);
    return 1.f + s.gustStrength * g;
}

glm::vec3 WindField::sample(glm::vec3 p, f32 t) const {
    const WindSettings& s = m_settings;
    const glm::vec2 dir = safeDir(s.direction);
    const glm::vec2 perp(-dir.y, dir.x);
    const f32 lambda = std::max(s.gustWavelength, 1e-3f);
    const f32 across = glm::dot(glm::vec2(p.x, p.z), perp) / lambda;
    const f32 sway = std::sin(kTwoPi * (s.turbulenceFrequency * t + across * 2.3f));
    const glm::vec2 v = dir * (s.speed * gustFactor(p, t)) + perp * (s.speed * s.turbulence * sway);
    return {v.x, 0.f, v.y};
}

WindGpu WindField::toGpu(f32 time) const {
    const glm::vec2 d = safeDir(m_settings.direction);
    return {{d.x, d.y, m_settings.speed, time},
            {m_settings.gustStrength, m_settings.gustWavelength, m_settings.turbulence, m_settings.turbulenceFrequency}};
}

void WindField::debugDraw(const DebugLineFn& line, glm::vec3 center, f32 extent, f32 time, u32 n) const {
    if (!line || n == 0) {
        return;
    }
    const f32 step = 2.f * extent / f32(n);
    const f32 scale = step * 0.8f / std::max(m_settings.speed * (1.f + m_settings.gustStrength), 1e-3f);
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            const glm::vec3 p = center + glm::vec3(-extent + f32(x) * step, 0.f, -extent + f32(z) * step);
            const glm::vec3 v = sample(p, time);
            const f32 g = glm::clamp((gustFactor(p, time) - 1.f) * 2.f + 0.5f, 0.f, 1.f);
            line(p, p + v * scale, glm::vec4(0.3f + 0.7f * g, 0.8f, 1.f - 0.6f * g, 1.f));
        }
    }
}

WeatherController::WeatherController(const WeatherPreset& initial, const WeatherSettings& s) : m_settings(s) {
    m_state.current = m_from = m_to = initial;
}

void WeatherController::setTarget(const WeatherPreset& target, f32 seconds) {
    m_from = m_state.current;
    m_to = target;
    m_duration = std::max(0.f, seconds);
    m_elapsed = 0.f;
    m_state.transition = m_duration > 0.f ? 0.f : 1.f;
    if (m_duration <= 0.f) {
        m_state.current = target;
    }
}

void WeatherController::update(f32 dt, f32 temperatureCelsius) {
    if (m_state.transition < 1.f) {
        m_elapsed += dt;
        const f32 u = glm::clamp(m_elapsed / std::max(m_duration, 1e-6f), 0.f, 1.f);
        const f32 k = u * u * (3.f - 2.f * u);
        auto lerp = [k](f32 a, f32 b) { return a + (b - a) * k; };
        WeatherPreset& c = m_state.current;
        c.cloudCover = lerp(m_from.cloudCover, m_to.cloudCover);
        c.rain = lerp(m_from.rain, m_to.rain);
        c.snow = lerp(m_from.snow, m_to.snow);
        c.fogDensityBoost = lerp(m_from.fogDensityBoost, m_to.fogDensityBoost);
        c.windSpeed = lerp(m_from.windSpeed, m_to.windSpeed);
        c.gustStrength = lerp(m_from.gustStrength, m_to.gustStrength);
        m_state.transition = u;
    }
    const WeatherPreset& c = m_state.current;
    m_state.wetness += dt * (c.rain * m_settings.wettingRate - (1.f - c.rain) * m_settings.dryingRate);
    m_state.wetness = glm::clamp(m_state.wetness, 0.f, 1.f);
    if (temperatureCelsius <= 0.f) {
        m_state.snowCover += dt * c.snow * m_settings.snowAccumRate;
    } else {
        m_state.snowCover -= dt * temperatureCelsius * m_settings.meltRate;
    }
    m_state.snowCover = glm::clamp(m_state.snowCover, 0.f, 1.f);
}

WindSettings WeatherController::wind(const WindSettings& base) const {
    WindSettings w = base;
    w.speed = m_state.current.windSpeed;
    w.gustStrength = m_state.current.gustStrength;
    return w;
}

} // namespace ox::world
