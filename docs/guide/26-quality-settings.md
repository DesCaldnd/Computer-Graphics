# 26. Качество графики: уровни, автоопределение, настройки игрока

> Модуль `render` (таргет `Oxwald::render`, пространство имён `ox::render`, зонтичный заголовок
> `<oxwald/render/render.hpp>`); уровни и группы — `core` (`<oxwald/core/scalability.hpp>`), настройки игрока —
> `runtime` (`<oxwald/runtime/settings.hpp>`). Общая механика cvar'ов, групп и `settings.json` описана в
> [главе 04](04-cvars-quality.md); здесь — только графика.

## Зачем

Одна и та же игра должна работать на ноутбуке со встроенной графикой и на RTX 4090. В Oxwald, как в Unreal Engine,
это решают **группы масштабируемости** (scalability groups): каждая «дорогая» настройка рендерера — cvar с четырьмя
значениями для Low / Medium / High / Ultra, привязанный к одной из 12 групп. Игрок (или автоопределение) выбирает
уровень группы — рендерер со следующего кадра перестраивает граф кадра, **без перезапуска**.

Глава отвечает на вопросы:

- какие группы есть и что именно меняет каждый уровень (полная таблица ниже — сверена с кодом);
- сколько это стоит в миллисекундах (замеры на Apple M4 Pro, 1080p);
- как работает кнопка «Авто» (GPU-бенчмарк + рекомендация апскейлера);
- как связать меню настроек с `GraphicsSettings` и как добавить в группу свой cvar.

## Ключевые понятия

| Понятие | Где | Коротко |
| --- | --- | --- |
| Группа (`ox::Scalability`) | `core/cvar.hpp` | `ViewDistance, AntiAliasing, Shadows, GlobalIllumination, Reflections, PostProcess, Textures, Effects, Foliage, Shading, Volumetrics, RayTracing` |
| Уровень (`QualityLevel`) | `core/cvar.hpp` | `Low=0, Medium=1, High=2, Ultra=3`, `Custom` — значения правили вручную |
| `sg.<Группа>` | `core/scalability.hpp` | сохраняемый int-cvar с уровнем группы (`sg.Shadows High`) |
| `RenderSettings` | `render/render_settings.hpp` | снимок всех cvar'ов ядра рендерера, берётся **раз в кадр** (`fromCVars()`) |
| `r.Feature.<Имя>` | `render/render_feature.hpp` | тумблер каждой фичи рендера (по умолчанию `true`) |
| `BenchmarkResult` | `render/quality.hpp` | результат GPU-бенчмарка: fill-rate, ALU, bandwidth, `score`, уровни групп |
| `RecommendedSettings` | `render/features/postprocess/postprocess.hpp` | уровни + AA + апскейлер для кнопки «Авто» |
| `GraphicsSettings` | `runtime/settings.hpp` | настройки графики игрока в `user://settings.json` |

Цепочка от меню до пикселей:

```
меню / консоль / settings.json / --quality
        │
        ▼
   sg.<Группа> ──► cvar'ы группы (r.Shadows.*, r.SSR.*, …)   ← ручные правки делают группу Custom
        │
        ▼
   RenderSettings::fromCVars()  (раз в кадр)  +  isEnabled() фич (читают свои cvar'ы)
        │
        ▼
   граф кадра перестраивается на следующем кадре (кэш плана по хэшу топологии)
```

## Шаг 1. Уровни и группы из кода

```cpp
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/render.hpp>
namespace sc = ox::scalability;

sc::setOverall(ox::QualityLevel::Low);
ox::render::RenderSettings s = ox::render::RenderSettings::fromCVars();
// s.csmResolution == 1024, s.csmCascades == 2, s.pcss == false, s.drawDistance == 400

sc::setOverall(ox::QualityLevel::Medium);
sc::setGroup(ox::Scalability::Shadows, ox::QualityLevel::Ultra);   // тени на максимум
sc::setGroup(ox::Scalability::Volumetrics, ox::QualityLevel::Low); // туман и облака подешевле
sc::overallLevel();                                                // Custom: группы на разных уровнях

ox::CVarRegistry::instance().execute("r.Shadows.CSM.Distance 400"); // ручная правка → Shadows = Custom
ox::CVarRegistry::instance().execute("sg.Shadows High");            // снова табличные значения
```

Полный пример: `samples/guide_examples/26-quality-settings/scalability_groups.cpp`.

