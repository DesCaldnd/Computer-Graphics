#include "audio_internal.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/vfs.hpp>

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <tuple>

namespace ox::audio {

using namespace detail;

f32 evaluateAttenuation(const Spatial3D& s, f32 distance) {
    const f32 minD = s.minDistance, maxD = s.maxDistance;
    switch (s.attenuation) {
    case AttenuationModel::None:
        return 1.f;
    case AttenuationModel::Inverse:
        if (minD >= maxD) {
            return 1.f;
        }
        return minD / (minD + s.rolloff * (std::clamp(distance, minD, maxD) - minD));
    case AttenuationModel::Linear:
        if (minD >= maxD) {
            return 1.f;
        }
        return std::clamp(1.f - s.rolloff * (std::clamp(distance, minD, maxD) - minD) / (maxD - minD), 0.f, 1.f);
    case AttenuationModel::Exponential:
        if (minD >= maxD || minD <= 0.f) {
            return 1.f;
        }
        return std::pow(std::clamp(distance, minD, maxD) / minD, -s.rolloff);
    case AttenuationModel::Custom: {
        const auto& c = s.customCurve;
        if (c.empty()) {
            return 1.f;
        }
        if (distance <= c.front().distance) {
            return c.front().gain;
        }
        for (usize i = 1; i < c.size(); ++i) {
            if (distance <= c[i].distance) {
                const f32 span = c[i].distance - c[i - 1].distance;
                const f32 t = span > 0.f ? (distance - c[i - 1].distance) / span : 1.f;
                return c[i - 1].gain + (c[i].gain - c[i - 1].gain) * t;
            }
        }
        return c.back().gain;
    }
    }
    return 1.f;
}

namespace {

// miniaudio's `none` model bypasses the whole spatializer (no panning, no master volume), so None/Custom
// use a linear model with zero rolloff (gain 1) and apply their gain through the sound volume.
ma_attenuation_model toMa(AttenuationModel m) {
    switch (m) {
    case AttenuationModel::Inverse: return ma_attenuation_model_inverse;
    case AttenuationModel::Exponential: return ma_attenuation_model_exponential;
    default: return ma_attenuation_model_linear;
    }
}

bool usesEngineAttenuation(AttenuationModel m) {
    return m == AttenuationModel::Inverse || m == AttenuationModel::Linear || m == AttenuationModel::Exponential;
}

f32 logLerp(f32 a, f32 b, f32 t) { return std::exp(std::log(a) + (std::log(b) - std::log(a)) * std::clamp(t, 0.f, 1.f)); }

u64 secondsToFrames(f32 s, u32 sampleRate) { return static_cast<u64>(std::max(s, 0.f) * static_cast<f32>(sampleRate) + 0.5f); }

} // namespace

// ------------------------------------------------------------------------------------------------
// EngineImpl voice helpers

Voice* EngineImpl::resolve(SoundHandle h) const {
    if (!h.valid() || h.index >= voices.size()) {
        return nullptr;
    }
    Voice* v = voices[h.index].get();
    return (v->active && v->generation == h.generation) ? v : nullptr;
}

void EngineImpl::freeVoice(Voice& v) {
    if (v.soundInit) {
        ma_sound_uninit(&v.sound);
        v.soundInit = false;
    }
    if (v.filterInit) {
        ma_node_uninit(&v.filterNode.base, nullptr);
        v.filterInit = false;
    }
    if (v.hasWaveform) {
        ma_waveform_uninit(&v.waveform);
        v.hasWaveform = false;
    }
    if (v.hasNoise) {
        ma_noise_uninit(&v.noise, nullptr);
        v.hasNoise = false;
    }
    if (v.hasBufferRef) {
        ma_audio_buffer_ref_uninit(&v.bufferRef);
        v.hasBufferRef = false;
    }
    v.active = false;
    ++v.generation;
}

void EngineImpl::applySpatial(Voice& v) {
    const Spatial3D& s = v.params.spatial;
    ma_sound* snd = &v.sound;
    ma_sound_set_spatialization_enabled(snd, s.enabled ? MA_TRUE : MA_FALSE);
    if (!s.enabled) {
        ma_sound_set_pan(snd, v.params.pan);
        ma_sound_set_volume(snd, v.params.volume);
        v.filter.cutoff.store(20000.f);
        return;
    }
    ma_sound_set_pan(snd, 0.f);
    ma_sound_set_positioning(snd, ma_positioning_absolute);
    ma_sound_set_position(snd, s.position.x, s.position.y, s.position.z);
    ma_sound_set_velocity(snd, s.velocity.x, s.velocity.y, s.velocity.z);
    const glm::vec3 dir = glm::length(s.direction) > 1e-6f ? glm::normalize(s.direction) : glm::vec3(0, 0, -1);
    ma_sound_set_direction(snd, dir.x, dir.y, dir.z);
    ma_sound_set_attenuation_model(snd, toMa(s.attenuation));
    ma_sound_set_min_distance(snd, s.minDistance);
    ma_sound_set_max_distance(snd, s.maxDistance);
    ma_sound_set_rolloff(snd, usesEngineAttenuation(s.attenuation) ? s.rolloff : 0.f);
    ma_sound_set_cone(snd, s.coneInnerAngle, s.coneOuterAngle, s.coneOuterGain);
    ma_sound_set_doppler_factor(snd, s.dopplerFactor);
    updateVoice(v, 0.f);
}

f32 EngineImpl::estimateAudibility(const PlayParams& p, const AudioBus* bus, f32 occ, f32 fade) const {
    f32 g = p.volume * fade;
    if (p.spatial.enabled) {
        g *= evaluateAttenuation(p.spatial, glm::distance(p.spatial.position, listener.position));
        g *= 1.f + (config.occlusionVolume - 1.f) * occ;
    }
    if (bus != nullptr) {
        g *= bus->effectiveGain();
    }
    return g;
}

void EngineImpl::updateVoice(Voice& v, f32 dt) {
    const Spatial3D& s = v.params.spatial;
    if (!s.enabled) {
        v.lastAudibility = estimateAudibility(v.params, v.bus, 0.f, ma_sound_get_current_fade_volume(&v.sound));
        return;
    }
    const f32 dist = glm::distance(s.position, listener.position);

    if (s.attenuation == AttenuationModel::Custom) {
        ma_sound_set_volume(&v.sound, v.params.volume * evaluateAttenuation(s, dist));
    } else {
        ma_sound_set_volume(&v.sound, v.params.volume);
    }

    if (s.occlusion && occlusionProvider != nullptr) {
        const f32 target = std::clamp(occlusionProvider->occlusion(listener.position, s.position), 0.f, 1.f);
        const f32 k = dt > 0.f ? 1.f - std::exp(-config.occlusionSmoothing * dt) : 1.f;
        v.occlusion += (target - v.occlusion) * k;
    } else {
        v.occlusion = 0.f;
    }

    const f32 nyquistSafe = 0.45f * static_cast<f32>(ma_engine_get_sample_rate(&engine));
    f32 cutoff = 20000.f;
    if (s.distanceLowPass && s.maxDistance > s.minDistance) {
        const f32 t = (dist - s.minDistance) / (s.maxDistance - s.minDistance);
        cutoff = std::min(cutoff, logLerp(std::max(s.lowPassNearCutoff, 20.f), std::max(s.lowPassFarCutoff, 20.f), t));
    }
    if (v.occlusion > 0.f) {
        cutoff = std::min(cutoff, logLerp(20000.f, std::max(config.occlusionCutoff, 20.f), v.occlusion));
    }
    v.filter.cutoff.store(std::min(cutoff, nyquistSafe * 2.f), std::memory_order_relaxed);
    v.filter.gain.store(1.f + (config.occlusionVolume - 1.f) * v.occlusion, std::memory_order_relaxed);
    v.lastAudibility = estimateAudibility(v.params, v.bus, v.occlusion, ma_sound_get_current_fade_volume(&v.sound));
}

// ------------------------------------------------------------------------------------------------
// AudioEngine

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(const AudioEngineConfig& config) {
    shutdown();
    m_impl = std::make_unique<EngineImpl>();
    EngineImpl& e = *m_impl;
    e.config = config;
    if (e.config.maxVoices == 0) {
        e.config.maxVoices = 1;
    }

    ma_engine_config cfg = ma_engine_config_init();
    cfg.channels = config.channels;
    cfg.sampleRate = config.sampleRate != 0 ? config.sampleRate : (config.offline ? 48000u : 0u);
    cfg.periodSizeInFrames = config.periodFrames;
    cfg.noDevice = config.offline ? MA_TRUE : MA_FALSE;
    if (config.offline && cfg.channels == 0) {
        cfg.channels = 2;
    }
    if (ma_engine_init(&cfg, &e.engine) != MA_SUCCESS) {
        OX_LOG_ERROR("audio", "failed to initialise miniaudio engine ({} mode)", config.offline ? "offline" : "device");
        m_impl.reset();
        return false;
    }
    e.engineInit = true;
    e.config.sampleRate = ma_engine_get_sample_rate(&e.engine);
    e.config.channels = ma_engine_get_channels(&e.engine);

    e.voices.reserve(e.config.maxVoices);
    for (u32 i = 0; i < e.config.maxVoices; ++i) {
        e.voices.push_back(std::make_unique<Voice>());
    }

    e.masterBus = e.createBus("Master", nullptr);
    if (config.createDefaultBuses && e.masterBus != nullptr) {
        for (const char* name : {"Music", "SFX", "Voice", "UI", "Ambience"}) {
            e.createBus(name, e.masterBus);
        }
    }
    setListener({});
    OX_LOG_INFO("audio", "audio engine ready: {} Hz, {} ch, {} voices{}", e.config.sampleRate, e.config.channels,
                e.config.maxVoices, config.offline ? " (offline)" : "");
    return true;
}

void AudioEngine::shutdown() {
    if (!m_impl) {
        return;
    }
    EngineImpl& e = *m_impl;
    for (auto& v : e.voices) {
        if (v->active) {
            e.freeVoice(*v);
        }
    }
    for (auto& [id, data] : e.sounds) {
        if (data->prototype) {
            ma_sound_uninit(data->prototype.get());
        }
    }
    e.sounds.clear();
    while (!e.buses.empty()) {
        e.buses.pop_back(); // children were created after their parents
    }
    if (e.engineInit) {
        ma_engine_uninit(&e.engine);
    }
    m_impl.reset();
}

bool AudioEngine::initialized() const { return m_impl != nullptr; }
bool AudioEngine::offline() const { return m_impl && m_impl->config.offline; }
u32 AudioEngine::sampleRate() const { return m_impl ? m_impl->config.sampleRate : 0; }
u32 AudioEngine::channels() const { return m_impl ? m_impl->config.channels : 0; }

u64 AudioEngine::render(f32* out, u64 frameCount) {
    if (!m_impl) {
        return 0;
    }
    if (!m_impl->config.offline) {
        OX_LOG_WARN("audio", "render() is only valid in offline mode");
        return 0;
    }
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&m_impl->engine, out, frameCount, &read);
    return read;
}

