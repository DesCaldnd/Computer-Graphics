# 21. Отражения, AO и глобальное освещение

> Модуль `render` (таргет `Oxwald::render`, пространство имён `ox::render`, функции бейка — `ox::render::reflections`). Заголовки: компоненты — `<oxwald/render/components/reflections.hpp>`, фичи, бейк и форматы файлов — `<oxwald/render/features/reflections/reflections.hpp>`. Это растровые техники. Их варианты с трассировкой лучей описаны в [главе 24](24-ray-tracing.md).

## Зачем

IBL от неба ([глава 20](20-lighting-shadows.md)) одинаково отражается во всех точках сцены. Комната с открытым небом в отражении пола, металлический шар в подвале, светящийся голубым, — типичные артефакты сцены, где кроме неба ничего нет. Эта глава про то, как дать каждой точке её собственное окружение:

| Задача | Техника | Компонент / cvar |
| --- | --- | --- |
| Отражения окружения в помещении | Пробы отражений: куб, снятый в точке, с коррекцией параллакса по коробке | `ReflectionProbeComponent` |
| Отражения объектов на экране | SSR: трассировка по Hi-Z буферу глубины, стохастический GGX, накопление во времени | `r.SSR` |
| Зеркала, полированный пол, вода | Планарные отражения: сцена рисуется второй раз из отражённой камеры | `PlanarReflectorComponent` |
| Контактные тени в углах и щелях | GTAO (или SSAO на Low) | `r.AO.Method` |
| Отражённый цветной свет («красная стена подсвечивает пол») | Объёмы проб освещённости, запекаемые по кадрам | `IrradianceVolumeComponent` |

Все три фичи — `Reflections`, `AmbientOcclusion`, `IrradianceVolumes` — встроены в рендерер и включаются компонентами на сцене и cvar'ами.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| Проба отражений | Кубическая карта, снятая из точки сцены и префильтрованная по шероховатости (GGX на мип). Влияет внутри своей коробки |
| Box projection | Коррекция параллакса: луч отражения пересекается со стенками коробки, а не уходит в бесконечность |
| Режим обновления | `Baked` — один раз, `OnEnable` — при включении или изменении, `Realtime` — постоянно, по граням |
| SSR | Screen Space Reflections: отражение ищется в буфере глубины текущего кадра, цвет берётся из прошлого |
| Планарный отражатель | Плоскость, для которой сцена рендерится из зеркальной камеры с косой ближней плоскостью |
| AO | Ambient occlusion: затенение непрямого света в углах, щелях и под объектами |
| Объём освещённости | Сетка проб (SH L1 + моменты глубины), из которой каждый пиксель берёт непрямой диффузный свет |
| `ReflectionsSpecular`, `AO`, `IndirectDiffuse` | Ресурсы графа, которые эти фичи публикуют. `ForwardOpaque` подмешивает их вместо IBL/SH неба |

## Шаг 1. Пробы отражений

Проба ставится в центр помещения. Её коробка (`extents`, полуразмеры в локальных осях сущности) должна совпадать со стенами — тогда box projection «приклеит» отражения к стенам.

```cpp
#include <oxwald/render/features/reflections/reflections.hpp>
#include <oxwald/render/register_types.hpp>
using namespace ox;
using namespace ox::render;

registerRenderTypes(); // компоненты рендера + extract hooks (рантайм и редактор делают это сами)

// Проба отражений в комнате 8 × 4 × 8 м: коробка влияния = стены, box projection включён.
Entity room = world.create("RoomProbe");
room.setPosition({0.0f, 2.0f, 0.0f});
auto& probe = room.add<ReflectionProbeComponent>();
probe.extents = {4.0f, 2.0f, 4.0f};          // полуразмеры коробки (м)
probe.blendDistance = 0.5f;                  // плавный выход за коробку
probe.boxProjection = true;                  // параллакс-коррекция под стены комнаты
probe.captureOffset = {0.0f, -0.5f, 0.0f};   // точка съёмки ниже центра — на высоте глаз
probe.update = ReflectionProbeUpdate::Baked; // снять один раз (или загрузить .oxcube)
probe.priority = 1;                          // перекрывает «уличную» пробу
```

