#pragma once

#include <oxwald/audio/audio_engine.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/scene/world.hpp>

#include <entt/signal/sigh.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox {
class DebugDraw;
}

namespace ox::gameplay {

class PhysicsRuntime;
class IAudioClipProvider;

// ---- components ------------------------------------------------------------------------------------------

struct AudioSourceComponent {
    Uuid clip;            // AudioClip asset (IAudioClipProvider)
    std::string clipPath; // used when clip is nil (loaded through AudioEngine::loadSound, cached)
    bool stream = false;  // LoadMode::Stream for clipPath
    std::string bus = "SFX";
    f32 volume = 1.f;
    f32 pitch = 1.f;
    bool loop = false;
    bool playOnStart = true;
    bool spatial = true;
    audio::AttenuationModel attenuation = audio::AttenuationModel::Inverse;
    f32 minDistance = 1.f;
    f32 maxDistance = 100.f;
    f32 rolloff = 1.f;
    f32 coneInnerAngle = 360.f; // degrees
    f32 coneOuterAngle = 360.f;
    f32 coneOuterGain = 1.f;
    f32 dopplerFactor = 1.f;
    bool distanceLowPass = false;
    bool occlusion = false; // physics raycast occlusion
    i32 priority = 0;
    f32 fadeInSeconds = 0.f;
    // runtime
    bool playing = false;
};

// The (first active) listener; usually on the camera entity.
struct AudioListenerComponent {
    bool active = true;
};

// ---- runtime -------------------------------------------------------------------------------------------

// Occlusion provider counting physics bodies between listener and source (audio sources/listeners ignored).
class PhysicsOcclusionProvider final : public audio::IAudioOcclusionProvider {
public:
    PhysicsOcclusionProvider(PhysicsRuntime& physics, f32 perHitOcclusion = 0.6f)
        : m_physics(physics), m_perHit(perHitOcclusion) {}
    [[nodiscard]] f32 occlusion(const glm::vec3& listener, const glm::vec3& source) override;

private:
    PhysicsRuntime& m_physics;
    f32 m_perHit;
};

class AudioRuntime {
public:
    AudioRuntime();
    ~AudioRuntime();
    AudioRuntime(const AudioRuntime&) = delete;
    AudioRuntime& operator=(const AudioRuntime&) = delete;

    [[nodiscard]] audio::AudioEngine* engine() const { return m_engine; }
    bool play(Entity source);
    void stop(Entity source, f32 fadeOut = 0.f);
    [[nodiscard]] audio::SoundHandle handleOf(Entity source) const;
    // One-shot (non-entity) sound; `spatial` uses `position`. Path or registered name.
    audio::SoundHandle playOneShot(const std::string& clipPath, const glm::vec3* position, f32 volume = 1.f,
                                   const std::string& bus = "SFX");
    audio::SoundId resolve(const Uuid& clip, const std::string& path, bool stream = false);

    bool debugDraw = false;

    // ---- driven by the gameplay systems ----
    void attach(World& world, Services& services);
    void detach();
    void syncPlayState(bool playing);
    void update(f32 dt);
    void drawDebug(DebugDraw& draw);

private:
    struct SourceRecord {
        audio::SoundHandle handle;
        glm::vec3 lastPosition{0.f};
        bool started = false;
    };
    void onSourceDestroyed(entt::registry& r, entt::entity e);
    void onSourceConstructed(entt::registry& r, entt::entity e);
    audio::PlayParams paramsFor(const AudioSourceComponent& c, const glm::vec3& position, const glm::vec3& forward) const;

    World* m_world = nullptr;
    Services* m_services = nullptr;
    audio::AudioEngine* m_engine = nullptr;
    IAudioClipProvider* m_clips = nullptr;
    std::unique_ptr<PhysicsOcclusionProvider> m_occlusion;
    std::unordered_map<entt::entity, SourceRecord> m_sources;
    std::unordered_map<std::string, audio::SoundId> m_loaded;
    std::vector<entt::scoped_connection> m_connections;
    glm::vec3 m_listenerLast{0.f};
    bool m_listenerInit = false;
    bool m_playing = false;
};

} // namespace ox::gameplay