Ядро рендерера читает cvar'ы только через снимок `RenderSettings` — поэтому кадр всегда согласован, даже если
консоль поменяла значение посреди кадра. Cvar'ы фич (SSR, облака, частицы, ландшафт…) объявлены рядом с фичами и
читаются в `isEnabled()`/`setup()`; в `RenderSettings` их нет — ищите их по имени (`CVarRegistry::find`).

**Список групп для меню.** `sc::cvars(group)` возвращает все переменные группы, `levelString(level)` — значение
уровня строкой. Тест `ListEveryGroupWithItsCVars` печатает именно ту таблицу, что приведена ниже:

```cpp
for (ox::usize g = 0; g < ox::kScalabilityGroupCount; ++g) {
    const ox::Scalability group = ox::Scalability(g);
    for (ox::ICVar* cv : sc::cvars(group)) {
        // cv->name(), cv->defaultString(), cv->levelString(ox::QualityLevel::Low) … Ultra, cv->description()
    }
}
```

> **Без устройства.** `Renderer::create()` регистрирует все cvar'ы сам. Если меню настроек нужно до создания
> рендерера (лаунчер), вызовите `ox::render::registerRenderCVars()` — это cvar'ы ядра; cvar'ы фич регистрируются,
> когда их код попадает в программу (`register*Features()` — см. `guide_quality.hpp` в примере).

## Шаг 2. Все группы и их cvar'ы

Значения по уровням Low / Medium / High / Ultra. «По умолч.» — значение в коде, которое действует, пока уровень не
выбран (голый `Renderer` в тестах и инструментах). Таблица снята с реестра тестом
`QualityGroups.ListEveryGroupWithItsCVars`.

### ViewDistance — дальность и LOD

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.ViewDistance.DrawDistance` | 0 | 400 | 1000 | 2500 | 0 | дальность отрисовки экземпляров, м (0 = дальняя плоскость камеры) |
| `r.ViewDistance.LODBias` | 0 | 1 | 0.5 | 0 | −0.5 | сдвиг LOD мешей (+1 = вдвое большая допустимая ошибка) |
| `r.GpuDriven.LODErrorPixels` | 1 | 2 | 1.5 | 1 | 0.75 | допустимая ошибка упрощения LOD в пикселях ([глава 27](27-gpu-driven-performance.md)) |
| `r.Terrain.LODScale` | 1 | 0.5 | 0.75 | 1 | 1.5 | масштаб дальностей CDLOD ландшафта ([глава 28](28-world-rendering.md)) |

### AntiAliasing — сглаживание

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.AntiAliasing` | 0 | 0 | 1 | 2 | 2 | 0 — нет, 1 — FXAA, 2 — TAA |
| `r.TAA.Quality` | 2 | 0 | 1 | 2 | 3 | клиппинг истории: min/max → variance clip → + Catmull-Rom/дилатация/анти-мерцание |
| `r.AntiAliasing.Samples` | 8 | 4 | 8 | 8 | 16 | период джиттера TAA |
| `r.FXAA.Quality` | 2 | 0 | 1 | 2 | 3 | 4 / 8 / 12 / 16 шагов поиска края |

### Shadows — растровые тени

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.Shadows.CSM.Resolution` | 2048 | 1024 | 2048 | 2048 | 4096 | разрешение каскада солнца |
| `r.Shadows.CSM.Cascades` | 4 | 2 | 3 | 4 | 4 | число каскадов |
| `r.Shadows.CSM.Distance` | 120 | 60 | 100 | 150 | 250 | дальность теней солнца, м |
| `r.Shadows.AtlasSize` | 4096 | 2048 | 4096 | 4096 | 8192 | атлас теней прожекторов |
| `r.Shadows.SpotMaxResolution` | 1024 | 512 | 1024 | 1024 | 2048 | максимальный тайл прожектора в атласе |
| `r.Shadows.PointResolution` | 512 | 256 | 512 | 512 | 1024 | грань куба тени точечного света |
| `r.Shadows.MaxShadowedLights` | 16 | 4 | 8 | 16 | 32 | локальных источников с тенью (прожекторы + точечные) |
| `r.Shadows.MaxPointShadows` | 8 | 2 | 4 | 8 | 12 | из них точечных |
| `r.Shadows.PCFTaps` | 16 | 0 | 8 | 16 | 16 | отсчёты Poisson PCF (0 = одно аппаратное сравнение) |
| `r.Shadows.PCSS` | true | false | false | true | true | мягкие тени с contact hardening |

### GlobalIllumination — AO и непрямой свет

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.AO.Method` | 0 | 1 | 2 | 2 | 2 | 0 — выкл., 1 — SSAO, 2 — GTAO |
| `r.AO.Quality` | 2 | 0 | 1 | 2 | 3 | срезы × шаги GTAO: 1×4, 2×6, 2×8, 3×12 |
| `r.AO.HalfRes` | false | true | true | false | false | AO в половинном разрешении |
| `r.GI.IrradianceVolumes` | true | false | true | true | true | объёмы облучённости (запечённый непрямой свет) |

