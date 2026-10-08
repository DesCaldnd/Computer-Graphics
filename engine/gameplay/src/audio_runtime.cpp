#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/audio.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/providers.hpp>

#include <cmath>

namespace ox::gameplay {

f32 PhysicsOcclusionProvider::occlusion(const glm::vec3& listener, const glm::vec3& source) {
    const u32 hits = m_physics.countHits(listener, source, [](Entity e) {
        return e.hasAny<AudioSourceComponent, AudioListenerComponent>();
    });
    return 1.f - std::pow(1.f - m_perHit, static_cast<f32>(hits));
}

AudioRuntime::AudioRuntime() = default;
AudioRuntime::~AudioRuntime() { detach(); }

void AudioRuntime::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_services = &services;
    m_engine = services.tryGet<audio::AudioEngine>();
    m_clips = services.tryGet<IAudioClipProvider>();
    if (m_engine) {
        if (auto* physics = services.tryGet<PhysicsRuntime>()) {
            m_occlusion = std::make_unique<PhysicsOcclusionProvider>(*physics);
            m_engine->setOcclusionProvider(m_occlusion.get());
        }
    }
    entt::registry& r = world.registry();
    m_connections.emplace_back(
        r.on_destroy<AudioSourceComponent>().connect<&AudioRuntime::onSourceDestroyed>(*this));
    m_connections.emplace_back(
        r.on_construct<AudioSourceComponent>().connect<&AudioRuntime::onSourceConstructed>(*this));
}

void AudioRuntime::detach() {
    if (!m_world) return;
    syncPlayState(false);
    m_connections.clear();
    if (m_engine && m_occlusion) m_engine->setOcclusionProvider(nullptr);
    m_occlusion.reset();
    m_world = nullptr;
    m_engine = nullptr;
    m_listenerInit = false;
}

void AudioRuntime::onSourceDestroyed(entt::registry&, entt::entity e) {
    auto it = m_sources.find(e);
    if (it == m_sources.end()) return;
    if (m_engine && it->second.handle) m_engine->stop(it->second.handle);
    m_sources.erase(it);
}

void AudioRuntime::onSourceConstructed(entt::registry&, entt::entity e) { m_sources.erase(e); }

void AudioRuntime::syncPlayState(bool playing) {
    if (playing == m_playing) return;
    m_playing = playing;
    if (!playing) {
        for (auto& [e, rec] : m_sources) {
            if (m_engine && rec.handle) m_engine->stop(rec.handle);
            if (m_world && m_world->valid(e)) {
                if (auto* c = m_world->registry().try_get<AudioSourceComponent>(e)) c->playing = false;
            }
        }
        m_sources.clear();
    }
}

audio::SoundId AudioRuntime::resolve(const Uuid& clip, const std::string& path, bool stream) {
    if (!m_engine) return {};
    if (clip.isValid()) {
        if (!m_clips && m_services) m_clips = m_services->tryGet<IAudioClipProvider>();
        if (m_clips) {
            if (auto id = m_clips->sound(*m_engine, clip)) return id;
        }
        OX_LOG_WARN("gameplay", "audio clip {} not available", clip.toString());
    }
    if (path.empty()) return {};
    auto it = m_loaded.find(path);
    if (it != m_loaded.end()) return it->second;
    const audio::SoundId id = m_engine->loadSound(path, stream ? audio::LoadMode::Stream : audio::LoadMode::Decode);
    if (id) m_loaded.emplace(path, id);
    return id;
}

audio::PlayParams AudioRuntime::paramsFor(const AudioSourceComponent& c, const glm::vec3& position,
                                          const glm::vec3& forward) const {
    audio::PlayParams p;
    p.bus = c.bus;
    p.volume = c.volume;
    p.pitch = c.pitch;
    p.loop = c.loop;
    p.priority = c.priority;
    p.fadeInSeconds = c.fadeInSeconds;
    p.spatial.enabled = c.spatial;
    p.spatial.position = position;
    p.spatial.direction = forward;
    p.spatial.attenuation = c.attenuation;
    p.spatial.minDistance = c.minDistance;
    p.spatial.maxDistance = c.maxDistance;
    p.spatial.rolloff = c.rolloff;
    p.spatial.coneInnerAngle = glm::radians(c.coneInnerAngle);
    p.spatial.coneOuterAngle = glm::radians(c.coneOuterAngle);
    p.spatial.coneOuterGain = c.coneOuterGain;
    p.spatial.dopplerFactor = c.dopplerFactor;
    p.spatial.distanceLowPass = c.distanceLowPass;
    p.spatial.occlusion = c.occlusion;
    return p;
}

