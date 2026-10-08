#include <oxwald/physics/interpolation.hpp>
#include <oxwald/physics/physics_world.hpp>

#include <algorithm>
#include <cmath>

namespace ox::physics {

u32 FixedStepper::advance(f32 frameDt) {
    m_accumulator += std::max(0.f, frameDt);
    u32 steps = 0;
    while (m_accumulator >= m_dt && steps < m_maxSteps) {
        m_accumulator -= m_dt;
        ++steps;
    }
    if (steps == m_maxSteps && m_accumulator >= m_dt) {
        m_accumulator = std::fmod(m_accumulator, m_dt);
    }
    return steps;
}

void TransformInterpolator::track(BodyHandle body) { m_entries.try_emplace(body); }
void TransformInterpolator::untrack(BodyHandle body) { m_entries.erase(body); }

void TransformInterpolator::capture(const PhysicsWorld& world) {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (!world.isValid(it->first)) {
            it = m_entries.erase(it);
            continue;
        }
        Entry& e = it->second;
        Transform now = world.getTransform(it->first);
        e.previous = e.initialized ? e.current : now;
        e.current = now;
        e.initialized = true;
        ++it;
    }
}

void TransformInterpolator::reset(BodyHandle body, const Transform& transform) {
    Entry& e = m_entries[body];
    e.previous = e.current = transform;
    e.initialized = true;
}

Transform TransformInterpolator::get(BodyHandle body, f32 alpha) const {
    auto it = m_entries.find(body);
    if (it == m_entries.end()) {
        return {};
    }
    return interpolate(it->second.previous, it->second.current, alpha);
}

const Transform* TransformInterpolator::previous(BodyHandle body) const {
    auto it = m_entries.find(body);
    return it == m_entries.end() ? nullptr : &it->second.previous;
}

const Transform* TransformInterpolator::current(BodyHandle body) const {
    auto it = m_entries.find(body);
    return it == m_entries.end() ? nullptr : &it->second.current;
}

Transform TransformInterpolator::interpolate(const Transform& a, const Transform& b, f32 alpha) {
    alpha = std::clamp(alpha, 0.f, 1.f);
    Transform t;
    t.position = a.position + (b.position - a.position) * alpha;
    glm::quat qb = glm::dot(a.rotation, b.rotation) < 0.f ? -b.rotation : b.rotation;
    t.rotation = glm::normalize(glm::slerp(a.rotation, qb, alpha));
    return t;
}

} // namespace ox::physics