### Reflections — отражения

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.SSR` | false | false | true | true | true | экранные отражения |
| `r.SSR.Quality` | 2 | 0 | 1 | 2 | 3 | переиспользование лучей 1/4/4/8, временное смешивание |
| `r.SSR.MaxSteps` | 64 | 24 | 40 | 64 | 96 | шаги трассировки по Hi-Z |
| `r.SSR.HalfRes` | false | true | true | false | false | SSR в половинном разрешении |
| `r.SSR.MaxRoughness` | 0.7 | 0.35 | 0.5 | 0.7 | 0.85 | выше этой шероховатости — только пробы |
| `r.ReflectionProbes.Resolution` | 128 | 64 | 128 | 128 | 256 | грань куба пробы |
| `r.ReflectionProbes.Realtime` | true | false | true | true | true | обновлять realtime-пробы |
| `r.ReflectionProbes.RealtimeFacesPerFrame` | 1 | 1 | 1 | 1 | 2 | граней куба за кадр |
| `r.ReflectionProbes.MaxPerPixel` | 4 | 1 | 2 | 4 | 4 | смешиваемых проб на пиксель |
| `r.PlanarReflections` | true | false | true | true | true | планарные отражения (зеркала, вода) |
| `r.PlanarReflections.ResolutionScale` | 0.75 | 0.25 | 0.5 | 0.75 | 1 | разрешение планарного отражения |
| `r.Refraction.Mips` | 6 | 3 | 5 | 6 | 7 | мипы буфера преломления (матовое стекло) |
| `r.Water.SSRSteps` | 12 | 0 | 8 | 12 | 20 | шаги SSR воды (0 = только окружение) |

### PostProcess — постобработка

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.Bloom` | false | true | true | true | true | bloom |
| `r.Bloom.Quality` | 3 | 1 | 2 | 3 | 4 | 4 + q мипов |
| `r.DepthOfField` | false | false | true | true | true | глубина резкости |
| `r.DOF.Quality` | 2 | 0 | 1 | 2 | 3 | 2 + q колец сбора |
| `r.MotionBlur` | false | false | true | true | true | размытие в движении |
| `r.MotionBlur.Quality` | 2 | 0 | 1 | 2 | 3 | 4 + 4q отсчётов |
| `r.Vignette` | true | true | true | true | true | виньетка |
| `r.ChromaticAberration` | true | false | true | true | true | хроматическая аберрация |
| `r.FilmGrain` | true | false | true | true | true | зерно плёнки |

### Textures — текстуры

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.Textures.Anisotropy` | 8 | 2 | 4 | 8 | 16 | анизотропная фильтрация |
| `r.Textures.MipBias` | 0 | 1 | 0.5 | 0 | 0 | сдвиг мипов материалов |
| `r.Textures.MaxSize` | 8192 | 1024 | 2048 | 4096 | 8192 | крупнейший загружаемый размер (большие мипы пропускаются) |
| `r.Streaming.PoolSizeMB` | 1024 | 256 | 512 | 1024 | 2048 | бюджет стриминга мипов, МБ ([глава 27](27-gpu-driven-performance.md)) |

### Effects — частицы и эффекты воды

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.Particles.Budget` | 262144 | 16384 | 65536 | 262144 | 1048576 | частиц на эмиттер |
| `r.Particles.ResolutionDivisor` | 2 | 4 | 2 | 2 | 1 | частицы рисуются в 1/N разрешения |
| `r.Particles.Collision` | true | false | true | true | true | столкновения с буфером глубины |
| `r.Particles.Lighting` | true | false | true | true | true | освещение частиц |
| `r.Particles.SoftParticles` | true | false | true | true | true | мягкие частицы |
| `r.Particles.Sorting` | true | false | false | true | true | сортировка от дальних к ближним |
| `r.Water.Caustics` | true | false | true | true | true | каустика под водой |

