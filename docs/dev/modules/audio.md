# Module `audio` (`Oxwald::audio`)

3D audio, mixer buses, effects and occlusion hooks on top of **miniaudio 0.11** (`ma_engine` node graph).
Depends only on `core` (types/log/assert) + glm. miniaudio + stb_vorbis are compiled in `src/miniaudio_impl.c`;
no miniaudio type leaks into public headers.

## Headers

| Header | Contents |
| --- | --- |
| `oxwald/audio/audio_engine.hpp` | `AudioEngine` service, `AudioEngineConfig` |
| `oxwald/audio/audio_types.hpp` | `SoundId`, `SoundHandle`, `PlayParams`, `Spatial3D`, `ListenerState`, `AttenuationModel`, `evaluateAttenuation`, dB helpers |
| `oxwald/audio/audio_bus.hpp` | `AudioBus`, `DuckingSettings`, `MixerSnapshot` |
| `oxwald/audio/audio_effects.hpp` | `IAudioEffect`, `LowPassEffect`, `HighPassEffect`, `DelayEffect`, `ReverbEffect` (Freeverb), `Biquad` |
| `oxwald/audio/occlusion.hpp` | `IAudioOcclusionProvider`, `RaycastOcclusionProvider` |

## Engine

```cpp
ox::audio::AudioEngine audio;
audio.init({.offline = false, .maxVoices = 64});    // device mode; offline=true → no device, use render()

auto shot  = audio.loadSound("sfx/shot.ogg");                       // LoadMode::Decode (shared PCM)
auto music = audio.loadSound("music/theme.mp3", ox::audio::LoadMode::Stream);
auto beep  = audio.createSine(880.f, 0.3f);                         // generators: sine / noise
auto pcm   = audio.createFromPcm(samples, /*channels*/1, 48000);

ox::audio::PlayParams p;
p.bus = "SFX";
p.volume = 0.8f; p.pitch = 1.1f; p.priority = 5;
p.fadeInSeconds = 0.05f; p.startDelaySeconds = 0.1f;
p.spatial.enabled = true;
p.spatial.position = {3, 0, -2};
p.spatial.attenuation = ox::audio::AttenuationModel::Inverse;   // None/Inverse/Linear/Exponential/Custom
p.spatial.minDistance = 1; p.spatial.maxDistance = 50;
p.spatial.distanceLowPass = true;                                  // air absorption
p.spatial.occlusion = true;                                        // ask the occlusion provider
ox::audio::SoundHandle h = audio.play(shot, p);                    // invalid handle if voice limit hit

audio.setPosition(h, newPos, velocity);         // doppler uses velocities
audio.fadeTo(h, 0.2f, 1.0f);
audio.stop(h, /*fadeOut*/ 0.5f);

audio.setListener({.position = camPos, .orientation = camRot, .velocity = camVel});
audio.update(dt);   // once per frame: frees finished voices, occlusion, custom curves, snapshots
```

* Handles are generational; a voice that ended (non-looping), was stopped or stolen invalidates its handle.
* **Voice limit / stealing**: when all `maxVoices` are busy, the voice with the lowest `(priority, audibility)`
  is stolen if it has lower priority than the new sound, or the same priority and is not louder (audibility =
  volume × fade × distance attenuation × occlusion × bus gain). Otherwise `play` returns an invalid handle.
* **Cones**: `coneInnerAngle/coneOuterAngle/coneOuterGain` + `direction`. **Doppler**: `dopplerFactor`.
* **Custom attenuation**: piecewise linear `customCurve` (distance → gain), applied in `update()`.
* **Offline rendering** (tests/tools): `init({.offline = true})`, then `render(buffer, frames)` or
  `renderSeconds(seconds, block)` (calls `update` between blocks). Fully deterministic.

`loadSoundFromMemory(bytes, name)` decodes an encoded file image (wav/ogg/mp3/flac, e.g. an asset database blob)
to PCM at the engine rate (used by the gameplay asset providers).

## Mixer

Default hierarchy: `Master → Music, SFX, Voice, UI, Ambience`. Each bus = `ma_sound_group` (summing input) →
effect nodes (one miniaudio node per effect, in order) → fader/meter node → parent bus.

```cpp
auto* sfx = audio.bus("SFX");
sfx->setVolumeDb(-6.f);
sfx->setMuted(false);
audio.bus("Music")->setSolo(true);              // non-related buses go silent
auto* steps = audio.createBus("Footsteps", sfx);

auto* reverb = audio.bus("Ambience")->addEffect<ox::audio::ReverbEffect>(
    ox::audio::ReverbEffect::Params{.roomSize = 0.8f, .wet = 0.4f});
auto* lp = audio.bus("Music")->addEffect<ox::audio::LowPassEffect>(800.f);
lp->setCutoff(2000.f);                          // atomics: safe from the game thread
audio.bus("Music")->removeEffect(lp);

audio.addDucking({.sidechainBus = "Voice", .targetBus = "Music", .threshold = 0.01f, .duckVolume = 0.3f});

audio.defineSnapshot({.name = "Pause", .busVolumes = {{"Music", 0.3f}, {"SFX", 0.f}}});
audio.defineSnapshot(audio.captureSnapshot("Gameplay"));
audio.applySnapshot("Pause", 0.5f);             // interpolated in update()

float level = audio.bus("Music")->rms();        // post-fader meters (peak/rms), duckGain()
```

Custom effects implement `IAudioEffect::prepare/process` (interleaved f32, in place, audio thread).

## Occlusion

```cpp
struct PhysicsOcclusion : ox::audio::IAudioOcclusionProvider {
    float occlusion(const glm::vec3& listener, const glm::vec3& source) override { /* raycasts */ }
};
// or adapt a hit counter:
ox::audio::RaycastOcclusionProvider rays([&](auto from, auto to) { return physics.countHits(from, to); }, 0.6f);
audio.setOcclusionProvider(&rays);
```
The factor (0..1, smoothed with `occlusionSmoothing`) lowers the voice gain towards `occlusionVolume` and its
low-pass cutoff towards `occlusionCutoff` (per-voice filter node, combined with distance low-pass).

## Debug draw

`audio.debugDraw([](glm::vec3 a, glm::vec3 b, glm::vec4 c) { debugDraw.line(a, b, c); })` — listener axes, source
crosses, min (yellow) / max (red) distance rings, line to the listener.

## Tests (`ox_audio_tests`, all offline)

Pan by position (L/R energy, listener rotation), attenuation formulas + rendered ratios for all models, bus
volume/mute/solo/hierarchy, sidechain ducking + release, voice stealing & priority, one-shot end + looping,
start delay/pause/resume, fade in/out/fadeTo timing, occlusion (HF energy and gain), distance low-pass, snapshots,
bus effect chain add/remove, standalone DSP (delay/reverb/high-pass), WAV load decoded & streamed, debug draw.

## Known limits / TODO

* One listener (miniaudio supports up to 4; API exposes listener 0).
* Streaming relies on miniaudio's resource-manager job thread; in offline mode very long streams rendered faster
  than real time could underrun (decoded mode is fully deterministic).
* Reverb/delay tails keep processing while a bus is silent (continuous nodes) — cheap but not free.
* Mixer snapshots interpolate volume/mute only (not effect parameters).
* HRTF / multichannel (5.1) panning: not implemented (miniaudio VBAP on the output channel map is used).
* `ma_sound` pitch resampler applies a gentle anti-alias low-pass even at pitch 1 (miniaudio behaviour).