Полный пример: `samples/guide_examples/21-reflections-gi/reflections_cpu.cpp` (`GuideReflections.ComponentsOnTheScene`).

| Поле `ReflectionProbeComponent` | По умолчанию | Смысл |
| --- | --- | --- |
| `enabled` | `true` | Выключенная проба не извлекается в снимок |
| `extents` | (5, 3, 5) м | Полуразмеры коробки влияния и проекции (локальные оси; поворот учитывается, масштаб — нет) |
| `blendDistance` | 1 м | Влияние спадает до нуля на этом расстоянии **снаружи** коробки |
| `boxProjection` | `true` | Коррекция параллакса. Для открытых пространств выключайте |
| `captureOffset` | (0, 0, 0) | Точка съёмки относительно сущности |
| `resolution` | 128 | Размер грани куба. Ограничивается сверху `r.ReflectionProbes.Resolution` |
| `update` | `Baked` | `Baked`, `OnEnable`, `Realtime` |
| `priority` | 0 | При перекрытии выигрывает больший приоритет, затем меньший объём |
| `intensity` | 1 | Множитель отражения |
| `nearPlane` / `farPlane` | 0.05 / 200 м | Отсечение при съёмке (объекты дальше `farPlane` не попадают в куб) |

**Как пробы смешиваются.** Видимые пробы сортируются по приоритету, затем по объёму, и раскладываются по той же кластерной сетке, что и источники света (до 16 проб на кластер, до 256 видимых на вид). Пиксель смешивает до `r.ReflectionProbes.MaxPerPixel` проб спереди назад по их влиянию: 1 внутри коробки, спад за её пределами. Остаток веса заполняется IBL неба.

**Режимы обновления.**

| Режим | Когда снимается | Цена |
| --- | --- | --- |
| `Baked` | Один раз при первом появлении, по запросу бейка или из установленного файла `.oxcube` | Только в момент съёмки |
| `OnEnable` | При активации пробы и при каждом изменении её параметров или трансформа | Только в момент съёмки |
| `Realtime` | Постоянно: `r.ReflectionProbes.RealtimeFacesPerFrame` граней в кадр | Одна отрисовка сцены на грань, каждый кадр |

Полные съёмки (`Baked`, `OnEnable`, бейк) ограничены `r.ReflectionProbes.CapturesPerFrame` проб в кадр. Каждая грань — отдельная отрисовка сцены. Внутри съёмки работают солнце (с тенями из каскадов главного вида), IBL и до `r.Reflections.CaptureMaxLocalLights` локальных источников. AO, SSR и другие пробы при съёмке не используются.

## Шаг 2. Бейк и файлы `.oxcube` / `.oxirr`

Пробы и объёмы освещённости можно запечь и сохранить, чтобы при загрузке уровня не тратить кадры на съёмку:

```cpp
// Кнопка «Bake probes» редактора делает то же самое.
reflections::requestBake(*renderer);
for (int i = 0; i < 16 && reflections::bakeInProgress(*renderer); ++i) render(showroomCamera(), 64, 64, 1);

auto baked = reflections::readBakedProbes(*renderer); // ждёт GPU: вызывать между кадрами
const auto file = std::filesystem::temp_directory_path() / "oxwald_guide_reflections" / "showroom.oxcube";
reflections::saveOxCube(file, baked[0].second);       // baked[i].first — Uuid сущности пробы

// При загрузке сцены: прочитать файл и отдать рендеру — Baked-проба с этим Uuid не будет сниматься заново.
Result<reflections::BakedCubemap> loaded = reflections::loadOxCube(file);
reflections::setBakedProbe(*renderer, id, std::move(*loaded));
```

Полный пример: `samples/guide_examples/21-reflections-gi/reflections_gpu.cpp` (`GuideReflectionsGpu.BakeAndSaveProbe`).