bool AudioRuntime::play(Entity e) {
    if (!m_engine || !m_engine->initialized() || !e.valid()) return false;
    auto* c = e.tryGet<AudioSourceComponent>();
    if (!c) return false;
    SourceRecord& rec = m_sources[e.handle()];
    if (rec.handle && m_engine->isValid(rec.handle)) m_engine->stop(rec.handle);
    const audio::SoundId id = resolve(c->clip, c->clipPath, c->stream);
    if (!id) {
        OX_LOG_WARN("gameplay", "audio source '{}': no clip", e.name());
        rec.started = true;
        return false;
    }
    const Transform wt = e.worldTransform();
    rec.handle = m_engine->play(id, paramsFor(*c, wt.position, wt.forward()));
    rec.lastPosition = wt.position;
    rec.started = true;
    c->playing = rec.handle.valid();
    return c->playing;
}

void AudioRuntime::stop(Entity e, f32 fadeOut) {
    if (!e.valid()) return;
    auto it = m_sources.find(e.handle());
    if (it == m_sources.end()) return;
    if (m_engine && it->second.handle) m_engine->stop(it->second.handle, fadeOut);
    it->second.handle = {};
    if (auto* c = e.tryGet<AudioSourceComponent>()) c->playing = false;
}

audio::SoundHandle AudioRuntime::handleOf(Entity e) const {
    if (!e.valid()) return {};
    auto it = m_sources.find(e.handle());
    return it == m_sources.end() ? audio::SoundHandle{} : it->second.handle;
}

audio::SoundHandle AudioRuntime::playOneShot(const std::string& clipPath, const glm::vec3* position, f32 volume,
                                             const std::string& bus) {
    if (!m_engine || !m_engine->initialized()) return {};
    const audio::SoundId id = resolve({}, clipPath);
    if (!id) return {};
    audio::PlayParams p;
    p.bus = bus;
    p.volume = volume;
    if (position) {
        p.spatial.enabled = true;
        p.spatial.position = *position;
    }
    return m_engine->play(id, p);
}

void AudioRuntime::update(f32 dt) {
    if (!m_world || !m_engine || !m_engine->initialized()) return;
    OX_PROFILE_ZONE_N("AudioRuntime::update");
    entt::registry& r = m_world->registry();
    const f32 invDt = dt > 0.f ? 1.f / dt : 0.f;

    for (auto [handle, l] : r.view<AudioListenerComponent>().each()) {
        if (!l.active) continue;
        const Entity e = m_world->wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const Transform wt = e.worldTransform();
        audio::ListenerState s;
        s.position = wt.position;
        s.orientation = glm::normalize(wt.rotation);
        s.velocity = m_listenerInit ? (wt.position - m_listenerLast) * invDt : glm::vec3(0.f);
        m_listenerLast = wt.position;
        m_listenerInit = true;
        m_engine->setListener(s);
        break;
    }

    if (m_playing) {
        auto view = r.view<AudioSourceComponent>();
        for (auto handle : view) {
            auto& c = view.get<AudioSourceComponent>(handle);
            const Entity e = m_world->wrap(handle);
            auto [it, created] = m_sources.try_emplace(handle);
            SourceRecord& rec = it->second;
            if (!rec.started && c.playOnStart && e.activeInHierarchy()) play(e);
            rec.started = true;
            if (!rec.handle) continue;
            if (!m_engine->isValid(rec.handle)) {
                rec.handle = {};
                c.playing = false;
                continue;
            }
            const Transform wt = e.worldTransform();
            const glm::vec3 vel = (wt.position - rec.lastPosition) * invDt;
            rec.lastPosition = wt.position;
            if (c.spatial) {
                m_engine->setPosition(rec.handle, wt.position, vel);
                m_engine->setDirection(rec.handle, wt.forward());
            }
            m_engine->setVolume(rec.handle, c.volume);
            m_engine->setPitch(rec.handle, c.pitch);
        }
    }
    m_engine->update(dt);
}

void AudioRuntime::drawDebug(DebugDraw& draw) {
    if (!debugDraw || !m_engine || !m_engine->initialized()) return;
    m_engine->debugDraw([&](glm::vec3 a, glm::vec3 b, glm::vec4 c) { draw.line(a, b, c); });
}

} // namespace ox::gameplay
