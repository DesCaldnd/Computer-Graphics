#pragma once

#include <oxwald/audio/audio_bus.hpp>
#include <oxwald/audio/audio_types.hpp>
#include <oxwald/audio/occlusion.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace ox::audio {

struct AudioEngineConfig {
    // Offline = no device: audio is produced only by render() (tests, tools, bouncing to file).
    bool offline = false;
    u32 sampleRate = 48000; // 0 = device native (device mode only)
    u32 channels = 2;
    u32 periodFrames = 0;   // 0 = backend default
    u32 maxVoices = 64;
    // Creates Master → Music / SFX / Voice / UI / Ambience.
    bool createDefaultBuses = true;
    // Occlusion response.
    f32 occlusionVolume = 0.35f;   // gain at full occlusion
    f32 occlusionCutoff = 900.f;   // low-pass cutoff at full occlusion (Hz)
    f32 occlusionSmoothing = 8.f;  // 1/s, exponential approach of the per-voice occlusion value
};

// The audio service. Game-thread API; the mix runs on the device thread or inside render().
// Call update(dt) once per frame: it frees finished voices, applies 3D/occlusion parameters,
// custom attenuation curves and mixer snapshot transitions.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init(const AudioEngineConfig& config = {});
    void shutdown();
    [[nodiscard]] bool initialized() const;
    [[nodiscard]] bool offline() const;
    [[nodiscard]] u32 sampleRate() const;
    [[nodiscard]] u32 channels() const;

    // Offline mode: mixes `frameCount` interleaved frames into `out` (size frameCount * channels()).
    u64 render(f32* out, u64 frameCount);
    // Convenience: renders `seconds` worth of audio, calling update(dt) between blocks of `blockFrames`.
    std::vector<f32> renderSeconds(f32 seconds, u32 blockFrames = 512);

    void update(f32 dt);
    [[nodiscard]] f64 timeSeconds() const;

    // ---- sound data ---------------------------------------------------------------------------
    SoundId loadSound(const std::string& path, LoadMode mode = LoadMode::Decode);
    SoundId createFromPcm(std::span<const f32> interleaved, u32 channels, u32 sampleRate);
    SoundId createSine(f32 frequency, f32 amplitude = 0.5f);
    SoundId createNoise(NoiseType type = NoiseType::White, f32 amplitude = 0.5f, i32 seed = 1);
    void unloadSound(SoundId id);
    [[nodiscard]] f32 soundDuration(SoundId id) const; // seconds; 0 for endless generators

    // ---- instances ----------------------------------------------------------------------------
    // Returns an invalid handle when the voice limit is reached and nothing could be stolen.
    SoundHandle play(SoundId id, const PlayParams& params = {});
    void stop(SoundHandle h, f32 fadeOutSeconds = 0.f);
    void stopAll(f32 fadeOutSeconds = 0.f);
    void pause(SoundHandle h);
    void resume(SoundHandle h);
    [[nodiscard]] bool isValid(SoundHandle h) const;
    [[nodiscard]] bool isPlaying(SoundHandle h) const; // true while scheduled/audible, false if paused/finished
    void setVolume(SoundHandle h, f32 volume);
    void fadeTo(SoundHandle h, f32 volume, f32 seconds);
    void setPitch(SoundHandle h, f32 pitch);
    void setPan(SoundHandle h, f32 pan);
    void setLooping(SoundHandle h, bool loop);
    void setSpatial(SoundHandle h, const Spatial3D& spatial);
    void setPosition(SoundHandle h, const glm::vec3& position, const glm::vec3& velocity = glm::vec3(0.f));
    void setDirection(SoundHandle h, const glm::vec3& direction);
    [[nodiscard]] f32 occlusion(SoundHandle h) const;     // smoothed 0..1
    [[nodiscard]] f32 audibility(SoundHandle h) const;    // estimated gain used for voice stealing
    [[nodiscard]] u32 activeVoiceCount() const;
    [[nodiscard]] u32 maxVoices() const;

    // ---- listener / occlusion ------------------------------------------------------------------
    void setListener(const ListenerState& listener);
    [[nodiscard]] const ListenerState& listener() const;
    void setOcclusionProvider(IAudioOcclusionProvider* provider); // not owned

    // ---- mixer ---------------------------------------------------------------------------------
    [[nodiscard]] AudioBus* master();
    [[nodiscard]] AudioBus* bus(std::string_view name);
    AudioBus* createBus(const std::string& name, AudioBus* parent = nullptr); // parent null → Master
    void addDucking(const DuckingSettings& settings);
    void clearDucking();
    void defineSnapshot(const MixerSnapshot& snapshot);
    [[nodiscard]] MixerSnapshot captureSnapshot(const std::string& name) const;
    bool applySnapshot(const std::string& name, f32 transitionSeconds = 0.5f);

    // ---- debug ---------------------------------------------------------------------------------
    // 3D sources: cross at the position, min (yellow) / max (red) distance rings; listener axes.
    void debugDraw(const DebugLineFn& line) const;

private:
    std::unique_ptr<detail::EngineImpl> m_impl;
};

} // namespace ox::audio