- `requestBake` переснимает **все** пробы (в любом режиме) и все объёмы освещённости в течение следующих кадров с бюджетами `CapturesPerFrame` и `ProbesPerFrame`. Пока `bakeInProgress()` возвращает `true`, продолжайте рендерить кадры.
- Ключ запечённых данных — `Uuid` сущности (`IdComponent`). Он сохраняется в сцене, поэтому файл привязывается к пробе надёжно.
- `.oxcube` хранит префильтрованный куб: RGBA16F, сначала мипы, внутри мипа грани +X, −X, +Y, −Y, +Z, −Z. `.oxirr` хранит объём: SH L1 на пробу и тайлы моментов глубины. Оба формата бинарные, little endian, с магическим числом и версией. Это собственные форматы рендера, а не ассеты `assets`.
- Для объёмов есть пара `readBakedVolumes` / `setBakedVolume` и `saveOxIrradiance` / `loadOxIrradiance`.

В текущей сборке редактор и рантайм эти функции ещё не вызывают: пробы `Baked` снимаются при первом появлении на каждом запуске. Если нужна загрузка готовых данных, вызовите `setBakedProbe`/`setBakedVolume` из своего кода загрузки уровня.

## Шаг 3. SSR

Screen Space Reflections дополняет пробы отражениями того, что есть на экране: объекты на полированном полу, отражения в мокром асфальте. Конвейер:

1. `HiZClosest` — пирамида ближайших глубин.
2. Луч по VNDF-выборке GGX (зеркальный ниже шероховатости 0.05) идёт иерархическим маршем с проверкой толщины.
3. Пространственный resolve переиспользует попадания 1/4/8 соседей.
4. Цвет берётся из пирамиды **прошлого кадра** с репроекцией.
5. Временное накопление с клампом по соседству.

Промахи и поверхности шероховатее `r.SSR.MaxRoughness` берут отражение из проб или неба.

```cpp
ScopedCVar ssr("r.SSR", "true");          // по умолчанию выключен: включают пресеты Medium+
ScopedCVar half("r.SSR.HalfRes", "false");
const Image withSsr = render(showroomCamera(), 128, 128, 8); // SSR накапливается во времени
ranPass("Reflections.SSRTrace");          // true
```

Полный пример: `samples/guide_examples/21-reflections-gi/reflections_gpu.cpp` (`GuideReflectionsGpu.ProbeAndSsr`). Тест сравнивает кадр «проба + SSR» с кадром «только проба».

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.SSR` | `false`¹ | Включить SSR |
| `r.SSR.Quality` | 2 | 0–3: число соседей в resolve (1/4/4/8) и смешивание по времени |
| `r.SSR.MaxSteps` | 64 | Итерации Hi-Z марша |
| `r.SSR.HalfRes` | `false` | Трассировка в половинном разрешении |
| `r.SSR.MaxRoughness` | 0.7 | Шероховатее — только пробы/небо |
| `r.SSR.Thickness` | 0.3 м | Предполагаемая толщина объектов в буфере глубины (растёт с расстоянием) |
| `r.SSR.Temporal` | `true` | Временное накопление |

¹ Объявлено выключенным, чтобы инструменты и тесты без выбора уровня качества рендерили как раньше. Любой пресет (рантайм и редактор всегда применяют пресет) включает SSR с уровня Medium.

## Шаг 4. Планарные отражения

Для настоящих зеркал и спокойной воды SSR не хватает: в нём нет того, что за кадром. Планарный отражатель рисует сцену из зеркальной камеры. У неё конечная дальняя плоскость `maxDistance` и косая ближняя плоскость Ленгьеля, которая отсекает всё под зеркалом. Отрисовка ограничена scissor'ом по экранному следу отражателя. Плоскость — локальная XZ сущности, нормаль — локальная +Y.

```cpp
Entity mirror = mesh(Primitive::Plane, material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.02f), {0, 0.01f, 2.0f},
                     glm::vec3(3.0f));
