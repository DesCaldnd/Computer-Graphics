#pragma once

#include <oxwald/world/common.hpp>
#include <oxwald/world/sky.hpp>

#include <functional>
#include <utility>
#include <vector>

namespace ox::world {

// --- keyframe curves -------------------------------------------------------------------------
enum class CurveInterp : u8 { Step, Linear, Smooth }; // Smooth = Catmull-Rom (auto tangents)

struct CurveKey {
    f32 time = 0.f;
    f32 value = 0.f;
};

// Scalar keyframe curve. Keys are kept sorted. With wrap = true the curve is periodic over `period`
// (e.g. 24 hours) and interpolates across the seam.
class Curve {
public:
    Curve() = default;
    Curve(std::initializer_list<CurveKey> keys, CurveInterp interp = CurveInterp::Linear, bool wrap = false, f32 period = 24.f);
    void addKey(f32 time, f32 value);
    void clear() { m_keys.clear(); }
    [[nodiscard]] f32 evaluate(f32 t) const;
    [[nodiscard]] const std::vector<CurveKey>& keys() const { return m_keys; }
    CurveInterp interp = CurveInterp::Linear;
    bool wrap = false;
    f32 period = 24.f;

private:
    std::vector<CurveKey> m_keys;
};

struct GradientKey {
    f32 time = 0.f;
    glm::vec4 color{1.f}; // linear RGBA
};

class Gradient {
public:
    Gradient() = default;
    Gradient(std::initializer_list<GradientKey> keys, bool wrap = false, f32 period = 24.f);
    void addKey(f32 time, glm::vec4 color);
    [[nodiscard]] glm::vec4 evaluate(f32 t) const; // linear interpolation
    [[nodiscard]] const std::vector<GradientKey>& keys() const { return m_keys; }
    bool wrap = false;
    f32 period = 24.f;

private:
    std::vector<GradientKey> m_keys;
};

// --- atmosphere curves -----------------------------------------------------------------------
enum class CurveDriver : u8 {
    SunElevation, // curve time = sun elevation in degrees [-90, 90] (robust across latitudes/seasons)
    LocalHour,    // curve time = local hour [0, 24) (curves should set wrap = true)
};

struct AtmosphereParams {
    f32 fogDensity = 0.f;
    f32 ambientIntensity = 1.f;
    f32 exposureCompensation = 0.f; // EV
    f32 starsIntensity = 0.f;
    glm::vec4 ambientColor{1.f};
    glm::vec4 fogColor{1.f};
};

struct AtmosphereCurves {
    CurveDriver driver = CurveDriver::SunElevation;
    Curve fogDensity, ambientIntensity, exposureCompensation, starsIntensity;
    Gradient ambientColor, fogColor;
    static AtmosphereCurves defaults();
    [[nodiscard]] AtmosphereParams evaluate(f32 sunElevationDeg, f32 localHour) const;
};

// --- time of day controller ------------------------------------------------------------------
enum class TimeOfDayEvent : u8 { Sunrise, Sunset, Noon, Midnight };

struct TimeOfDaySettings {
    GeoLocation location{52.37, 4.90}; // Amsterdam
    i32 year = 2024, month = 6, day = 21; // local date
    f64 localHours = 9.0;
    f64 utcOffsetHours = 2.0;   // local = UTC + offset
    f64 timeScale = 60.0;       // game seconds per real second (60 → a day lasts 24 real minutes)
    bool paused = false;
    f32 turbidity = 2.5f;
};

struct SkyState {
    DateTime utc{};
    DateTime local{};
    SolarPosition sun{};
    CelestialPosition moon{};
    MoonPhase moonPhase{};
    glm::vec3 sunDirection{0.f, 1.f, 0.f};  // towards the sun
    glm::vec3 moonDirection{0.f, 1.f, 0.f};
    CelestialLight sunLight{};
    CelestialLight moonLight{};
    glm::quat starsRotation{1.f, 0.f, 0.f, 0.f};
    PreethamSky preetham{};
    AtmosphereParams atmosphere{};
    bool isDay = true; // sun centre above -0.833° (apparent sunrise/sunset definition)
    // Dominant directional light (sun by day, moon by night) — what the renderer's main light uses.
    glm::vec3 mainLightDirection{0.f, 1.f, 0.f};
    glm::vec3 mainLightColor{1.f};
    f32 mainLightIlluminance = 0.f;
};

class TimeOfDay {
public:
    using Listener = std::function<void(TimeOfDayEvent, const SkyState&)>;

    explicit TimeOfDay(const TimeOfDaySettings& s = {});

    // Advance by real seconds × timeScale (unless paused), fire events crossed on the way, recompute
    // the sky state. Large steps are sub-sampled (≤ 10 game minutes) so no sunrise is missed.
    void update(f64 realDeltaSeconds);
    void advanceGameTime(f64 gameSeconds);

    void setLocalTime(f64 hours); // jumps without events
    void setDate(i32 year, i32 month, i32 day);
    void setLocation(const GeoLocation& loc);
    void setTimeScale(f64 s) { m_settings.timeScale = s; }
    void setPaused(bool p) { m_settings.paused = p; }
    [[nodiscard]] const TimeOfDaySettings& settings() const { return m_settings; }
    [[nodiscard]] f64 localHours() const { return m_settings.localHours; }

    u32 addListener(Listener l);
    void removeListener(u32 id);

    AtmosphereCurves& curves() { return m_curves; }
    [[nodiscard]] const SkyState& state() const { return m_state; }
    // Pure function of settings (no events): handy for previews and tests.
    [[nodiscard]] static SkyState evaluate(const TimeOfDaySettings& s, const AtmosphereCurves& curves);

    void debugDraw(const DebugLineFn& line, glm::vec3 origin, f32 length = 50.f) const;

private:
    void recompute();
    void fire(TimeOfDayEvent e);
    TimeOfDaySettings m_settings;
    AtmosphereCurves m_curves = AtmosphereCurves::defaults();
    SkyState m_state{};
    std::vector<std::pair<u32, Listener>> m_listeners;
    u32 m_nextListener = 1;
};

} // namespace ox::world