### Foliage — растительность

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.Foliage.Density` | 1 | 0.35 | 0.6 | 0.85 | 1 | доля отрисовываемых экземпляров |
| `r.Foliage.DrawDistanceScale` | 1 | 0.5 | 0.75 | 1 | 1.5 | множитель дальностей LOD и отсечения |
| `r.Foliage.ImpostorDistanceScale` | 1 | 0.5 | 0.75 | 1 | 1.5 | множитель дистанции перехода на импосторы |
| `r.Foliage.Grass` | true | false | true | true | true | трава |
| `r.Foliage.Shadows` | true | false | true | true | true | тени деревьев |

### Shading — шейдинг

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.IBL.Resolution` | 128 | 64 | 128 | 128 | 256 | грань префильтрованного куба окружения |
| `r.Shading.MultiScatter` | true | false | true | true | true | компенсация энергии GGX |
| `r.Refraction.BackfaceDepth` | true | false | true | true | true | толщина преломляющих объектов по задним граням |
| `r.Water.GridResolution` | 256 | 96 | 160 | 256 | 384 | сетка поверхности воды |
| `r.Sky.CubeSize` | 128 | 64 | 128 | 128 | 256 | куб неба для IBL |
| `r.Terrain.MaxLayers` | 8 | 2 | 3 | 4 | 8 | слоёв splat на пиксель ландшафта |
| `r.Terrain.Triplanar` | true | false | true | true | true | трипланар на крутых склонах |
| `r.Terrain.Tessellation` | false | false | false | false | true | тесселяция ландшафта вблизи |

### Volumetrics — объёмный туман, облака, атмосфера

| CVar | По умолч. | Low | Medium | High | Ultra | Что делает |
| --- | --- | --- | --- | --- | --- | --- |
| `r.VolumetricFog.GridSizeX/Y/Z` | 160·90·64 | 96·54·32 | 128·72·48 | 160·90·64 | 240·135·128 | сетка фроксел |
| `r.VolumetricFog.Distance` | 128 | 64 | 96 | 128 | 192 | дальность объёмного тумана, м |
| `r.VolumetricFog.HistoryWeight` | 0.9 | 0.85 | 0.9 | 0.9 | 0.95 | временное накопление |
| `r.VolumetricFog.LocalLightShadows` | true | false | true | true | true | тени локальных источников в тумане |
| `r.VolumetricFog.CloudShadows` | true | false | false | true | true | тени облаков в тумане |
| `r.VolumetricClouds` | true | false | true | true | true | объёмные облака |
| `r.VolumetricClouds.Downsample` | 2 | 4 | 4 | 2 | 2 | делитель разрешения облаков |
| `r.VolumetricClouds.Checkerboard` | 16 | 16 | 16 | 16 | 4 | 1 из N пикселей трассируется за кадр |
| `r.VolumetricClouds.Steps` / `.LightSteps` | 64 / 6 | 32 / 4 | 48 / 5 | 64 / 6 | 96 / 8 | шаги марша по облаку и к солнцу |
| `r.VolumetricClouds.MaxDistance` | 40000 | 25000 | 30000 | 40000 | 50000 | дальность облаков, м |
| `r.Sky.AerialPerspective` | true | false | true | true | true | воздушная перспектива неба мира |

### RayTracing — трассировка лучей (действует только при `r.RayTracing 1` и поддержке GPU)

| CVar | По умолч. | Low | Medium | High | Ultra |
| --- | --- | --- | --- | --- | --- |
| `r.RayTracing.Shadows` | true | true | true | true | true |
| `r.RayTracing.Shadows.SamplesPerPixel` / `.MaxLocalLights` | 1 / 4 | 1 / 1 | 1 / 2 | 1 / 4 | 2 / 4 |
| `r.RayTracing.Shadows.ReSTIR` / `.Colored` | false / true | false / false | false / true | false / true | true / true |
| `r.RayTracing.Shadows.ResolutionScale` | 100 | 50 | 100 | 100 | 100 |
| `r.RayTracing.Reflections` / `.AO` / `.Translucency` | true | false | true | true | true |
| `r.RayTracing.Reflections.MaxRoughness` | 0.6 | 0.3 | 0.4 | 0.6 | 0.8 |
| `r.RayTracing.Reflections.SamplesPerPixel` / `.ResolutionScale` | 1 / 100 | 1 / 50 | 1 / 50 | 1 / 100 | 2 / 100 |
| `r.RayTracing.AO.SamplesPerPixel` / `.ResolutionScale` | 1 / 100 | 1 / 50 | 1 / 50 | 2 / 100 | 4 / 100 |
| `r.RayTracing.GI` / `.Volumetrics` | true | false | false | true | true |
| `r.RayTracing.GI.RaysPerProbe` | 128 | 64 | 96 | 128 | 256 |
| `r.RayTracing.GI.ProbesXZ` / `.ProbesY` | 24 / 8 | 12 / 6 | 16 / 8 | 24 / 8 | 32 / 12 |
| `r.RayTracing.GI.ResolutionScale` | 100 | 50 | 50 | 100 | 100 |
| `r.RayTracing.Translucency.MaxBounces` | 4 | 2 | 3 | 4 | 6 |
| `r.RayTracing.Volumetrics.LocalSamples` / `.SkyRays` | 2 / 1 | 1 / 0 | 1 / 1 | 2 / 1 | 4 / 2 |
| `r.RayTracing.Denoiser.Iterations` | 4 | 3 | 4 | 4 | 5 |
| `r.RayTracing.BLAS.LOD` | −1 | −1 | −1 | −1 | 0 |

