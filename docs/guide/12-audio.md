# 12. Аудио

> Модуль `audio` (таргет `Oxwald::audio`, пространство имён `ox::audio`). Построен на miniaudio 0.11; зависит только от `core` и glm, типы miniaudio наружу не торчат. Компонент «источник звука» на сцене описан в главе [32. Компоненты ECS](32-gameplay-components.md).

## Зачем

`AudioEngine` — это служба, которая играет звуки: эффекты, музыку, речь, эмбиент. Она умеет:

- загружать звук целиком в память (decode) или потоково (stream) — WAV, MP3, FLAC, OGG Vorbis;
- позиционировать звук в 3D: панорама, затухание с расстоянием (attenuation), конусы направленности, эффект Доплера, «поглощение воздухом»;
- сводить всё через иерархию шин (buses) микшера с громкостью, mute/solo, цепочками эффектов, sidechain-приглушением (ducking) и снапшотами;
- приглушать звук за стенами (окклюзия, occlusion) через подключаемый провайдер, например лучи физики;
- ограничивать число одновременных голосов и «красть» самые тихие (voice stealing).

Главная особенность для разработки: **офлайн-режим** (offline). В нём нет аудиоустройства, а звук «рендерится» в буфер по вызову `render()`. Результат полностью детерминирован, поэтому звук можно проверять в юнит-тестах на CI без звуковой карты. Все примеры этой главы работают именно так.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `AudioEngine` | Служба звука: API для игрового потока, а микширование идёт в потоке устройства (или внутри `render()` в офлайне) |
| `AudioEngineConfig` | Режим (устройство или офлайн), частота, каналы, лимит голосов, параметры окклюзии |
| `SoundId` | Звуковые данные: загруженный файл, PCM из памяти, генератор. Их разделяют все экземпляры |
| `SoundHandle` | Играющий экземпляр (голос, voice). Хэндл с поколением (generational): протухает, когда звук закончился, остановлен или его голос украли |
| `PlayParams` | Параметры запуска: шина, громкость, высота тона (pitch), панорама, цикл, fade-in, задержка, приоритет, `Spatial3D` |
| `Spatial3D` | 3D-параметры: позиция, скорость, направление, модель затухания, конус, Доплер, low-pass по расстоянию, окклюзия |
| `ListenerState` | Слушатель (обычно камера): позиция, ориентация, скорость. Ориентация по умолчанию смотрит в −Z, Y вверх |
| `AudioBus` | Шина микшера. По умолчанию есть `Master → Music, SFX, Voice, UI, Ambience` |
| `IAudioEffect` | DSP-эффект на шине: `LowPassEffect`, `HighPassEffect`, `DelayEffect`, `ReverbEffect` (Freeverb) или свой |
| `MixerSnapshot` | Именованное состояние громкостей шин; переход между снапшотами плавный |
| `IAudioOcclusionProvider` | Отвечает на вопрос «насколько перекрыт путь от слушателя до источника» (0..1) |

## Шаг 1. Движок и первый звук

```cpp
#include <oxwald/audio/audio_engine.hpp>
using namespace ox::audio;

AudioEngine audio;
AudioEngineConfig cfg;
cfg.offline = true;      // без устройства: звук производит только render()/renderSeconds()
cfg.sampleRate = 48000;
cfg.maxVoices = 32;
audio.init(cfg);         // false, если устройство не открылось (в режиме с устройством)

const SoundId beep = audio.createSine(440.f, 0.5f);  // генератор: бесконечный синус
PlayParams p;
p.bus = "UI";
const SoundHandle h = audio.play(beep, p);

// 0.5 с звука. Между блоками по 512 кадров вызывается update(dt).
const std::vector<float> pcm = audio.renderSeconds(0.5f);   // interleaved, 24000 кадров × 2 канала
```

Полный пример: `samples/guide_examples/12-audio/basics.cpp`.

