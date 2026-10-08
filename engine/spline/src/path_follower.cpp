#include <oxwald/spline/path_follower.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ox::spline {

void PathFollower::reset(f32 distance) {
    m_distance = distance;
    m_direction = 1;
    m_finished = false;
    m_inclusiveStart = true;
}

void PathFollower::addEvent(std::string name, f32 distance) {
    m_events.push_back(Event{std::move(name), distance});
}

void PathFollower::fireBetween(const Spline& spline, f32 from, f32 to, bool includeFrom, i32 direction) const {
    if (!m_callback) {
        return;
    }
    auto crossed = [&](f32 d) {
        if (direction > 0) {
            return (includeFrom ? d >= from : d > from) && d <= to;
        }
        return (includeFrom ? d <= from : d < from) && d >= to;
    };
    std::vector<PathEvent> hits;
    for (const Event& e : m_events) {
        if (crossed(e.distance)) {
            hits.push_back(PathEvent{e.name, e.distance, direction, false});
        }
    }
    if (m_settings.fireMarkers) {
        for (const SplineMarker& m : spline.markers()) {
            const f32 d = spline.tToDistance(m.t);
            if (crossed(d)) {
                hits.push_back(PathEvent{m.name, d, direction, true});
            }
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [direction](const PathEvent& a, const PathEvent& b) {
        return direction > 0 ? a.distance < b.distance : a.distance > b.distance;
    });
    for (const PathEvent& e : hits) {
        m_callback(e);
    }
}

void PathFollower::advance(const Spline& spline, f32 dt) {
    if (m_finished || dt <= 0.0f) {
        return;
    }
    const f32 total = spline.length();
    const f32 velocity = m_settings.speed * static_cast<f32>(m_direction);
    if (total <= 0.0f || velocity == 0.0f) {
        return;
    }
    i32 dir = velocity > 0.0f ? 1 : -1;
    f32 remaining = std::abs(velocity) * dt;
    if (m_settings.loopMode != LoopMode::Once && remaining > 8.0f * total) {
        // Absurd time steps: skip whole laps instead of firing every event dozens of times.
        remaining = std::fmod(remaining, 2.0f * total);
    }
    m_distance = std::clamp(m_distance, 0.0f, total);

    for (int guard = 0; guard < 64; ++guard) {
        const f32 boundary = dir > 0 ? total : 0.0f;
        const f32 room = std::abs(boundary - m_distance);
        const bool reaches = remaining >= room;
        const f32 step = reaches ? room : remaining;
        const f32 next = reaches ? boundary : m_distance + static_cast<f32>(dir) * step;
        fireBetween(spline, m_distance, next, m_inclusiveStart, dir);
        m_inclusiveStart = false;
        m_distance = next;
        remaining -= step;
        if (!reaches) {
            break;
        }
        if (m_settings.loopMode == LoopMode::Once) {
            m_finished = true;
            break;
        }
        if (m_settings.loopMode == LoopMode::Loop) {
            m_distance = dir > 0 ? 0.0f : total;
            m_inclusiveStart = true;
        } else {
            m_direction = -m_direction;
            dir = -dir;
        }
        if (remaining <= 0.0f) {
            break;
        }
    }
}

FollowerPose PathFollower::pose(const Spline& spline) const {
    FollowerPose out;
    out.distance = m_distance;
    out.t = spline.distanceToT(m_distance);
    const SplineSample s = spline.evaluate(out.t);
    out.position = s.position;
    if (m_settings.orientToPath) {
        const f32 velocity = m_settings.speed * static_cast<f32>(m_direction);
        const bool backwards = m_settings.faceTravelDirection && (velocity < 0.0f || (velocity == 0.0f && m_direction < 0));
        out.rotation = frameRotation(backwards ? -s.tangent : s.tangent, s.normal, m_settings.forwardAxis,
                                     m_settings.upAxis);
    }
    return out;
}

} // namespace ox::spline