std::vector<f32> AudioEngine::renderSeconds(f32 seconds, u32 blockFrames) {
    std::vector<f32> out;
    if (!m_impl || blockFrames == 0) {
        return out;
    }
    const u32 ch = channels();
    const u64 total = secondsToFrames(seconds, sampleRate());
    out.assign(total * ch, 0.f);
    u64 done = 0;
    while (done < total) {
        const u64 n = std::min<u64>(blockFrames, total - done);
        update(static_cast<f32>(n) / static_cast<f32>(sampleRate()));
        render(out.data() + done * ch, n);
        done += n;
    }
    return out;
}

f64 AudioEngine::timeSeconds() const {
    return m_impl ? static_cast<f64>(ma_engine_get_time_in_pcm_frames(&m_impl->engine)) / m_impl->config.sampleRate : 0.0;
}

void AudioEngine::update(f32 dt) {
    if (!m_impl) {
        return;
    }
    EngineImpl& e = *m_impl;

    if (e.transition) {
        SnapshotTransition& t = *e.transition;
        t.elapsed += dt;
        const f32 a = t.duration > 0.f ? std::clamp(t.elapsed / t.duration, 0.f, 1.f) : 1.f;
        for (auto& entry : t.entries) {
            entry.bus->setVolume(entry.from + (entry.to - entry.from) * a);
        }
        if (a >= 1.f) {
            for (auto& entry : t.entries) {
                if (entry.mute) {
                    entry.bus->setMuted(*entry.mute);
                }
            }
            e.transition.reset();
        }
    }

    for (auto& vp : e.voices) {
        Voice& v = *vp;
        if (!v.active) {
            continue;
        }
        const bool playing = ma_sound_is_playing(&v.sound) == MA_TRUE;
        if (v.state == VoiceState::Stopping && !playing) {
            e.freeVoice(v);
            continue;
        }
        if (!ma_sound_is_looping(&v.sound) && ma_sound_at_end(&v.sound)) {
            e.freeVoice(v);
            continue;
        }
        e.updateVoice(v, dt);
    }
}