В игре всё то же самое, но с `offline = false` (по умолчанию), без `renderSeconds()`, а `update(dt)` вызывается раз в кадр. Если вы используете `ox::Engine` ([глава 05](05-runtime.md)), он сам создаёт `AudioEngine` в службах, вызывает `update` (фаза `PostUpdate`) и в headless-режиме включает офлайн. Если устройство не открылось, движок тоже откатывается в офлайн. Взять службу можно так: `engine.services().get<ox::audio::AudioEngine>()`.

Поля `AudioEngineConfig`:

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `offline` | `false` | Без устройства; звук — только через `render()` |
| `sampleRate` | `48000` | Частота; `0` — родная частота устройства (только с устройством) |
| `channels` | `2` | Число выходных каналов |
| `periodFrames` | `0` | Размер периода устройства (`0` — по умолчанию бэкенда) |
| `maxVoices` | `64` | Лимит одновременных голосов |
| `createDefaultBuses` | `true` | Создать `Master → Music/SFX/Voice/UI/Ambience` |
| `occlusionVolume` | `0.35` | Громкость при полной окклюзии |
| `occlusionCutoff` | `900` Гц | Срез low-pass при полной окклюзии |
| `occlusionSmoothing` | `8` 1/с | Скорость сглаживания значения окклюзии |

## Шаг 2. Звуковые данные и экземпляры

```cpp
SoundId shot  = audio.loadSound("sfx/shot.ogg");                     // LoadMode::Decode — целиком в память
SoundId music = audio.loadSound("music/theme.mp3", LoadMode::Stream); // потоково — для музыки и длинного эмбиента
SoundId tone  = audio.createFromPcm(samples, /*channels*/ 1, /*sampleRate*/ 48000);
SoundId hiss  = audio.createNoise(NoiseType::Pink, 0.3f, /*seed*/ 7);
float len     = audio.soundDuration(tone);   // секунды; 0 для бесконечных генераторов
audio.unloadSound(shot);
```

Неудачная загрузка возвращает невалидный `SoundId` (`!id.valid()`) и пишет ошибку в лог. Исключений нет.

Экземпляр живёт, пока звучит:

```cpp
const SoundHandle h = audio.play(shot);
audio.renderSeconds(0.3f);
audio.update(0.f);
// Отыгравший one-shot освобождает голос, и хэндл становится недействительным.
assert(!audio.isValid(h));

PlayParams loop;
loop.loop = true;
const SoundHandle music = audio.play(shot, loop);
audio.stop(music, /*fadeOutSeconds*/ 0.1f);
```

Управление экземпляром: `stop(h, fade)`, `stopAll(fade)`, `pause`/`resume`, `setVolume`, `fadeTo(h, volume, seconds)`, `setPitch`, `setPan` (только для 2D), `setLooping`, `setSpatial`, `setPosition`, `setDirection`. Все они безопасно игнорируют протухший хэндл.

Поля `PlayParams`:

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `bus` | `"SFX"` | Имя шины |
| `volume` | `1` | Линейная громкость |
| `pitch` | `1` | Высота тона (и скорость) |
| `pan` | `0` | −1 лево … +1 право, только для 2D |
| `loop` | `false` | Зациклить |
| `fadeInSeconds` | `0` | Плавное нарастание |
| `startDelaySeconds` | `0` | Задержка старта |
| `priority` | `0` | Приоритет при нехватке голосов (больше — важнее) |
| `startPaused` | `false` | Создать на паузе |
| `spatial` | выключено | `Spatial3D`, см. шаг 3 |

```cpp
PlayParams p;
p.startDelaySeconds = 0.2f; // начнёт звучать через 0.2 с
p.fadeInSeconds = 0.1f;     // и плавно нарастёт за 0.1 с
const SoundHandle h = audio.play(sine, p);
// ...
audio.fadeTo(h, 0.25f, 0.1f);   // увести громкость до 0.25 за 0.1 с
```

### Лимит голосов и приоритеты

