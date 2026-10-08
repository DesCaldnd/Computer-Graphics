#pragma once

#include <oxwald/world/common.hpp>

namespace ox::world {

// --- wind ------------------------------------------------------------------------------------
// Global wind with travelling gusts. The evaluation is mirrored exactly in
// engine/shaders/world/wind.glsl (oxWindSample) so vegetation sway on the GPU matches CPU queries
// (cloth, particles, sailing, audio).
struct WindSettings {
    glm::vec2 direction{1.f, 0.f}; // XZ, normalised on use (direction the wind blows towards)
    f32 speed = 4.f;               // m/s
    f32 gustStrength = 0.4f;       // gust amplitude as a fraction of speed
    f32 gustWavelength = 60.f;     // metres between gust fronts (they travel downwind at `speed`)
    f32 turbulence = 0.15f;        // cross-wind sway as a fraction of speed
    f32 turbulenceFrequency = 0.6f; // Hz
};

struct WindGpu { // std140, 32 bytes
    glm::vec4 dirSpeedTime; // xy = direction, z = speed, w = time (seconds)
    glm::vec4 gust;         // x = gustStrength, y = gustWavelength, z = turbulence, w = turbulenceFrequency
};

class WindField {
public:
    WindField() = default;
    explicit WindField(const WindSettings& s) : m_settings(s) {}
    void setSettings(const WindSettings& s) { m_settings = s; }
    [[nodiscard]] const WindSettings& settings() const { return m_settings; }

    // Wind velocity (m/s, horizontal) at a world position and time.
    [[nodiscard]] glm::vec3 sample(glm::vec3 position, f32 time) const;
    [[nodiscard]] f32 gustFactor(glm::vec3 position, f32 time) const; // ~[1-g, 1+g]
    [[nodiscard]] WindGpu toGpu(f32 time) const;
    void debugDraw(const DebugLineFn& line, glm::vec3 center, f32 extent, f32 time, u32 gridCount = 8) const;

private:
    WindSettings m_settings{};
};

// --- precipitation / weather state ----------------------------------------------------------
struct WeatherPreset {
    f32 cloudCover = 0.2f;  // 0..1
    f32 rain = 0.f;         // 0..1 intensity (renderer: particle rate, wet shading, audio)
    f32 snow = 0.f;         // 0..1
    f32 fogDensityBoost = 0.f; // added to the time-of-day fog density
    f32 windSpeed = 4.f;
    f32 gustStrength = 0.4f;
    static WeatherPreset clear() { return {}; }
    static WeatherPreset overcast() { return {0.9f, 0.f, 0.f, 0.005f, 6.f, 0.5f}; }
    static WeatherPreset rainy() { return {1.f, 0.8f, 0.f, 0.01f, 8.f, 0.6f}; }
    static WeatherPreset storm() { return {1.f, 1.f, 0.f, 0.015f, 16.f, 0.8f}; }
    static WeatherPreset snowy() { return {0.95f, 0.f, 0.7f, 0.012f, 3.f, 0.3f}; }
};

struct WeatherState {
    WeatherPreset current{};
    f32 wetness = 0.f;    // surface wetness 0..1 (accumulates with rain, dries otherwise)
    f32 snowCover = 0.f;  // 0..1 (accumulates with snow below 0 °C, melts above)
    f32 transition = 1.f; // 0..1 progress towards the target preset
};

struct WeatherSettings {
    f32 wettingRate = 0.05f;   // per second at rain = 1
    f32 dryingRate = 0.005f;   // per second
    f32 snowAccumRate = 0.01f; // per second at snow = 1
    f32 meltRate = 0.003f;     // per second per °C above zero
};

class WeatherController {
public:
    explicit WeatherController(const WeatherPreset& initial = {}, const WeatherSettings& s = {});
    // Smoothly blend to `target` over `seconds` (smoothstep). 0 = immediate.
    void setTarget(const WeatherPreset& target, f32 seconds);
    void update(f32 dt, f32 temperatureCelsius = 15.f);
    [[nodiscard]] const WeatherState& state() const { return m_state; }
    [[nodiscard]] bool transitioning() const { return m_state.transition < 1.f; }
    // Wind settings with the current preset's speed/gust applied on top of a base (direction etc).
    [[nodiscard]] WindSettings wind(const WindSettings& base) const;

private:
    WeatherSettings m_settings;
    WeatherState m_state;
    WeatherPreset m_from{}, m_to{};
    f32 m_duration = 0.f;
    f32 m_elapsed = 0.f;
};

} // namespace ox::world