// ---- sound data ---------------------------------------------------------------------------------

SoundId AudioEngine::loadSound(const std::string& path, LoadMode mode) {
    if (!m_impl) {
        return {};
    }
    EngineImpl& e = *m_impl;
    // miniaudio's resource manager reads a freed node on its own failure path (heap-use-after-free found with ASan
    // for missing files): reject files that cannot be opened before handing the path to it.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(path), ec)) {
        OX_LOG_ERROR("audio", "failed to load sound '{}': no such file", path);
        return {};
    }
    auto data = std::make_unique<SoundData>();
    data->kind = SoundData::Kind::File;
    data->path = path;
    data->mode = mode;
    auto proto = std::make_unique<ma_sound>();
    ma_sound_config cfg = ma_sound_config_init_2(&e.engine);
    cfg.pFilePath = path.c_str();
    cfg.flags = MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT |
                (mode == LoadMode::Decode ? MA_SOUND_FLAG_DECODE : MA_SOUND_FLAG_STREAM);
    if (ma_sound_init_ex(&e.engine, &cfg, proto.get()) != MA_SUCCESS) {
        OX_LOG_ERROR("audio", "failed to load sound '{}'", path);
        return {};
    }
    f32 len = 0.f;
    ma_sound_get_length_in_seconds(proto.get(), &len);
    data->duration = len;
    if (mode == LoadMode::Decode) {
        data->prototype = std::move(proto);
    } else {
        ma_sound_uninit(proto.get());
    }
    const SoundId id{e.nextSoundId++};
    e.sounds.emplace(id.value, std::move(data));
    return id;
}