Когда все `maxVoices` голосов заняты, `play()` ищет жертву: голос с наименьшими `(priority, audibility)`. Голос крадётся, если его приоритет ниже, чем у нового звука, или приоритет тот же, а он не громче нового. Иначе `play()` вернёт **невалидный хэндл**. Слышимость (audibility) оценивается как volume × fade × затухание с расстоянием × окклюзия × громкость шины. Её можно узнать через `audio.audibility(h)`.

```cpp
AudioEngine audio;
audio.init({.offline = true, .maxVoices = 2});
// ... заняты два голоса: тихий (0.2) и громкий (1.0)
const SoundHandle c = audio.play(sine, loud);    // крадёт тихий голос
whisper.volume = 0.05f;
audio.play(sine, whisper);                       // невалидный: тише всех, приоритет тот же
whisper.priority = 10;
audio.play(sine, whisper);                       // высокий приоритет побеждает
```

Полный пример: `samples/guide_examples/12-audio/basics.cpp` (`GuideAudioBasics.*`).

## Шаг 3. 3D-звук и слушатель

```cpp
audio.setListener({.position = camPos, .orientation = camRot, .velocity = camVel});

PlayParams p;
p.loop = true;
p.spatial.enabled = true;              // 3D-звук
p.spatial.position = {3.f, 0.f, 0.f};  // справа от слушателя
p.spatial.attenuation = AttenuationModel::Inverse;
p.spatial.minDistance = 1.f;
p.spatial.maxDistance = 50.f;
const SoundHandle h = audio.play(engineHum, p);

// Каждый кадр: позиция и скорость источника (скорость нужна для эффекта Доплера).
audio.setPosition(h, {-3.f, 0.f, 0.f}, /*velocity*/ {-1.f, 0.f, 0.f});
```

Полный пример: `samples/guide_examples/12-audio/spatial.cpp`. Тест проверяет энергию левого и правого каналов при разных положениях источника и повороте слушателя.

Модели затухания (`evaluateAttenuation(spatial, distance)` вычисляет ту же формулу, что и микшер):

| `AttenuationModel` | Громкость на расстоянии `d` |
| --- | --- |
| `None` | 1 |
| `Inverse` | `minD / (minD + rolloff·(d − minD))` (OpenAL inverse clamped) |
| `Linear` | `1 − rolloff·(d − minD)/(maxD − minD)`, ноль на `maxDistance` |
| `Exponential` | `(d / minD)^−rolloff` |
| `Custom` | кусочно-линейная `customCurve` из точек `{distance, gain}`, отсортированных по расстоянию |

Ближе `minDistance` звук не ослабляется. Кривая `Custom` применяется в `update()`.

```cpp
Spatial3D s;
s.minDistance = 1.f;
s.maxDistance = 21.f;
s.attenuation = AttenuationModel::Custom;
s.customCurve = {{0.f, 1.f}, {10.f, 0.5f}, {20.f, 0.f}};
evaluateAttenuation(s, 5.f);   // 0.75
dbToLinear(-6.f);              // ≈ 0.5; linearToDb — обратно
```

Остальные поля `Spatial3D`:

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `velocity`, `dopplerFactor` | 0, `1` | Эффект Доплера (учитываются скорости источника и слушателя) |
| `direction` | `(0, 0, −1)` | Направление «динамика» для конуса |
| `coneInnerAngle` / `coneOuterAngle` | 2π / 2π | Полные углы конуса в **радианах**; по умолчанию звук всенаправленный |
| `coneOuterGain` | `1` | Громкость вне внешнего конуса |
| `distanceLowPass` | `false` | «Поглощение воздухом»: срез падает от `lowPassNearCutoff` (на `minDistance`) до `lowPassFarCutoff` (на `maxDistance`) |
| `lowPassNearCutoff` / `lowPassFarCutoff` | 20000 / 2000 Гц | Границы среза |
| `occlusion` | `false` | Спрашивать провайдера окклюзии для этого звука |

```cpp
p.spatial.direction = {0.f, 0.f, 1.f};          // «динамик» смотрит на слушателя
p.spatial.coneInnerAngle = glm::radians(60.f);
p.spatial.coneOuterAngle = glm::radians(120.f);
p.spatial.coneOuterGain = 0.2f;                  // сзади — 20 % громкости
p.spatial.distanceLowPass = true;
p.spatial.lowPassFarCutoff = 1500.f;
```

