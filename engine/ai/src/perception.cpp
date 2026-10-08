#include <oxwald/ai/perception.hpp>

#include <algorithm>
#include <cmath>

namespace ox::ai {

TeamAttitudes::TeamAttitudes() {
    for (usize a = 0; a < 256; ++a) {
        for (usize b = 0; b < 256; ++b) {
            m_table[a][b] = (a == 0 || b == 0) ? Attitude::Neutral : (a == b ? Attitude::Friendly : Attitude::Hostile);
        }
    }
}

void TeamAttitudes::set(TeamId a, TeamId b, Attitude attitude, bool symmetric) {
    m_table[a][b] = attitude;
    if (symmetric) {
        m_table[b][a] = attitude;
    }
}

Attitude TeamAttitudes::get(TeamId observer, TeamId target) const { return m_table[observer][target]; }

PerceptionListenerId PerceptionSystem::addListener(const PerceptionListenerDesc& desc) {
    const PerceptionListenerId id = m_nextListener++;
    m_listeners[id].desc = desc;
    return id;
}

void PerceptionSystem::removeListener(PerceptionListenerId id) { m_listeners.erase(id); }

void PerceptionSystem::setListenerTransform(PerceptionListenerId id, const glm::vec3& position, const glm::vec3& forward) {
    if (auto it = m_listeners.find(id); it != m_listeners.end()) {
        it->second.desc.position = position;
        it->second.desc.forward = forward;
    }
}

PerceptionListenerDesc* PerceptionSystem::listener(PerceptionListenerId id) {
    auto it = m_listeners.find(id);
    return it != m_listeners.end() ? &it->second.desc : nullptr;
}

void PerceptionSystem::setSource(const PerceptionSource& source) { m_sources[source.id] = source; }
void PerceptionSystem::removeSource(u64 id) { m_sources.erase(id); }
void PerceptionSystem::reportNoise(const NoiseEvent& noise) { m_pendingNoises.push_back(noise); }

bool PerceptionSystem::detects(const ListenerState& l, Attitude a) const {
    switch (a) {
    case Attitude::Hostile: return l.desc.detectHostile;
    case Attitude::Neutral: return l.desc.detectNeutral;
    case Attitude::Friendly: return l.desc.detectFriendly;
    }
    return false;
}

PerceivedStimulus* PerceptionSystem::findMemory(ListenerState& l, u64 sourceId, Sense sense) {
    for (auto& m : l.memory) {
        if (m.sourceId == sourceId && m.sense == sense) {
            return &m;
        }
    }
    return nullptr;
}

void PerceptionSystem::update(f32 dt) {
    for (auto& [lid, l] : m_listeners) {
        const PerceptionListenerDesc& d = l.desc;
        for (auto& m : l.memory) {
            m.age += dt;
        }

        // ---- sight ----
        if (d.sight.enabled) {
            const glm::vec3 eye = d.position + glm::vec3(0.f, d.sight.eyeHeight, 0.f);
            const glm::vec3 fwd = glm::length(d.forward) > 1e-5f ? glm::normalize(d.forward) : glm::vec3(0, 0, -1);
            const f32 cosMain = std::cos(glm::radians(d.sight.fovDegrees * 0.5f));
            const f32 cosPeriph = std::cos(glm::radians(d.sight.peripheralFovDegrees * 0.5f));
            for (const auto& [sid, src] : m_sources) {
                if (sid == d.selfId || !src.visible) {
                    continue;
                }
                const Attitude att = m_attitudes.get(d.team, src.team);
                if (!detects(l, att)) {
                    continue;
                }
                PerceivedStimulus* mem = findMemory(l, sid, Sense::Sight);
                const bool wasSeen = mem != nullptr && mem->currentlySensed;
                const glm::vec3 target = src.position + glm::vec3(0.f, d.sight.targetHeight, 0.f);
                const glm::vec3 to = target - eye;
                const f32 dist = glm::length(to);
                const f32 cosAngle = dist > 1e-5f ? glm::dot(to / dist, fwd) : 1.f;
                const f32 mainRange = wasSeen ? std::max(d.sight.loseSightRange, d.sight.range) : d.sight.range;
                f32 strength = 0.f;
                if (cosAngle >= cosMain && dist <= mainRange) {
                    strength = std::clamp(1.f - dist / std::max(mainRange, 1e-3f), 0.2f, 1.f);
                } else if (cosAngle >= cosPeriph && dist <= d.sight.peripheralRange) {
                    strength = 0.5f * std::clamp(1.f - dist / std::max(d.sight.peripheralRange, 1e-3f), 0.2f, 1.f);
                }
                if (strength > 0.f && m_raycast && m_raycast(eye, target)) {
                    strength = 0.f; // blocked line of sight
                }
                if (strength > 0.f) {
                    if (mem == nullptr) {
                        l.memory.push_back({});
                        mem = &l.memory.back();
                        mem->sourceId = sid;
                        mem->sense = Sense::Sight;
                    }
                    mem->lastKnownPosition = src.position;
                    mem->age = 0.f;
                    mem->strength = strength;
                    mem->attitude = att;
                    mem->team = src.team;
                    mem->currentlySensed = true;
                    if (!wasSeen && m_onEvent) {
                        m_onEvent(lid, *mem, true);
                    }
                } else if (wasSeen) {
                    mem->currentlySensed = false;
                    if (m_onEvent) {
                        m_onEvent(lid, *mem, false);
                    }
                }
            }
        }

        // ---- hearing ----
        if (d.hearing.enabled) {
            const glm::vec3 ear = d.position + glm::vec3(0.f, d.sight.eyeHeight, 0.f);
            for (const NoiseEvent& n : m_pendingNoises) {
                if (n.instigator != 0 && n.instigator == d.selfId) {
                    continue;
                }
                const Attitude att = m_attitudes.get(d.team, n.team);
                if (!detects(l, att)) {
                    continue;
                }
                const f32 radius = n.radius * d.hearing.rangeMultiplier;
                const f32 dist = glm::distance(d.position, n.position);
                if (dist > radius || radius <= 0.f) {
                    continue;
                }
                f32 loudness = n.loudness * (1.f - dist / radius);
                if (d.hearing.useOcclusion && m_raycast && m_raycast(ear, n.position + glm::vec3(0.f, 0.5f, 0.f))) {
                    loudness *= d.hearing.occludedFactor;
                }
                if (loudness < d.hearing.threshold) {
                    continue;
                }
                const u64 sid = n.instigator != 0 ? n.instigator : m_anonymousNoiseId++;
                PerceivedStimulus* mem = findMemory(l, sid, Sense::Hearing);
                const bool fresh = mem == nullptr;
                if (mem == nullptr) {
                    l.memory.push_back({});
                    mem = &l.memory.back();
                    mem->sourceId = sid;
                    mem->sense = Sense::Hearing;
                }
                mem->lastKnownPosition = n.position;
                mem->age = 0.f;
                mem->strength = std::max(fresh ? 0.f : mem->strength, std::min(loudness, 1.f));
                mem->attitude = att;
                mem->team = n.team;
                mem->tag = n.tag;
                mem->currentlySensed = true; // a noise is "sensed" only during the update it was heard
                if (m_onEvent) {
                    m_onEvent(lid, *mem, true);
                }
            }
        }

        // ---- memory decay / forgetting ----
        for (auto& m : l.memory) {
            if (m.sense == Sense::Hearing && m.age > 0.f) {
                m.currentlySensed = false;
            }
            if (!m.currentlySensed) {
                const f32 forget = m.sense == Sense::Sight ? d.sight.forgetAfter : d.hearing.forgetAfter;
                m.strength = std::max(0.f, m.strength - (forget > 0.f ? dt / forget : 1.f));
            }
        }
        std::erase_if(l.memory, [&](const PerceivedStimulus& m) {
            const f32 forget = m.sense == Sense::Sight ? d.sight.forgetAfter : d.hearing.forgetAfter;
            return !m.currentlySensed && m.age > forget;
        });
    }
    m_pendingNoises.clear();
}

const std::vector<PerceivedStimulus>& PerceptionSystem::perceived(PerceptionListenerId id) const {
    static const std::vector<PerceivedStimulus> kEmpty;
    auto it = m_listeners.find(id);
    return it != m_listeners.end() ? it->second.memory : kEmpty;
}

std::optional<PerceivedStimulus> PerceptionSystem::knowledgeOf(PerceptionListenerId id, u64 sourceId, Sense sense) const {
    for (const auto& m : perceived(id)) {
        if (m.sourceId == sourceId && m.sense == sense) {
            return m;
        }
    }
    return std::nullopt;
}

bool PerceptionSystem::canSee(PerceptionListenerId id, u64 sourceId) const {
    const auto k = knowledgeOf(id, sourceId, Sense::Sight);
    return k && k->currentlySensed;
}

std::optional<PerceivedStimulus> PerceptionSystem::bestHostile(PerceptionListenerId id) const {
    std::optional<PerceivedStimulus> best;
    for (const auto& m : perceived(id)) {
        if (m.attitude != Attitude::Hostile) {
            continue;
        }
        auto score = [](const PerceivedStimulus& s) {
            return (s.currentlySensed ? 2.f : 0.f) + (s.sense == Sense::Sight ? 1.f : 0.f) + s.strength;
        };
        if (!best || score(m) > score(*best)) {
            best = m;
        }
    }
    return best;
}

void PerceptionSystem::debugDraw(PerceptionListenerId id, const DebugLineFn& line) const {
    auto it = m_listeners.find(id);
    if (it == m_listeners.end() || !line) {
        return;
    }
    const PerceptionListenerDesc& d = it->second.desc;
    const glm::vec3 eye = d.position + glm::vec3(0.f, d.sight.eyeHeight, 0.f);
    const glm::vec3 fwd = glm::length(d.forward) > 1e-5f ? glm::normalize(d.forward) : glm::vec3(0, 0, -1);
    auto cone = [&](f32 fovDeg, f32 range, const glm::vec4& color) {
        const f32 half = glm::radians(fovDeg * 0.5f);
        constexpr int kSeg = 12;
        glm::vec3 prev{};
        for (int i = 0; i <= kSeg; ++i) {
            const f32 a = -half + 2.f * half * static_cast<f32>(i) / kSeg;
            const glm::vec3 dir(fwd.x * std::cos(a) - fwd.z * std::sin(a), 0.f, fwd.x * std::sin(a) + fwd.z * std::cos(a));
            const glm::vec3 p = eye + dir * range;
            if (i == 0 || i == kSeg) {
                line(eye, p, color);
            }
            if (i > 0) {
                line(prev, p, color);
            }
            prev = p;
        }
    };
    if (d.sight.enabled) {
        cone(d.sight.fovDegrees, d.sight.range, {0.f, 1.f, 0.f, 1.f});
        cone(d.sight.peripheralFovDegrees, d.sight.peripheralRange, {0.f, 0.5f, 0.f, 1.f});
    }
    for (const auto& m : it->second.memory) {
        const glm::vec4 c = m.currentlySensed ? glm::vec4(1, 0, 0, 1) : glm::vec4(1, 1, 0, 1);
        line(eye, m.lastKnownPosition, glm::vec4(c.r, c.g, c.b, 0.3f));
        line(m.lastKnownPosition, m.lastKnownPosition + glm::vec3(0, 2, 0), c);
    }
}

} // namespace ox::ai
