#pragma once

#include <oxwald/core/types.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <deque>
#include <optional>

namespace ox::net {

struct TransformSample {
    f64 time = 0.0; // server time of the snapshot
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    std::optional<glm::vec3> velocity; // if replicated, used as Hermite tangent; else finite differences
};

struct InterpolatedTransform {
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    bool extrapolated = false;
};

// Snapshot interpolation for remote objects. Push samples as snapshots arrive (NetObject::onSnapshotApplied),
// sample at renderTime = serverTime - interpolationDelay (NetClient::renderTime()).
// Positions: cubic Hermite; rotations: slerp. Past the newest sample it extrapolates up to maxExtrapolation.
class InterpolationBuffer {
public:
    explicit InterpolationBuffer(usize capacity = 32, f64 maxExtrapolation = 0.25)
        : m_capacity(capacity), m_maxExtrapolation(maxExtrapolation) {}

    // Out-of-order or duplicate samples (time <= newest) are ignored.
    void push(const TransformSample& sample);
    std::optional<InterpolatedTransform> sample(f64 renderTime) const;

    usize size() const { return m_samples.size(); }
    void clear() { m_samples.clear(); }
    const std::deque<TransformSample>& samples() const { return m_samples; }

private:
    glm::vec3 tangent(usize i) const; // units per second
    usize m_capacity;
    f64 m_maxExtrapolation;
    std::deque<TransformSample> m_samples;
};

// NTP-style clock offset estimation. The client sends its local time; the server answers with (clientTime,
// serverTime); offset = serverTime - (send + receive) / 2. Uses the lowest-RTT sample of the recent window
// (queueing delay only ever inflates RTT) and slews towards it to avoid visible jumps.
class ClockSync {
public:
    explicit ClockSync(usize window = 16) : m_window(window) {}

    void addSample(f64 clientSendTime, f64 serverTime, f64 clientReceiveTime);
    bool synced() const { return m_count > 0; }
    f64 offset() const { return m_offset; }
    f64 rtt() const { return m_bestRtt; }
    f64 serverTime(f64 localNow) const { return localNow + m_offset; }

private:
    struct Sample {
        f64 rtt;
        f64 offset;
    };
    usize m_window;
    std::deque<Sample> m_samples;
    usize m_count = 0;
    f64 m_offset = 0.0;
    f64 m_bestRtt = 0.0;
};

} // namespace ox::net