mirror.add<PlanarReflectorComponent>().size = {0.5f, 0.5f}; // в локальных единицах плоскости (масштаб 3 → 1.5 м)
```

Полный пример: `samples/guide_examples/21-reflections-gi/reflections_gpu.cpp` (`GuideReflectionsGpu.PlanarMirror`).

| Поле `PlanarReflectorComponent` | По умолчанию | Смысл |
| --- | --- | --- |
| `size` | (0, 0) | Полуразмеры в локальных XZ для отсечения и композита; 0 — бесконечная плоскость |
| `resolutionScale` | 1 | × `r.PlanarReflections.ResolutionScale` × разрешение рендера |
| `clipOffset` | 0.02 | Сдвиг плоскости отсечения по нормали (прячет швы и z-fighting у поверхности) |
| `maxDistance` | 500 м | Дальняя плоскость отражённого вида |
| `maxRoughness` | 0.3 | Поверхности шероховатее берут отражение из проб/SSR |
| `distortion` | 0.02 | Сдвиг экранных UV на единицу отклонения нормали (неровные зеркала, вода) |
| `intensity`, `priority` | 1, 0 | Яркость; кто важнее при лимите отражателей |

Отражение накладывается на непрозрачные пиксели, лежащие в плоскости отражателя. Первый отражатель публикуется ресурсом `PlanarReflection` — его читает шейдер воды ([глава 23](23-transparency-water-particles.md)). Всего отражателей в кадре — до `r.PlanarReflections.MaxReflectors` (не больше 4). Каждый стоит одной дополнительной отрисовки сцены.

## Шаг 5. Ambient occlusion: GTAO

AO затемняет непрямой свет в углах, щелях и под объектами. Освещение умножает на AO непрямой диффуз и выводит из него затенение бликов (specular occlusion). Прямой свет AO не трогает.

Конвейер: GTAO (на Low — полусферический SSAO) в полном или половинном разрешении → билатеральный фильтр 3×3 → временное накопление с репроекцией по глубине → апсемпл с учётом глубины в ресурс `AO`.

```cpp
ScopedCVar method("r.AO.Method", "2"); // 0 выкл, 1 SSAO, 2 GTAO
ScopedCVar quality("r.AO.Quality", "2");
const Image withAo = render(cam, 128, 128, 8);
ranPass("AO.Trace");                   // true
```

Полный пример: `samples/guide_examples/21-reflections-gi/reflections_gpu.cpp` (`GuideReflectionsGpu.GtaoDarkensCorners`). Тест проверяет, что AO только затемняет кадр. `r.DebugView AO` показывает сам буфер AO.

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.AO.Method` | 0¹ | 0 — выкл., 1 — SSAO, 2 — GTAO |
| `r.AO.Quality` | 2 | GTAO: срезов × шагов 1×4, 2×6, 2×8, 3×12 (SSAO: 8–24 выборки) |
| `r.AO.HalfRes` | `false` | Половинное разрешение + билатеральный апсемпл |
| `r.AO.Radius` | 0.75 м | Радиус поиска в мире |
| `r.AO.Intensity` | 1 | Сила (показатель степени видимости), 0–4 |
| `r.AO.Temporal` | `true` | Временное накопление |

¹ Как и SSR, объявлено выключенным. Любой пресет включает AO, начиная с Low (SSAO).

## Шаг 6. Объёмы освещённости (запечённый GI)

Пробы отражений решают блики, а объём освещённости — непрямой **диффузный** свет. Это свет, отражённый от стен и пола: красная стена подсвечивает белый пол красным, в тени под навесом не чёрно, а светло от освещённой земли.

Объём — сетка проб в коробке. Каждая проба снимается шестью гранями куба размера `captureResolution`, проецируется в SH L1, а также сохраняет моменты глубины в октаэдрической раскладке. Пиксель берёт трилинейную интерполяцию соседних проб. Пробы, видимые сзади или закрытые стеной, отбрасываются по Чебышеву (как в DDGI), поэтому свет не протекает сквозь стены. Вне коробки объём плавно переходит в SH неба.