## Шаг 4. Микшер: шины, эффекты, ducking, снапшоты

Каждая шина суммирует свои звуки и дочерние шины. Сигнал проходит через цепочку эффектов (в порядке добавления), затем через фейдер с измерителем и уходит в родительскую шину. `Master` отдаёт его на устройство.

```cpp
AudioBus* footsteps = audio.createBus("Footsteps", audio.bus("SFX"));   // Master → SFX → Footsteps
audio.bus("SFX")->setVolumeDb(-6.f);     // ≈ 0.5
footsteps->setVolume(0.5f);
footsteps->effectiveGain();              // 0.25: громкости перемножаются по иерархии

PlayParams p;
p.bus = "Footsteps";
audio.play(step, p);

audio.bus("SFX")->setMuted(true);        // глушит и дочерние шины
audio.bus("Music")->setSolo(true);       // всё, что не предок и не потомок Music, замолкает
```

Эффекты:

```cpp
AudioBus* ambience = audio.bus("Ambience");
auto* lp     = ambience->addEffect<LowPassEffect>(800.f);
auto* reverb = ambience->addEffect<ReverbEffect>(ReverbEffect::Params{.roomSize = 0.8f, .wet = 0.4f});
lp->setCutoff(2000.f);      // параметры — атомики, менять можно с игрового потока «на лету»
reverb->setBypass(true);    // временно выключить, не удаляя
ambience->removeEffect(lp);
ambience->clearEffects();
```

| Эффект | Параметры |
| --- | --- |
| `LowPassEffect(cutoffHz = 5000, q = 0.707)` | `setCutoff`, `setQ` |
| `HighPassEffect(cutoffHz = 200, q = 0.707)` | `setCutoff`, `setQ` |
| `DelayEffect(delay = 0.25, feedback = 0.4, wet = 0.35, dry = 1)` | `setDelay` (до 2 с), `setFeedback`, `setMix` |
| `ReverbEffect(Params)` | `roomSize`, `damping`, `wet`, `dry`, `width`; `setParams` |

Свой эффект реализует `IAudioEffect`. Метод `process` вызывается **на аудиопотоке** и обрабатывает interleaved `float` на месте:

```cpp
class GainEffect final : public IAudioEffect {
public:
    explicit GainEffect(float gain) : m_gain(gain) {}
    void setGain(float g) { m_gain.store(g, std::memory_order_relaxed); }
    void prepare(uint32_t /*sampleRate*/, uint32_t channels) override { m_channels = channels; }
    void process(float* frames, uint32_t frameCount) override {
        const float g = m_gain.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frameCount * m_channels; ++i) frames[i] *= g;
    }
    const char* typeName() const override { return "Gain"; }
private:
    std::atomic<float> m_gain;
    uint32_t m_channels = 2;
};
auto* gain = ambience->addEffect<GainEffect>(0.5f);
```

Эффекты работают и без движка (`prepare` + `process` на своём буфере). Это удобно для тестов DSP и для офлайн-обработки.

**Ducking.** Пока в шине `Voice` кто-то говорит, музыка приглушается:

```cpp
audio.addDucking({.sidechainBus = "Voice", .targetBus = "Music", .threshold = 0.01f, .duckVolume = 0.25f,
                  .attackSeconds = 0.02f, .releaseSeconds = 0.2f});
audio.bus("Music")->duckGain();   // 1 — не приглушено; меньше — приглушено
```

`threshold` сравнивается с RMS шины-«сайдчейна». Измерители шины: `peak()`, `rms()` (после фейдера, по последнему блоку), `duckGain()`.

**Снапшоты.** Это именованные наборы громкостей шин, например «пауза», «под водой», «катсцена»:

```cpp
audio.defineSnapshot(audio.captureSnapshot("Gameplay"));   // запомнить текущее состояние
MixerSnapshot pause;
pause.name = "Pause";
pause.busVolumes = {{"Music", 0.2f}, {"SFX", 0.f}};
pause.busMuted = {{"Ambience", true}};                      // mute применяется в конце перехода
audio.defineSnapshot(pause);

audio.applySnapshot("Pause", /*transitionSeconds*/ 1.f);    // интерполяция идёт в update()
audio.applySnapshot("Gameplay", 0.f);                       // мгновенно
```

Полный пример: `samples/guide_examples/12-audio/mixer.cpp`.

## Шаг 5. Окклюзия

Движок не знает геометрию уровня. Для каждого звука с `spatial.occlusion = true` он раз в `update()` спрашивает провайдера: «насколько перекрыт отрезок слушатель → источник» (0 — путь чист, 1 — полностью перекрыт). Значение сглаживается во времени. Громкость голоса стремится к `occlusionVolume`, а срез low-pass — к `occlusionCutoff`, поэтому звук за стеной становится тише и глуше.

Готовый адаптер `RaycastOcclusionProvider` переводит число пересечений в окклюзию: `1 − (1 − perHit)^hits`. Вот провайдер поверх лучей физики ([глава 09](09-physics.md)):

```cpp
#include <oxwald/audio/audio_engine.hpp>
#include <oxwald/physics/physics.hpp>

audio::RaycastOcclusionProvider makePhysicsOcclusion(const physics::PhysicsWorld& world) {
    return audio::RaycastOcclusionProvider(
        [&world](const glm::vec3& from, const glm::vec3& to) -> uint32_t {
            const glm::vec3 d = to - from;
            const float len = glm::length(d);
            if (len < 1e-4f) return 0u;
            return static_cast<uint32_t>(world.raycastAll(from, d, len).size());
        },
        /*perHitOcclusion*/ 0.6f);   // каждая стена «съедает» 60 %
}

audio::RaycastOcclusionProvider occlusion = makePhysicsOcclusion(physicsWorld);
audio.setOcclusionProvider(&occlusion);   // не владеет!

audio::PlayParams p;
p.spatial.enabled = true;
p.spatial.position = {0.f, 0.f, -10.f};   // за стеной
p.spatial.occlusion = true;
const audio::SoundHandle radio = audio.play(noise, p);
audio.renderSeconds(1.f);
audio.occlusion(radio);                   // ≈ 0.6 (одна стена)
```

Полный пример: `samples/guide_examples/12-audio/occlusion.cpp` (таргет `ox_guide_audio_occlusion`, нужен `Oxwald::physics`).

Если нужна своя логика (порталы комнат, материалы стен), реализуйте `IAudioOcclusionProvider::occlusion(listener, source)` напрямую. Метод вызывается на игровом потоке из `update()`.

## Шаг 6. Отладка

```cpp
audio.debugDraw([&](glm::vec3 a, glm::vec3 b, glm::vec4 color) { debugDraw.line(a, b, color); });
```

Рисуются оси слушателя, крестики источников, кольца `minDistance` (жёлтые) и `maxDistance` (красные) и линия до слушателя. Для логов пригодятся `activeVoiceCount()`, `audibility(h)`, `occlusion(h)` и `bus(...)->rms()`.

## Типичные ошибки и подводные камни