Подробно эффекты описаны в главах [20](20-lighting-shadows.md) (тени), [21](21-reflections-gi.md) (отражения, AO, GI),
[22](22-volumetrics.md) (туман и облака), [23](23-transparency-water-particles.md) (вода и частицы),
[24](24-ray-tracing.md) (RT), [25](25-upscalers-postprocess.md) (AA, апскейлеры, пост).

**Вне групп** (не меняются уровнем, только вручную/меню): `r.ScreenPercentage` (25–200), `r.Upscaler`
(`Off, FSR1, DLSS, TAAU`), `r.Upscaler.Quality` (`UltraPerformance` 33 %, `Performance` 50 %, `Balanced` 58 %,
`Quality` 67 %, `Native` 100 % / DLAA), `r.RayTracing`, `r.Tonemapper`, `r.Shadows.Caching`, `r.GpuDriven*` (кроме
LOD), `r.AsyncCompute`, `r.ParallelRecording`, художественные параметры (`r.Bloom.Intensity`, `r.AO.Radius`, …).

> **Важно: «По умолч.» ≠ High.** SSR, GTAO, bloom, DoF и motion blur в коде выключены, чтобы голый `Renderer`
> (тесты, утилиты) рисовал «чистое» изображение. Игра всегда выбирает уровень: `Settings` применяет
> `GraphicsSettings::quality` (по умолчанию `High`) при старте движка.

## Шаг 3. Сколько это стоит

GPU-время на Apple M4 Pro (MoltenVK), 1080p, из отчётов `PerfReport1080p` тестов рендера (`docs/dev/perf.md`,
`docs/dev/modules/render*.md`). Сцены разные — сравнивайте порядки, а не складывайте числа.

| Группа | Что измерено | мс |
| --- | --- | --- |
| — (база) | 400 экземпляров, солнце + 4 каскада, 64 точечных источника (8 с тенью), IBL — весь кадр | 3.4 |
| Shadows | там же: `Shadow.Cascades` 0.18 + `Shadow.Points` 0.14 + `ShadowMask` 0.19 | 0.51 |
| Shadows | стресс-сцена 50 000 экземпляров: `Shadow.Cascades` (самый дорогой пасс кадра) | 9.7 |
| GlobalIllumination | GTAO в полном разрешении (High) / `IndirectDiffuse` (объёмы облучённости) | 1.3 / 0.5 |
| Reflections | SSR (High) / планарное отражение 12×12 м при 0.75 / пробы (отсечение + композит) | 2.0 / 0.6 / 0.2 |
| Volumetrics | туман + 8 объёмов + облака: Low / Medium / High / Ultra | 0.17 / 0.72 / 1.14 / 3.80 |
| AntiAliasing | FXAA / TAA (+ CAS-резкость 0.18) | 0.11 / 0.56 |
| — (апскейлеры) | TAAU 540p→1080p / FSR 1 540p→1080p (EASU 0.32 + RCAS 0.15) | 0.55 / 0.47 |
| PostProcess | bloom (7 мипов) / DoF / motion blur / HDR-композит / LDR-пост | ≈0.3 / 0.2–0.35 / ≈0.22 / 0.2 / 0.11 |
| Effects | частицы: симуляция 0.03 + отрисовка 0.31 + глубина 0.06 + композит 0.06 | 0.46 |
| Shading / Reflections | поверхность воды High / Ultra | 0.48 / 0.73 |
| Foliage + Shading | ландшафт 2 км + ≈20k деревьев + ≈25k кустов травы + небо — весь кадр / `World.Forward` | ≈7.5 / ≈4.5 |
| RayTracing | денойзер SVGF на 1080p RGBA16F, 4 итерации (RT-пассы на Mac не измерить) | 5.7 |

