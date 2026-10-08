#include <oxwald/net/interpolation.hpp>

#include <algorithm>
#include <cmath>

namespace ox::net {

void InterpolationBuffer::push(const TransformSample& sample) {
    if (!m_samples.empty() && sample.time <= m_samples.back().time) {
        return;
    }
    m_samples.push_back(sample);
    while (m_samples.size() > m_capacity) {
        m_samples.pop_front();
    }
}

glm::vec3 InterpolationBuffer::tangent(usize i) const {
    const TransformSample& s = m_samples[i];
    if (s.velocity) {
        return *s.velocity;
    }
    const usize n = m_samples.size();
    if (n < 2) {
        return glm::vec3(0.f);
    }
    // Catmull-Rom style central difference, one-sided at the ends.
    const usize a = i == 0 ? 0 : i - 1;
    const usize b = i + 1 >= n ? n - 1 : i + 1;
    const f64 dt = m_samples[b].time - m_samples[a].time;
    if (dt <= 0.0) {
        return glm::vec3(0.f);
    }
    return (m_samples[b].position - m_samples[a].position) / static_cast<f32>(dt);
}

std::optional<InterpolatedTransform> InterpolationBuffer::sample(f64 renderTime) const {
    if (m_samples.empty()) {
        return std::nullopt;
    }
    InterpolatedTransform out;
    const TransformSample& first = m_samples.front();
    const TransformSample& last = m_samples.back();
    if (renderTime <= first.time) {
        out.position = first.position;
        out.rotation = first.rotation;
        return out;
    }
    if (renderTime >= last.time) {
        const f64 dt = std::min(renderTime - last.time, m_maxExtrapolation);
        out.position = last.position + tangent(m_samples.size() - 1) * static_cast<f32>(dt);
        out.rotation = last.rotation;
        out.extrapolated = renderTime > last.time;
        return out;
    }
    auto it = std::upper_bound(m_samples.begin(), m_samples.end(), renderTime,
                               [](f64 t, const TransformSample& s) { return t < s.time; });
    const usize i1 = static_cast<usize>(it - m_samples.begin());
    const usize i0 = i1 - 1;
    const TransformSample& s0 = m_samples[i0];
    const TransformSample& s1 = m_samples[i1];
    const f64 span = s1.time - s0.time;
    const f32 t = static_cast<f32>((renderTime - s0.time) / span);
    const glm::vec3 m0 = tangent(i0) * static_cast<f32>(span);
    const glm::vec3 m1 = tangent(i1) * static_cast<f32>(span);
    const f32 t2 = t * t, t3 = t2 * t;
    const f32 h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    out.position = h00 * s0.position + h10 * m0 + h01 * s1.position + h11 * m1;
    out.rotation = glm::slerp(s0.rotation, s1.rotation, t);
    return out;
}

void ClockSync::addSample(f64 clientSendTime, f64 serverTime, f64 clientReceiveTime) {
    const f64 rtt = clientReceiveTime - clientSendTime;
    if (rtt < 0.0) {
        return;
    }
    m_samples.push_back({rtt, serverTime - (clientSendTime + clientReceiveTime) * 0.5});
    while (m_samples.size() > m_window) {
        m_samples.pop_front();
    }
    const Sample best = *std::min_element(m_samples.begin(), m_samples.end(),
                                          [](const Sample& a, const Sample& b) { return a.rtt < b.rtt; });
    m_bestRtt = best.rtt;
    // Snap on the first samples or big errors (e.g. server restart), otherwise slew.
    if (m_count < 4 || std::abs(best.offset - m_offset) > 0.25) {
        m_offset = best.offset;
    } else {
        m_offset += (best.offset - m_offset) * 0.25;
    }
    ++m_count;
}

} // namespace ox::net