- **Забыли `update(dt)`.** Без него голоса не освобождаются, не применяются окклюзия, кривые `Custom` и переходы снапшотов. В `ox::Engine` его вызывает встроенная система. Если держите `AudioEngine` сами, вызывайте раз в кадр.
- **Хранение протухших хэндлов.** One-shot, украденный голос или остановленный звук инвалидируют хэндл. Проверяйте `isValid(h)`, если хэндл живёт дольше кадра. Все методы молча игнорируют невалидный хэндл.
- **`play()` вернул невалидный хэндл.** Это не ошибка, а лимит голосов. Важным звукам (реплики, музыка) задавайте `priority` выше, чем шагам и гильзам.
- **Путь в `loadSound`.** Это обычный путь файловой системы, который открывает miniaudio. URI виртуальной ФС (`project://…`) и содержимое архивов `.oxpak` напрямую не читаются: сначала превратите их в реальный путь. Также можно декодировать звук сами и передать PCM в `createFromPcm`.
- **Опечатка в имени шины.** `play()` с несуществующей `bus` не падает: пишет предупреждение в лог и отправляет звук прямо в `Master`, мимо громкости группы.
- **Углы конуса в градусах.** Поля `coneInnerAngle`/`coneOuterAngle` задаются в радианах и как **полные** углы, а не половины.
- **`pan` у 3D-звука.** Панорама действует только при `spatial.enabled = false`. В 3D её задаёт позиция.
- **Источник ровно на краю стены** в провайдере окклюзии на лучах даёт «мигающее» значение. Это не баг аудио: так ведёт себя ваша геометрия.
- **Провайдер окклюзии уничтожен раньше движка.** `setOcclusionProvider` не владеет объектом. Снимите его (`nullptr`) до уничтожения.
- **Громкость стандартных шин «сама» меняется.** `ox::Engine` выставляет громкости `Master/Music/SFX/Voice/UI/Ambience` из пользовательских настроек ([глава 05](05-runtime.md)) при старте и при каждом изменении категории Audio. Игровой микс (снапшоты, свои фейды) лучше строить на своих дочерних шинах (`createBus("Combat", bus("SFX"))`), чтобы не спорить с ползунками в меню.
- **Тяжёлая работа в `IAudioEffect::process`.** Метод работает на аудиопотоке: никаких аллокаций, блокировок и логов. Параметры передавайте через атомики.
- **Стриминг в офлайне.** `LoadMode::Stream` опирается на фоновый поток декодера miniaudio. При очень быстром офлайн-рендере длинного стрима возможны пропуски. Для детерминированных тестов используйте `Decode`.
- **Снапшоты двигают только громкость и mute.** Параметры эффектов (срез фильтра «под водой») меняйте сами.
- **Один слушатель.** Сплит-скрин с двумя слушателями пока не поддерживается. HRTF и 5.1-панорамирование тоже не реализованы.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`audio_engine.hpp`](../../engine/audio/include/oxwald/audio/audio_engine.hpp) | `AudioEngine`, `AudioEngineConfig` |
| [`audio_types.hpp`](../../engine/audio/include/oxwald/audio/audio_types.hpp) | `SoundId`, `SoundHandle`, `LoadMode`, `NoiseType`, `PlayParams`, `Spatial3D`, `ListenerState`, `AttenuationModel`, `evaluateAttenuation`, `dbToLinear`/`linearToDb` |
| [`audio_bus.hpp`](../../engine/audio/include/oxwald/audio/audio_bus.hpp) | `AudioBus`, `DuckingSettings`, `MixerSnapshot` |
| [`audio_effects.hpp`](../../engine/audio/include/oxwald/audio/audio_effects.hpp) | `IAudioEffect`, `Biquad`, `LowPassEffect`, `HighPassEffect`, `DelayEffect`, `ReverbEffect` |
| [`occlusion.hpp`](../../engine/audio/include/oxwald/audio/occlusion.hpp) | `IAudioOcclusionProvider`, `RaycastOcclusionProvider` |

Заметки для разработчиков модуля (граф узлов miniaudio, ограничения): [`docs/dev/modules/audio.md`](../dev/modules/audio.md).

## Что дальше

- [09. Физика](09-physics.md) — лучи для окклюзии, события контактов для звуков ударов.
- [05. Рантайм](05-runtime.md) — где живёт `AudioEngine`, настройки громкости пользователя, headless-режим.
- [16. Открытый мир](16-world.md) — ветер и погода как источник для эмбиента (дождь, порывы).
- [13. ИИ](13-ai.md) — шумы из игры можно параллельно отдавать в «слух» ИИ (`PerceptionSystem::reportNoise`).
- [32. Компоненты ECS](32-gameplay-components.md) — компонент источника звука и слушателя на сцене.
- [Оглавление](README.md).
