#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/spline/spline.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ox::spline {

enum class LoopMode : u8 {
    Once,     // stop at the end (finished() becomes true)
    Loop,     // wrap to the start (seamless on closed splines, a jump on open ones)
    PingPong, // reverse direction at each end
};

struct PathEvent {
    std::string_view name;
    f32 distance = 0.0f;
    i32 direction = 1;    // +1 when crossed moving forward, -1 backward
    bool marker = false;  // true for Spline markers, false for follower events
};

struct FollowerPose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    f32 distance = 0.0f;
    f32 t = 0.0f;
};

struct FollowerSettings {
    f32 speed = 1.0f; // units per second along the arc length; negative runs backwards
    LoopMode loopMode = LoopMode::Loop;
    bool orientToPath = true;
    bool faceTravelDirection = true; // when moving backwards, turn around (forward axis follows motion)
    glm::vec3 forwardAxis{0.0f, 0.0f, -1.0f}; // local axis aligned with the path tangent
    glm::vec3 upAxis{0.0f, 1.0f, 0.0f};       // local axis aligned with the frame normal
    bool fireMarkers = true;                  // also fire the spline's named markers
};

// Moves a point along a Spline at constant world speed (distance based, independent of control point spacing).
// Stateless with respect to the spline: pass the spline to every call, so it can live in a component.
// Events fire for every crossed distance, including several per update, across loop wraps and in both
// ping-pong directions. Crossing uses half-open intervals so an event exactly at a turn-around fires once.
class PathFollower {
public:
    using EventCallback = std::function<void(const PathEvent&)>;

    PathFollower() = default;
    explicit PathFollower(FollowerSettings settings) : m_settings(settings) {}

    FollowerSettings& settings() { return m_settings; }
    const FollowerSettings& settings() const { return m_settings; }

    void advance(const Spline& spline, f32 dt);
    FollowerPose pose(const Spline& spline) const;

    void reset(f32 distance = 0.0f);
    void setDistance(f32 distance) { m_distance = distance; }
    f32 distance() const { return m_distance; }
    // +1 forward / -1 backward; ping-pong flips it. The effective velocity is speed * direction.
    i32 direction() const { return m_direction; }
    void setDirection(i32 direction) { m_direction = direction < 0 ? -1 : 1; }
    bool finished() const { return m_finished; }

    // Follower-local events at fixed distances (in addition to the spline's markers).
    void addEvent(std::string name, f32 distance);
    void clearEvents() { m_events.clear(); }
    void setEventCallback(EventCallback callback) { m_callback = std::move(callback); }

private:
    struct Event {
        std::string name;
        f32 distance = 0.0f;
    };
    void fireBetween(const Spline& spline, f32 from, f32 to, bool includeFrom, i32 direction) const;

    FollowerSettings m_settings;
    f32 m_distance = 0.0f;
    i32 m_direction = 1;
    bool m_finished = false;
    bool m_inclusiveStart = true; // the next crossing test includes the start distance (first move, after a wrap)
    std::vector<Event> m_events;
    EventCallback m_callback;
};

} // namespace ox::spline