SoundId AudioEngine::loadSound(const Vfs& vfs, std::string_view uri, LoadMode mode) {
    if (!m_impl) {
        return {};
    }
    if (auto native = vfs.resolveNative(uri)) {
        return loadSound(native->string(), mode);
    }
    auto bytes = vfs.readBytes(uri);
    if (!bytes) {
        OX_LOG_ERROR("audio", "failed to read sound '{}': {}", uri, bytes.error().message);
        return {};
    }
    return loadSoundFromMemory(*bytes, uri);
}

SoundId AudioEngine::createFromPcm(std::span<const f32> interleaved, u32 channels, u32 sampleRate) {
    if (!m_impl || channels == 0 || sampleRate == 0) {
        return {};
    }
    auto data = std::make_unique<SoundData>();
    data->kind = SoundData::Kind::Pcm;
    data->pcm.assign(interleaved.begin(), interleaved.end());
    data->channels = channels;
    data->sampleRate = sampleRate;
    data->duration = static_cast<f32>(interleaved.size() / channels) / static_cast<f32>(sampleRate);
    const SoundId id{m_impl->nextSoundId++};
    m_impl->sounds.emplace(id.value, std::move(data));
    return id;
}

SoundId AudioEngine::loadSoundFromMemory(std::span<const std::byte> encoded, std::string_view debugName) {
    if (!m_impl || encoded.empty()) {
        return {};
    }
    // Decoded to the engine rate up front: PCM sounds need no decoder (or its source buffer) at play time.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, m_impl->config.sampleRate);
    ma_decoder decoder;
    if (ma_decoder_init_memory(encoded.data(), encoded.size(), &cfg, &decoder) != MA_SUCCESS) {
        OX_LOG_ERROR("audio", "failed to decode sound '{}' from memory", debugName);
        return {};
    }
    const u32 channels = decoder.outputChannels;
    std::vector<f32> pcm;
    f32 block[4096];
    const ma_uint64 framesPerBlock = channels ? (sizeof(block) / sizeof(f32)) / channels : 0;
    for (;;) {
        ma_uint64 read = 0;
        if (framesPerBlock == 0 || ma_decoder_read_pcm_frames(&decoder, block, framesPerBlock, &read) != MA_SUCCESS ||
            read == 0) {
            break;
        }
        pcm.insert(pcm.end(), block, block + read * channels);
    }
    ma_decoder_uninit(&decoder);
    if (pcm.empty()) {
        OX_LOG_ERROR("audio", "sound '{}' decoded to no samples", debugName);
        return {};
    }
    return createFromPcm(pcm, channels, m_impl->config.sampleRate);
}

SoundId AudioEngine::createSine(f32 frequency, f32 amplitude) {
    if (!m_impl) {
        return {};
    }
    auto data = std::make_unique<SoundData>();
    data->kind = SoundData::Kind::Sine;
    data->frequency = frequency;
    data->amplitude = amplitude;
    const SoundId id{m_impl->nextSoundId++};
    m_impl->sounds.emplace(id.value, std::move(data));
    return id;
}

SoundId AudioEngine::createNoise(NoiseType type, f32 amplitude, i32 seed) {
    if (!m_impl) {
        return {};
    }
    auto data = std::make_unique<SoundData>();
    data->kind = SoundData::Kind::Noise;
    data->noise = type;
    data->amplitude = amplitude;
    data->seed = seed;
    const SoundId id{m_impl->nextSoundId++};
    m_impl->sounds.emplace(id.value, std::move(data));
    return id;
}