```cpp
Entity v = world->create("IrradianceVolume");
v.setPosition({0.0f, 1.5f, 0.0f});
auto& vol = v.add<IrradianceVolumeComponent>();
vol.extents = {3.0f, 1.5f, 3.0f};
vol.probeCount = {6, 3, 6};    // 108 проб
vol.captureResolution = 16;    // грань куба при съёмке пробы
ScopedCVar perFrame("r.GI.IrradianceVolumes.ProbesPerFrame", "64"); // бюджет бейка в кадр
```

Полный пример: `samples/guide_examples/21-reflections-gi/reflections_gpu.cpp` (`GuideReflectionsGpu.IrradianceVolumeBakesOverFrames`). Тест ставит красную стену под солнцем и проверяет, что после бейка кадр меняется.

| Поле `IrradianceVolumeComponent` | По умолчанию | Смысл |
| --- | --- | --- |
| `extents` | (10, 4, 10) м | Полуразмеры; пробы занимают всю коробку |
| `probeCount` | (8, 4, 8) | Пробы по осям: не меньше 2, не больше 64 по оси и 16 384 всего |
| `blendDistance` | 1 м | Переход к SH неба за пределами коробки |
| `intensity` | 1 | Множитель |
| `normalBias` / `viewBias` | 0.25 / 0.15 | Сдвиг точки выборки по нормали и к камере (× шаг проб). Против протечек и «пятен» |
| `captureResolution` | 32 | Размер грани при съёмке пробы |
| `priority` | 0 | Приоритет при перекрытии (до 8 объёмов на вид) |