Выводы для настройки: на M4 Pro самые дорогие ручки — **Volumetrics Ultra** (×3 к High), **SSR** и **GTAO** (их
половинное разрешение на Low/Medium экономит ~¾), **трава** (`r.Foliage.Grass` и `r.Foliage.Density`) и **тени
каскадов в плотных сценах**. Свои цифры смотрите в `RenderStats::passes` и оверлее статистики
([глава 27](27-gpu-driven-performance.md)).

## Шаг 4. Автоопределение качества

`autoDetectQuality(device)` прогоняет три коротких теста через render graph — blended-заливку RGBA16F (fill-rate),
FMA в compute (ALU) и копирование буфера (bandwidth), лучший из 3 прогонов, ≈30 мс, работает **headless**. Итог —
`score` (100 = GTX 1060 / RX 580) и уровни групп.

```cpp
#include <oxwald/render/quality.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>

const ox::render::BenchmarkResult bench = ox::render::autoDetectQuality(*device);
if (bench.valid) {                                  // false без timestamp-запросов
    OX_LOG_INFO("game", "{}", bench.toString());    // fill/ALU/bandwidth, score, уровни групп
    // Вариант 1: только группы.
    ox::render::applyQuality(bench);
    // Вариант 2 (рекомендуется): группы + AA + апскейлер.
    const ox::render::RecommendedSettings rec = ox::render::recommendedSettings(*device, bench.score);
    ox::render::applyRecommendedSettings(rec);      // sg.* + r.AntiAliasing + r.Upscaler(.Quality)
    showToast(rec.rationale);                       // "score 112 (High): native resolution with TAA"
}
```

Полные примеры: `samples/guide_examples/26-quality-settings/auto_detect.cpp` (CPU: отображение оценки, без GPU) и
`auto_detect_gpu.cpp` (настоящий бенчмарк на headless-устройстве, метка `gpu`).

**Оценка → уровни** (`levelsForScore(score, rayTracingSupported)`):

| `score` | Базовый уровень | GlobalIllumination, Volumetrics | RayTracing |
| --- | --- | --- | --- |
| < 35 | Low | Low | Low |
| 35–80 | Medium | на шаг ниже (если score < 250) | на шаг ниже базового; Low без поддержки RT |
| 80–160 | High | на шаг ниже (если score < 250) | то же |
| ≥ 160 | Ultra | Ultra только при score ≥ 250 | то же |

M4 Pro по замеру модуля: ≈35 GPix/s, 3.7 TFLOPS, 227 GB/s → score ≈ 112 → **High** (GI и Volumetrics — Medium).
Оценка плавает от прогона к прогону (в тесте главы под validation и параллельной нагрузкой вышло 90 — тоже High):
не привязывайте к точному `score` ничего, кроме порогов.

**Рекомендация AA и апскейлера** (`recommendedSettings`):

| Базовый уровень | RTX + DLSS доступен | Без DLSS |
| --- | --- | --- |
| Ultra | DLSS `Native` (DLAA) | натив + TAA |
| High | DLSS `Quality` | натив + TAA |
| Medium | DLSS `Balanced` | TAAU `Quality` (67 %) |
| Low | DLSS `Performance` | FSR 1 `Balanced` (58 %) + TAA |

Доступность апскейлеров для меню — `upscalerAvailability(device)`: для каждого `{name, available, temporal,
reason}`; без устройства известны только статические причины (на macOS DLSS: «Windows or Linux»). Подробнее об
апскейлерах — [глава 25](25-upscalers-postprocess.md).

**Где вызывать.** В рантайме проще всего — опция рендерера:

```cpp
ox::render::RuntimeRendererOptions opts;
opts.autoDetectQuality = isFirstLaunch;   // бенчмарк в init() + applyRecommendedSettings
engine.setRenderer(ox::render::createRenderer(opts));
```

Рендерер выполняет бенчмарк **в `init()`, после** применения `settings.json` и `--quality`, поэтому включайте
опцию только при первом запуске (нет `settings.json`), иначе выбор игрока будет перезаписываться при каждом старте.
Кнопка «Авто» в игровом меню (модуль `ui`) и «Auto-Detect» в редакторе запускают тот же бенчмарк на потоке рендера
между кадрами — бенчмарку нужны собственные кадры устройства ([глава 29](29-ui.md), [глава 30](30-editor.md)).

## Шаг 5. Настройки игрока

Меню графики работает с `GraphicsSettings` (поля и `settings.json` — в [главе 04](04-cvars-quality.md)). Для
рендерера важны:

| Поле `GraphicsSettings` | cvar | Значения |
| --- | --- | --- |
| `quality` | все `sg.*` | `Low`, `Medium`, `High` (по умолч.), `Ultra`; `Custom` — только `groups` |
| `groups` | `sg.<Группа>` | `{"Shadows": "Ultra", ...}` поверх `quality` |
| `upscaler` | `r.Upscaler` | `Off` (по умолч.), `FSR1`, `DLSS`, `TAAU` |
| `upscalerQuality` | `r.Upscaler.Quality` | `UltraPerformance`, `Performance`, `Balanced`, `Quality` (по умолч.), `Native` |
| `rayTracing` | `r.RayTracing` | `false`; без ray query рендерер принудительно выключает |

```cpp
ox::GraphicsSettings g = engine.settings().user().graphics;
g.quality = "Medium";
g.groups = {{"Shadows", "Ultra"}, {"Volumetrics", "Low"}};
g.upscaler = "TAAU";
g.upscalerQuality = "Balanced";
engine.settings().setGraphics(g);   // cvar'ы + сигнал changed → IRenderer::settingsChanged() перед следующим кадром

// Игрок поменял что-то в консоли (sg.Textures Ultra) — забрать в настройки перед сохранением:
engine.settings().captureFromCVars();   // quality станет "Custom", groups["Textures"] = "Ultra"
engine.settings().save();
```

Полный пример: `samples/guide_examples/26-quality-settings/user_settings.cpp`.

Для подсказок в меню берите значения уровня у самого cvar'а: `findAs<int>("r.Shadows.CSM.Resolution")->levelValue(
QualityLevel::Ultra)` → 4096, а описание — `description()`.

## Шаг 6. Свой cvar в группе

Фича игры (или своя фича рендера) объявляет cvar'ы с таблицей уровней — и автоматически подчиняется меню, кнопке
«Авто», `settings.json` и консоли:

```cpp
// cvar'ы фичи: статические объекты рядом с кодом, который их читает.               Low    Medium High   Ultra
ox::CVar<bool> cvRain("mygame.Rain", true, "Screen-space rain streaks", ox::Scalability::Effects, {false, true, true, true});
ox::CVar<int> cvRainDrops("mygame.Rain.Drops", 4096, "Rain drops per view", ox::Scalability::Effects, {1024, 2048, 4096, 8192});
ox::CVar<float> cvRainResolution("mygame.Rain.ResolutionScale", 1.0f, "Rain buffer scale", ox::Scalability::Effects,
                                 {0.5f, 0.5f, 1.0f, 1.0f});
ox::CVar<float> cvRainIntensity("mygame.Rain.Intensity", 1.0f, "Artistic amount (no scalability)", 0.0f, 4.0f);

class RainFeature final : public ox::render::IRenderFeature {
public:
    std::string_view name() const override { return "Rain"; }   // + тумблер r.Feature.Rain
    ox::render::InjectionMask injectionPoints() const override {
        return ox::render::maskOf(ox::render::InjectionPoint::Translucency);
    }
    std::vector<std::string> cvarNames() const override {        // группировка в UI настроек
        return {"mygame.Rain", "mygame.Rain.Drops", "mygame.Rain.ResolutionScale", "mygame.Rain.Intensity"};
    }
    // Каждый кадр: смена уровня включает/выключает фичу и перестраивает граф без перезапуска.
    bool isEnabled(const ox::render::RenderSettings&, const ox::rhi::DeviceCaps&) const override { return cvRain.get(); }
    void setup(ox::render::FeatureContext& ctx) override { /* пассы: cvRainDrops.get(), cvRainResolution.get() */ }
};