void AudioEngine::unloadSound(SoundId id) {
    if (!m_impl) {
        return;
    }
    auto it = m_impl->sounds.find(id.value);
    if (it == m_impl->sounds.end()) {
        return;
    }
    for (auto& v : m_impl->voices) {
        if (v->active && v->soundId == id) {
            m_impl->freeVoice(*v);
        }
    }
    if (it->second->prototype) {
        ma_sound_uninit(it->second->prototype.get());
    }
    m_impl->sounds.erase(it);
}

f32 AudioEngine::soundDuration(SoundId id) const {
    if (!m_impl) {
        return 0.f;
    }
    auto it = m_impl->sounds.find(id.value);
    return it != m_impl->sounds.end() ? it->second->duration : 0.f;
}

// ---- instances ------------------------------------------------------------------------------------

SoundHandle AudioEngine::play(SoundId id, const PlayParams& params) {
    if (!m_impl) {
        return {};
    }
    EngineImpl& e = *m_impl;
    auto it = e.sounds.find(id.value);
    if (it == e.sounds.end()) {
        OX_LOG_WARN("audio", "play: unknown sound id {}", id.value);
        return {};
    }
    const SoundData& data = *it->second;
    AudioBus* bus = e.findBus(params.bus);
    if (bus == nullptr) {
        OX_LOG_WARN("audio", "play: unknown bus '{}', routing to Master", params.bus);
        bus = e.masterBus;
    }

    // Slot allocation with priority-aware stealing.
    Voice* slot = nullptr;
    u32 slotIndex = 0;
    for (u32 i = 0; i < e.voices.size(); ++i) {
        if (!e.voices[i]->active) {
            slot = e.voices[i].get();
            slotIndex = i;
            break;
        }
    }
    if (slot == nullptr) {
        const f32 newAudibility = e.estimateAudibility(params, bus, 0.f, 1.f);
        Voice* victim = nullptr;
        for (u32 i = 0; i < e.voices.size(); ++i) {
            Voice* v = e.voices[i].get();
            const auto key = [](const Voice* x) { return std::tuple(x->params.priority, x->lastAudibility, x->sequence); };
            if (victim == nullptr || key(v) < key(victim)) {
                victim = v;
                slotIndex = i;
            }
        }
        const bool canSteal = victim != nullptr &&
                              (victim->params.priority < params.priority ||
                               (victim->params.priority == params.priority && victim->lastAudibility <= newAudibility));
        if (!canSteal) {
            OX_LOG_DEBUG("audio", "voice limit reached, sound {} rejected", id.value);
            return {};
        }
        e.freeVoice(*victim);
        slot = victim;
    }

    Voice& v = *slot;
    v.params = params;
    v.soundId = id;
    v.bus = bus;
    v.occlusion = 0.f;
    v.state = VoiceState::Playing;
    v.sequence = ++e.voiceSequence;

    if (!initVoiceFilterNode(e, v)) {
        OX_LOG_ERROR("audio", "failed to create voice filter");
        return {};
    }

    ma_sound_config cfg = ma_sound_config_init_2(&e.engine);
    cfg.flags = MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
    ma_result r = MA_SUCCESS;
    switch (data.kind) {
    case SoundData::Kind::File:
        cfg.pFilePath = data.path.c_str();
        cfg.flags |= data.mode == LoadMode::Decode ? MA_SOUND_FLAG_DECODE : MA_SOUND_FLAG_STREAM;
        break;
    case SoundData::Kind::Pcm:
        r = ma_audio_buffer_ref_init(ma_format_f32, data.channels, data.pcm.data(), data.pcm.size() / data.channels,
                                     &v.bufferRef);
        v.bufferRef.sampleRate = data.sampleRate;
        v.hasBufferRef = r == MA_SUCCESS;
        cfg.pDataSource = &v.bufferRef;
        break;
    case SoundData::Kind::Sine: {
        const ma_waveform_config wc = ma_waveform_config_init(ma_format_f32, 1, e.config.sampleRate, ma_waveform_type_sine,
                                                              data.amplitude, data.frequency);
        r = ma_waveform_init(&wc, &v.waveform);
        v.hasWaveform = r == MA_SUCCESS;
        cfg.pDataSource = &v.waveform;
        break;
    }
    case SoundData::Kind::Noise: {
        const ma_noise_type nt = data.noise == NoiseType::White  ? ma_noise_type_white
                                 : data.noise == NoiseType::Pink ? ma_noise_type_pink
                                                                 : ma_noise_type_brownian;
        const ma_noise_config nc = ma_noise_config_init(ma_format_f32, 1, nt, data.seed, data.amplitude);
        r = ma_noise_init(&nc, nullptr, &v.noise);
        v.hasNoise = r == MA_SUCCESS;
        cfg.pDataSource = &v.noise;
        break;
    }
    }
    if (r == MA_SUCCESS) {
        r = ma_sound_init_ex(&e.engine, &cfg, &v.sound);
    }
    if (r != MA_SUCCESS) {
        OX_LOG_ERROR("audio", "failed to create sound instance (ma_result {})", static_cast<int>(r));
        v.active = true; // so freeVoice releases partial state
        e.freeVoice(v);
        return {};
    }
    v.soundInit = true;
    v.active = true;

    ma_node_attach_output_bus(&v.sound, 0, &v.filterNode.base, 0);
    ma_node_attach_output_bus(&v.filterNode.base, 0, &e.internals(*bus).group, 0);

    ma_sound_set_looping(&v.sound, params.loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_pitch(&v.sound, params.pitch);
    ma_sound_set_volume(&v.sound, params.volume);
    e.applySpatial(v);

    const u64 now = ma_engine_get_time_in_pcm_frames(&e.engine);
    const u64 startAt = now + secondsToFrames(params.startDelaySeconds, e.config.sampleRate);
    if (params.startDelaySeconds > 0.f) {
        ma_sound_set_start_time_in_pcm_frames(&v.sound, startAt);
    }
    if (params.fadeInSeconds > 0.f) {
        ma_sound_set_fade_start_in_pcm_frames(&v.sound, 0.f, 1.f, secondsToFrames(params.fadeInSeconds, e.config.sampleRate),
                                              startAt);
    }
    if (params.startPaused) {
        v.state = VoiceState::Paused;
    } else {
        ma_sound_start(&v.sound);
    }
    v.lastAudibility = e.estimateAudibility(params, bus, 0.f, params.fadeInSeconds > 0.f ? 0.f : 1.f);
    return SoundHandle{slotIndex, v.generation};
}

void AudioEngine::stop(SoundHandle h, f32 fadeOutSeconds) {
    Voice* v = m_impl ? m_impl->resolve(h) : nullptr;
    if (v == nullptr) {
        return;
    }
    if (fadeOutSeconds <= 0.f || v->state == VoiceState::Paused) {
        m_impl->freeVoice(*v);
        return;
    }
    ma_sound_stop_with_fade_in_pcm_frames(&v->sound, secondsToFrames(fadeOutSeconds, m_impl->config.sampleRate));
    v->state = VoiceState::Stopping;
}

void AudioEngine::stopAll(f32 fadeOutSeconds) {
    if (!m_impl) {
        return;
    }
    for (u32 i = 0; i < m_impl->voices.size(); ++i) {
        Voice& v = *m_impl->voices[i];
        if (v.active) {
            stop(SoundHandle{i, v.generation}, fadeOutSeconds);
        }
    }
}

void AudioEngine::pause(SoundHandle h) {
    Voice* v = m_impl ? m_impl->resolve(h) : nullptr;
    if (v != nullptr && v->state == VoiceState::Playing) {
        ma_sound_stop(&v->sound);
        v->state = VoiceState::Paused;
    }
}

void AudioEngine::resume(SoundHandle h) {
    Voice* v = m_impl ? m_impl->resolve(h) : nullptr;
    if (v != nullptr && v->state == VoiceState::Paused) {
        ma_sound_start(&v->sound);
        v->state = VoiceState::Playing;
    }
}

bool AudioEngine::isValid(SoundHandle h) const { return m_impl && m_impl->resolve(h) != nullptr; }

bool AudioEngine::isPlaying(SoundHandle h) const {
    const Voice* v = m_impl ? m_impl->resolve(h) : nullptr;
    if (v == nullptr || v->state == VoiceState::Paused) {
        return false;
    }
    if (v->state == VoiceState::Stopping) {
        return ma_sound_is_playing(&v->sound) == MA_TRUE;
    }
    return !ma_sound_at_end(&v->sound) || ma_sound_is_looping(&v->sound);
}

void AudioEngine::setVolume(SoundHandle h, f32 volume) {
    if (Voice* v = m_impl ? m_impl->resolve(h) : nullptr) {
        v->params.volume = volume;
        ma_sound_set_volume(&v->sound, volume);
        m_impl->updateVoice(*v, 0.f);
    }
}

void AudioEngine::fadeTo(SoundHandle h, f32 volume, f32 seconds) {
    if (Voice* v = m_impl ? m_impl->resolve(h) : nullptr) {
        // The fader multiplies the instance volume; express the target relative to it.
        const f32 base = std::max(v->params.volume, 1e-4f);
        ma_sound_set_fade_in_pcm_frames(&v->sound, -1.f, volume / base, secondsToFrames(seconds, m_impl->config.sampleRate));
    }
}

void AudioEngine::setPitch(SoundHandle h, f32 pitch) {
    if (Voice* v = m_impl ? m_impl->resolve(h) : nullptr) {
        v->params.pitch = pitch;
        ma_sound_set_pitch(&v->sound, pitch);
    }
}

void AudioEngine::setPan(SoundHandle h, f32 pan) {
    if (Voice* v = m_impl ? m_impl->resolve(h) : nullptr) {
        v->params.pan = pan;
        if (!v->params.spatial.enabled) {
            ma_sound_set_pan(&v->sound, pan);
        }
    }
}

void AudioEngine::setLooping(SoundHandle h, bool loop) {
    if (Voice* v = m_impl ? m_impl->resolve(h) : nullptr) {
        v->params.loop = loop;
        ma_sound_set_looping(&v->sound, loop ? MA_TRUE : MA_FALSE);
    }
}

void AudioEngine::setSpatial(SoundHandle h, const Spatial3D& spatial) {
    if (Voice* v = m_impl ? m_impl->resolve(h) : nullptr) {
        v->params.spatial = spatial;
        m_impl->applySpatial(*v);
    }
}

void AudioEngine::setPosition(SoundHandle h, const glm::vec3& position, const glm::vec3& velocity) {
    if (Voice* v = m_impl ? m_impl->resolve(h) : nullptr) {
        v->params.spatial.position = position;
        v->params.spatial.velocity = velocity;
        ma_sound_set_position(&v->sound, position.x, position.y, position.z);
        ma_sound_set_velocity(&v->sound, velocity.x, velocity.y, velocity.z);
    }
}

void AudioEngine::setDirection(SoundHandle h, const glm::vec3& direction) {
    if (Voice* v = m_impl ? m_impl->resolve(h) : nullptr) {
        v->params.spatial.direction = direction;
        ma_sound_set_direction(&v->sound, direction.x, direction.y, direction.z);
    }
}

f32 AudioEngine::occlusion(SoundHandle h) const {
    const Voice* v = m_impl ? m_impl->resolve(h) : nullptr;
    return v != nullptr ? v->occlusion : 0.f;
}

f32 AudioEngine::audibility(SoundHandle h) const {
    const Voice* v = m_impl ? m_impl->resolve(h) : nullptr;
    return v != nullptr ? v->lastAudibility : 0.f;
}

u32 AudioEngine::activeVoiceCount() const {
    if (!m_impl) {
        return 0;
    }
    return static_cast<u32>(std::count_if(m_impl->voices.begin(), m_impl->voices.end(), [](const auto& v) { return v->active; }));
}

u32 AudioEngine::maxVoices() const { return m_impl ? m_impl->config.maxVoices : 0; }

// ---- listener / occlusion ------------------------------------------------------------------------

void AudioEngine::setListener(const ListenerState& l) {
    if (!m_impl) {
        return;
    }
    m_impl->listener = l;
    ma_engine* eng = &m_impl->engine;
    const glm::vec3 fwd = l.orientation * glm::vec3(0.f, 0.f, -1.f);
    const glm::vec3 up = l.orientation * glm::vec3(0.f, 1.f, 0.f);
    ma_engine_listener_set_position(eng, 0, l.position.x, l.position.y, l.position.z);
    ma_engine_listener_set_direction(eng, 0, fwd.x, fwd.y, fwd.z);
    ma_engine_listener_set_world_up(eng, 0, up.x, up.y, up.z);
    ma_engine_listener_set_velocity(eng, 0, l.velocity.x, l.velocity.y, l.velocity.z);
}

const ListenerState& AudioEngine::listener() const {
    static const ListenerState kDefault{};
    return m_impl ? m_impl->listener : kDefault;
}

void AudioEngine::setOcclusionProvider(IAudioOcclusionProvider* provider) {
    if (m_impl) {
        m_impl->occlusionProvider = provider;
    }
}

// ---- mixer ---------------------------------------------------------------------------------------

AudioBus* AudioEngine::master() { return m_impl ? m_impl->masterBus : nullptr; }
AudioBus* AudioEngine::bus(std::string_view name) { return m_impl ? m_impl->findBus(name) : nullptr; }

AudioBus* AudioEngine::createBus(const std::string& name, AudioBus* parent) {
    if (!m_impl) {
        return nullptr;
    }
    return m_impl->createBus(name, parent != nullptr ? parent : m_impl->masterBus);
}

void AudioEngine::addDucking(const DuckingSettings& settings) {
    if (m_impl) {
        m_impl->ducking.push_back(settings);
        m_impl->rebuildDucking();
    }
}

void AudioEngine::clearDucking() {
    if (m_impl) {
        m_impl->ducking.clear();
        m_impl->rebuildDucking();
    }
}

void AudioEngine::defineSnapshot(const MixerSnapshot& snapshot) {
    if (m_impl) {
        m_impl->snapshots[snapshot.name] = snapshot;
    }
}

MixerSnapshot AudioEngine::captureSnapshot(const std::string& name) const {
    MixerSnapshot s;
    s.name = name;
    if (m_impl) {
        for (const auto& b : m_impl->buses) {
            s.busVolumes[b->name()] = b->volume();
            s.busMuted[b->name()] = b->muted();
        }
    }
    return s;
}

bool AudioEngine::applySnapshot(const std::string& name, f32 transitionSeconds) {
    if (!m_impl) {
        return false;
    }
    auto it = m_impl->snapshots.find(name);
    if (it == m_impl->snapshots.end()) {
        OX_LOG_WARN("audio", "unknown mixer snapshot '{}'", name);
        return false;
    }
    SnapshotTransition t;
    t.duration = std::max(transitionSeconds, 0.f);
    for (const auto& [busName, vol] : it->second.busVolumes) {
        if (AudioBus* b = m_impl->findBus(busName)) {
            t.entries.push_back({b, b->volume(), vol, std::nullopt});
        }
    }
    for (const auto& [busName, muted] : it->second.busMuted) {
        AudioBus* b = m_impl->findBus(busName);
        if (b == nullptr) {
            continue;
        }
        auto e = std::find_if(t.entries.begin(), t.entries.end(), [&](const auto& x) { return x.bus == b; });
        if (e != t.entries.end()) {
            e->mute = muted;
        } else {
            t.entries.push_back({b, b->volume(), b->volume(), muted});
        }
    }
    m_impl->transition = std::move(t);
    if (transitionSeconds <= 0.f) {
        update(0.f);
    }
    return true;
}

// ---- debug ---------------------------------------------------------------------------------------

void AudioEngine::debugDraw(const DebugLineFn& line) const {
    if (!m_impl || !line) {
        return;
    }
    auto ring = [&](const glm::vec3& c, f32 r, const glm::vec4& color) {
        constexpr int kSeg = 32;
        for (int i = 0; i < kSeg; ++i) {
            const f32 a0 = 6.2831853f * static_cast<f32>(i) / kSeg, a1 = 6.2831853f * static_cast<f32>(i + 1) / kSeg;
            line(c + glm::vec3(std::cos(a0), 0, std::sin(a0)) * r, c + glm::vec3(std::cos(a1), 0, std::sin(a1)) * r, color);
        }
    };
    const ListenerState& l = m_impl->listener;
    line(l.position, l.position + l.orientation * glm::vec3(0, 0, -1), {0.2f, 0.4f, 1.f, 1.f});
    line(l.position, l.position + l.orientation * glm::vec3(0, 1, 0) * 0.5f, {0.2f, 1.f, 0.2f, 1.f});
    for (const auto& vp : m_impl->voices) {
        const Voice& v = *vp;
        if (!v.active || !v.params.spatial.enabled) {
            continue;
        }
        const Spatial3D& s = v.params.spatial;
        const glm::vec4 c = v.occlusion > 0.5f ? glm::vec4(0.6f, 0.6f, 0.6f, 1) : glm::vec4(0, 1, 1, 1);
        const f32 k = 0.25f;
        line(s.position - glm::vec3(k, 0, 0), s.position + glm::vec3(k, 0, 0), c);
        line(s.position - glm::vec3(0, k, 0), s.position + glm::vec3(0, k, 0), c);
        line(s.position - glm::vec3(0, 0, k), s.position + glm::vec3(0, 0, k), c);
        ring(s.position, s.minDistance, {1, 1, 0, 1});
        ring(s.position, s.maxDistance, {1, 0, 0, 1});
        line(s.position, l.position, {c.r, c.g, c.b, 0.3f});
    }
}

} // namespace ox::audio