**Стоимость бейка.** Одна проба — шесть отрисовок сцены. Объём 8×4×8 (256 проб) при `ProbesPerFrame` 32 запекается за 8 кадров. Пока бейк идёт, пробы без данных не участвуют, и освещение «проявляется» постепенно. Освещение при съёмке однократное: в пробах нет ни отражённого света других проб, ни AO и SSR. Для динамического GI есть DDGI на трассировке лучей ([глава 24](24-ray-tracing.md)), он использует тот же формат ресурса `IndirectDiffuse`.

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.GI.IrradianceVolumes` | `true` | Объёмы освещённости |
| `r.GI.IrradianceVolumes.ProbesPerFrame` | 32 | Проб, запекаемых за кадр (1–4096) |

## Шаг 7. Трассировка лучей вместо растра

При `r.RayTracing 1` на GPU с ray queries RT-варианты встают в те же эксклюзивные группы с более высоким приоритетом:

| Растровая фича (группа) | RT-замена | Что меняется |
| --- | --- | --- |
| `Reflections` (`Reflections`) | `ReflectionsRT` | Отражения внеэкранных объектов. Выше `r.RayTracing.Reflections.MaxRoughness` остаются пробы/IBL |
| `AmbientOcclusion` (`AO`) | `AmbientOcclusionRT` | AO без экранных ореолов |
| `IrradianceVolumes` (`IndirectDiffuse`) | `GlobalIlluminationRT` | DDGI: динамический объём проб вокруг камеры вместо запечённого |

На macOS (MoltenVK) трассировки лучей нет: флаг ни на что не влияет, работают растровые техники этой главы. Подробности — в [главе 24](24-ray-tracing.md).

## Уровни качества

Группа `Reflections`:

| CVar | По умолчанию | Low | Medium | High | Ultra |
| --- | --- | --- | --- | --- | --- |
| `r.SSR` | `false` | выкл. | вкл. | вкл. | вкл. |
| `r.SSR.Quality` | 2 | 0 | 1 | 2 | 3 |
| `r.SSR.MaxSteps` | 64 | 24 | 40 | 64 | 96 |
| `r.SSR.HalfRes` | `false` | вкл. | вкл. | выкл. | выкл. |
| `r.SSR.MaxRoughness` | 0.7 | 0.35 | 0.5 | 0.7 | 0.85 |
| `r.ReflectionProbes.Resolution` | 128 | 64 | 128 | 128 | 256 |
| `r.ReflectionProbes.Realtime` | `true` | выкл. | вкл. | вкл. | вкл. |
| `r.ReflectionProbes.RealtimeFacesPerFrame` | 1 | 1 | 1 | 1 | 2 |
| `r.ReflectionProbes.MaxPerPixel` | 4 | 1 | 2 | 4 | 4 |
| `r.PlanarReflections` | `true` | выкл. | вкл. | вкл. | вкл. |
| `r.PlanarReflections.ResolutionScale` | 0.75 | 0.25 | 0.5 | 0.75 | 1.0 |

Группа `GlobalIllumination`:

| CVar | По умолчанию | Low | Medium | High | Ultra |
| --- | --- | --- | --- | --- | --- |
| `r.AO.Method` | 0 | 1 (SSAO) | 2 (GTAO) | 2 | 2 |
| `r.AO.Quality` | 2 | 0 | 1 | 2 | 3 |
| `r.AO.HalfRes` | `false` | вкл. | вкл. | выкл. | выкл. |
| `r.GI.IrradianceVolumes` | `true` | выкл. | вкл. | вкл. | вкл. |

Без групп: `r.ReflectionProbes` (`true`), `r.ReflectionProbes.CapturesPerFrame` (2), `r.SSR.Thickness` (0.3), `r.SSR.Temporal`, `r.PlanarReflections.MaxReflectors` (2), `r.Reflections.CaptureMaxLocalLights` (64), `r.AO.Radius` (0.75), `r.AO.Intensity` (1), `r.AO.Temporal`, `r.GI.IrradianceVolumes.ProbesPerFrame` (32). Выключить фичу целиком: `r.Feature.Reflections`, `r.Feature.AmbientOcclusion`, `r.Feature.IrradianceVolumes`.

`r.ReflectionProbes.Realtime 0` (Low) не отключает пробы `Realtime`: они снимаются один раз, как `OnEnable`. При автоопределении качества группа `GlobalIllumination` ставится на ступень ниже остальных, если счёт GPU меньше 250 ([глава 26](26-quality-settings.md)).

```cpp
scalability::setGroup(Scalability::Reflections, QualityLevel::Low);
scalability::setGroup(Scalability::GlobalIllumination, QualityLevel::Low);
CVarRegistry::instance().find("r.SSR")->toString();       // "false": на Low только пробы
CVarRegistry::instance().find("r.AO.Method")->toString(); // "1": SSAO
```

Полный пример: `samples/guide_examples/21-reflections-gi/reflections_cpu.cpp` (`GuideReflections.ScalabilityLevels`).

## Стоимость

Замер `ReflectionsTest.PerfReport1080p` (Apple M4 Pro, 1080p, значения уровня High):

| Эффект | GPU, мс | Из чего |
| --- | --- | --- |
| GTAO, полное разрешение | 1.3 | трассировка 0.75, фильтр 0.3, накопление 0.18, апсемпл 0.1 |
| SSR | 2.0 | HiZClosest 0.2, трассировка 0.97, resolve 0.39, накопление 0.23, пирамида цвета 0.22 |
| Планарное зеркало 12×12 м, масштаб 0.75 | 0.6 | вторая отрисовка сцены |
| Пробы: каллинг + композит | 0.2 | |
| `IndirectDiffuse` (объёмы освещённости) | 0.5 | |
| Съёмка пробы | одна отрисовка сцены на грань | только в момент съёмки |

Самые дорогие — SSR и GTAO, на слабых GPU их первыми переводят в половинное разрешение (`HalfRes`). Пробы и объёмы после съёмки почти бесплатны: их цена в кадре — каллинг и выборка.

## Типичные ошибки и подводные камни

- **SSR и AO «не работают» в своём инструменте.** Без выбранного уровня качества `r.SSR` и `r.AO.Method` выключены. Вызовите `scalability::setOverall(...)` или включите их явно.
- **Отражения «плывут» по полу комнаты.** Коробка пробы не совпадает со стенами или выключен `boxProjection`. Подгоните `extents` под помещение.
- **Box projection на улице.** Для открытого пространства коробка даёт неверный параллакс. Выключите `boxProjection` и сделайте коробку большой.
- **Пробы-«соседи» мерцают на границе.** Увеличьте `blendDistance` или разведите приоритеты (`priority`).
- **Много `Realtime`-проб.** Каждая грань — полная отрисовка сцены каждый кадр. Используйте `OnEnable` для того, что меняется редко (открытая дверь, включённый свет).
- **В отражении пробы нет теней вдали.** Съёмки используют каскады главного вида: всё за `r.Shadows.CSM.Distance` от камеры в пробе не затенено.
- **SSR отстаёт на кадр и не видит стекло.** Цвет берётся из прошлого кадра, полупрозрачные объекты в нём отсутствуют.
- **Зеркало «режет» объекты у поверхности или видно шов.** Подстройте `clipOffset`.
- **Шероховатый пол с планарным отражателем.** Выше `maxRoughness` отражатель не применяется — это не ошибка, а переход к пробам/SSR.
- **Свет протекает сквозь стены в объёме освещённости.** Добавьте проб по толщине стены (шаг меньше толщины), подстройте `normalBias`/`viewBias`.
- **Слишком плотная сетка проб.** 64×8×64 = 32 768 проб — больше лимита 16 384: рендер молча уменьшит сетку (по единице на каждой оси, пока не влезет). К тому же бейк займёт сотни кадров. Ставьте несколько объёмов по помещениям.
- **Объём «проявляется» несколько секунд после загрузки.** Идёт бейк по `ProbesPerFrame`. Сохраните результат в `.oxirr` и установите его при загрузке через `setBakedVolume`.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`components/reflections.hpp`](../../engine/render/include/oxwald/render/components/reflections.hpp) | `ReflectionProbeComponent`, `ReflectionProbeUpdate`, `PlanarReflectorComponent`, `IrradianceVolumeComponent`, `registerReflectionTypes` |
| [`features/reflections/reflections.hpp`](../../engine/render/include/oxwald/render/features/reflections/reflections.hpp) | `reflections::requestBake`, `bakeInProgress`, `readBakedProbes/Volumes`, `setBakedProbe/Volume`, `saveOxCube`/`loadOxCube`, `saveOxIrradiance`/`loadOxIrradiance`, `ReflectionSnapshot`, имена ресурсов (`res::kSSR`, `kPlanarReflection`, `kHiZClosest`), `registerReflectionCVars` |
| [`features/reflections/reflection_gpu_types.hpp`](../../engine/render/include/oxwald/render/features/reflections/reflection_gpu_types.hpp) | GPU-раскладки проб, планарных отражений и объёмов; лимиты (`kMaxReflectionProbes` 256, `kMaxPlanarReflections` 4, `kMaxIrradianceVolumes` 8) |
| [`register_types.hpp`](../../engine/render/include/oxwald/render/register_types.hpp) | `registerRenderTypes` — все компоненты рендера |

Шейдерные включения: `render/reflections/probes.glsl`, `planar.glsl` (`oxFindPlanarReflection`, `oxSamplePlanarReflection`), `irradiance_volume.glsl` (`oxSampleIrradianceVolume`). Заметки для разработчиков: [`docs/dev/modules/render_reflections_ao.md`](../dev/modules/render_reflections_ao.md).

## Что дальше

- [20. Освещение и тени](20-lighting-shadows.md) — IBL неба, на которое опираются пробы.
- [22. Объёмные эффекты](22-volumetrics.md) — туман и облака поверх отражений.
- [23. Прозрачность, вода и частицы](23-transparency-water-particles.md) — вода с планарными отражениями.
- [24. Трассировка лучей](24-ray-tracing.md) — RT-отражения, RTAO и DDGI.
- [26. Настройки качества](26-quality-settings.md) — пресеты и меню графики.
- [Оглавление](README.md).