renderer->features().emplace<RainFeature>();
ox::scalability::setGroup(ox::Scalability::Effects, ox::QualityLevel::Low);   // дождь выключен, 1024 капли
```

Полный пример: `samples/guide_examples/26-quality-settings/custom_cvar.cpp` (тест проверяет через
`FeatureRegistry::resolve`, что фича пропадает из кадра на Low и при `r.Feature.Rain 0`).

Правила выбора группы и значений:

- **Группа — по тому, что игрок ожидает от ползунка.** Частицы, декали, дождь — `Effects`; всё, что про дальность, —
  `ViewDistance`; разрешения и сэмплы теней — `Shadows`. Не создавайте «свою» группу: их набор фиксирован
  (`Scalability::Count`), меню и автоопределение знают только эти 12.
- **High — это «как задумано на целевом железе».** Значение по умолчанию в коде обычно совпадает с High; Ultra —
  запас для мощных GPU, Low — «чтобы игра шла».
- **Художественные параметры — без уровней** (`cvRainIntensity`): группа их не трогает и не делает `Custom`.
- **Включение/выключение — через `isEnabled()`**, а не ранний выход из `setup()`: тогда пассы и ресурсы фичи не
  объявляются, и граф их не держит.
- **Дорогие группы GI и Volumetrics** автоопределение ставит на шаг ниже — кладите туда то, что реально дорого.

## Типичные ошибки и подводные камни

- **Голый `Renderer` без уровня.** В тестах и утилитах SSR, AO, bloom, DoF, motion blur выключены значениями по
  умолчанию. Хотите «как в игре» — вызовите `scalability::setOverall(QualityLevel::High)`.
- **`autoDetectQuality = true` при каждом запуске.** Бенчмарк в `init()` перезапишет `settings.json` и `--quality`.
  Включайте только при первом запуске или по кнопке.
- **Бенчмарк посреди кадра.** `autoDetectQuality` сам открывает и закрывает кадры устройства. Вызывайте его до
  первого кадра или между кадрами на потоке рендера (так делают модуль `ui` и редактор).
- **`r.RayTracing 1` на Mac.** Без ray query рендерер принудительно выставляет `RenderSettings::rayTracing = false`;
  группа RayTracing на таком GPU ничего не меняет (автоопределение держит её на Low).
- **`r.Upscaler DLSS` без RTX.** Тихо откатывается на TAAU. Проверяйте `upscalerAvailability()` и показывайте
  `reason` в меню.
- **Ручная правка группы показывает `Custom`.** Это ожидаемо; `savePreset`/`captureFromCVars` сохранят правку.
  Повторный выбор уровня вернёт табличные значения.
- **`upscalerQuality = "DLAA"`.** Значения cvar'а — `UltraPerformance … Native`; DLAA — это `Native` при `DLSS`.
  Комментарий в `settings.hpp` упоминает «DLAA», но такую строку cvar не примет.
- **Меню до создания рендерера.** Cvar'ы фич ещё не зарегистрированы — таблица групп будет неполной. Значения,
  заданные по имени, «подождут» регистрации, но `cvars(group)` их не покажет.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`core/scalability.hpp`](../../engine/core/include/oxwald/core/scalability.hpp) | `setOverall`, `setGroup`, `currentLevel`, `overallLevel`, `cvars`, `groupName`, `levelName`, `savePreset`/`loadPreset` |
| [`core/cvar.hpp`](../../engine/core/include/oxwald/core/cvar.hpp) | `CVar<T>` с таблицей уровней, `Scalability`, `QualityLevel`, `ICVar::levelString` |
| [`render/render_settings.hpp`](../../engine/render/include/oxwald/render/render_settings.hpp) | `RenderSettings`, `fromCVars`, `registerRenderCVars` |
| [`render/quality.hpp`](../../engine/render/include/oxwald/render/quality.hpp) | `BenchmarkResult`, `BenchmarkOptions`, `runGpuBenchmark`, `autoDetectQuality`, `levelsForScore`, `applyQuality` |
| [`render/features/postprocess/postprocess.hpp`](../../engine/render/include/oxwald/render/features/postprocess/postprocess.hpp) | `RecommendedSettings`, `recommendedSettings`, `applyRecommendedSettings`, `upscalerAvailability`, `UpscalerType`, `UpscalerQuality`, `upscalerRenderScale` |
| [`render/render_feature.hpp`](../../engine/render/include/oxwald/render/render_feature.hpp) | `IRenderFeature::isEnabled`, `cvarNames`, `FeatureRegistry::toggle` |
| [`render/runtime_renderer.hpp`](../../engine/render/include/oxwald/render/runtime_renderer.hpp) | `RuntimeRendererOptions::autoDetectQuality` |
| [`runtime/settings.hpp`](../../engine/runtime/include/oxwald/runtime/settings.hpp) | `Settings`, `GraphicsSettings`, `captureFromCVars` |

Для разработчиков движка: [`docs/dev/modules/render.md`](../dev/modules/render.md) §7 и §11,
[`render_postprocess.md`](../dev/modules/render_postprocess.md), [`docs/dev/perf.md`](../dev/perf.md).

## Что дальше

- [04. CVar'ы, консоль и качество](04-cvars-quality.md) — механика cvar'ов, групп, `settings.json`, `.oxproj`.
- [18. Обзор рендеринга](18-rendering-overview.md) — кадр, фичи и граф.
- [25. Апскейлеры и постобработка](25-upscalers-postprocess.md) — FSR 1, TAAU, DLSS, TAA.
- [27. GPU-driven и производительность](27-gpu-driven-performance.md) — как измерить свою сцену.
- [29. UI](29-ui.md) — меню настроек графики и отладочный оверлей.
- [Оглавление](README.md)
