#include "audio_internal.hpp"

#include <oxwald/core/log.hpp>

#include <algorithm>

namespace ox::audio {

using detail::BusInternals;
using detail::EffectSlot;

AudioBus::AudioBus(detail::EngineImpl& engine, std::string name, AudioBus* parent)
    : m_engine(engine), m_name(std::move(name)), m_parent(parent), m_internals(std::make_unique<BusInternals>()) {}

AudioBus::~AudioBus() {
    BusInternals& in = *m_internals;
    if (in.groupInit) {
        ma_sound_group_uninit(&in.group);
    }
    for (auto& slot : in.effects) {
        if (slot->nodeInit) {
            ma_node_uninit(&slot->node.base, nullptr);
        }
    }
    if (in.tailInit) {
        ma_node_uninit(&in.tail.base, nullptr);
    }
}

void AudioBus::setVolume(f32 linear) {
    m_volume = std::max(linear, 0.f);
    m_engine.refreshBusGains();
}

void AudioBus::setVolumeDb(f32 db) { setVolume(dbToLinear(db)); }

void AudioBus::setMuted(bool muted) {
    m_muted = muted;
    m_engine.refreshBusGains();
}

void AudioBus::setSolo(bool solo) {
    m_solo = solo;
    m_engine.refreshBusGains();
}

IAudioEffect* AudioBus::addEffect(std::unique_ptr<IAudioEffect> effect) {
    if (!effect) {
        return nullptr;
    }
    auto slot = std::make_unique<EffectSlot>();
    slot->effect = std::move(effect);
    if (!detail::initEffectNode(m_engine, *slot, m_internals->channels)) {
        OX_LOG_ERROR("audio", "bus '{}': failed to create effect node", m_name);
        return nullptr;
    }
    IAudioEffect* raw = slot->effect.get();
    m_internals->effects.push_back(std::move(slot));
    m_engine.rewireBus(*this);
    return raw;
}

bool AudioBus::removeEffect(IAudioEffect* effect) {
    auto& fx = m_internals->effects;
    auto it = std::find_if(fx.begin(), fx.end(), [&](const auto& s) { return s->effect.get() == effect; });
    if (it == fx.end()) {
        return false;
    }
    std::unique_ptr<EffectSlot> slot = std::move(*it);
    fx.erase(it);
    m_engine.rewireBus(*this);
    if (slot->nodeInit) {
        ma_node_uninit(&slot->node.base, nullptr);
    }
    return true;
}

void AudioBus::clearEffects() {
    while (!m_internals->effects.empty()) {
        removeEffect(m_internals->effects.back()->effect.get());
    }
}

usize AudioBus::effectCount() const { return m_internals->effects.size(); }

IAudioEffect* AudioBus::effect(usize index) const {
    return index < m_internals->effects.size() ? m_internals->effects[index]->effect.get() : nullptr;
}

f32 AudioBus::peak() const { return m_internals->peak.load(std::memory_order_relaxed); }
f32 AudioBus::rms() const { return m_internals->rms.load(std::memory_order_relaxed); }
f32 AudioBus::duckGain() const { return m_internals->duck.load(std::memory_order_relaxed); }

f32 AudioBus::effectiveGain() const {
    f32 g = 1.f;
    for (const AudioBus* b = this; b != nullptr; b = b->m_parent) {
        g *= b->m_internals->targetGain.load(std::memory_order_relaxed);
    }
    return g;
}

namespace detail {

AudioBus* EngineImpl::createBus(const std::string& name, AudioBus* parent) {
    if (findBus(name) != nullptr) {
        OX_LOG_WARN("audio", "bus '{}' already exists", name);
        return findBus(name);
    }
    auto bus = std::unique_ptr<AudioBus>(new AudioBus(*this, name, parent));
    BusInternals& in = *bus->m_internals;
    in.channels = ma_engine_get_channels(&engine);
    in.sampleRate = ma_engine_get_sample_rate(&engine);
    if (ma_sound_group_init(&engine, MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &in.group) !=
        MA_SUCCESS) {
        OX_LOG_ERROR("audio", "failed to create bus '{}'", name);
        return nullptr;
    }
    in.groupInit = true;
    if (!initBusTailNode(*this, in)) {
        OX_LOG_ERROR("audio", "failed to create bus '{}' fader", name);
        return nullptr;
    }
    AudioBus* raw = bus.get();
    if (parent != nullptr) {
        parent->m_children.push_back(raw);
    }
    buses.push_back(std::move(bus));
    rewireBus(*raw);
    rebuildDucking();
    refreshBusGains();
    return raw;
}

void EngineImpl::rewireBus(AudioBus& bus) {
    BusInternals& in = *bus.m_internals;
    ma_node* target = bus.m_parent != nullptr ? static_cast<ma_node*>(&bus.m_parent->m_internals->group)
                                              : ma_engine_get_endpoint(&engine);
    ma_node* prev = &in.group;
    for (auto& slot : in.effects) {
        ma_node_attach_output_bus(prev, 0, &slot->node.base, 0);
        prev = &slot->node.base;
    }
    ma_node_attach_output_bus(prev, 0, &in.tail.base, 0);
    ma_node_attach_output_bus(&in.tail.base, 0, target, 0);
}

void EngineImpl::refreshBusGains() {
    const bool anySolo = std::any_of(buses.begin(), buses.end(), [](const auto& b) { return b->m_solo; });
    auto soloRelated = [](const AudioBus* bus) {
        for (const AudioBus* b = bus; b != nullptr; b = b->m_parent) {
            if (b->m_solo) {
                return true;
            }
        }
        // descendant soloed?
        std::vector<const AudioBus*> stack(bus->m_children.begin(), bus->m_children.end());
        while (!stack.empty()) {
            const AudioBus* b = stack.back();
            stack.pop_back();
            if (b->m_solo) {
                return true;
            }
            stack.insert(stack.end(), b->m_children.begin(), b->m_children.end());
        }
        return false;
    };
    for (auto& b : buses) {
        f32 g = b->m_muted ? 0.f : b->m_volume;
        if (anySolo && !soloRelated(b.get())) {
            g = 0.f;
        }
        b->m_internals->targetGain.store(g, std::memory_order_relaxed);
    }
}

void EngineImpl::rebuildDucking() {
    for (auto& b : buses) {
        BusInternals& in = *b->m_internals;
        std::vector<DuckRule> rules;
        for (const DuckingSettings& d : ducking) {
            if (d.targetBus != b->m_name) {
                continue;
            }
            AudioBus* side = findBus(d.sidechainBus);
            if (side == nullptr) {
                continue;
            }
            rules.push_back({side->m_internals.get(), d.threshold, d.duckVolume, d.attackSeconds, d.releaseSeconds});
        }
        std::lock_guard lock(in.duckMutex);
        in.duckRules = std::move(rules);
    }
}

AudioBus* EngineImpl::findBus(std::string_view name) const {
    for (const auto& b : buses) {
        if (b->m_name == name) {
            return b.get();
        }
    }
    return nullptr;
}

} // namespace detail
} // namespace ox::audio
