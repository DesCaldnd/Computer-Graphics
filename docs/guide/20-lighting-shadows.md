# 20. Освещение и тени

> Модуль `render` (таргет `Oxwald::render`, пространство имён `ox::render`, зонтичный заголовок `<oxwald/render/render.hpp>`). Компоненты света, окружения и камеры объявлены в модуле `scene` (`<oxwald/scene/components.hpp>`): `LightComponent`, `EnvironmentComponent`, `CameraComponent`. Глава для художников по свету, левел-дизайнеров и программистов, которые настраивают качество графики.

## Зачем

Освещение в Oxwald физически корректное. Солнце задаётся в люксах, лампы в люменах, экспозиция камеры в EV100, как у фотоаппарата. Поэтому сцена, выставленная по реальным значениям, сразу выглядит правдоподобно, а свет из одной сцены переносится в другую без подгонки.

Что даёт рендерер:

| Задача | Что есть |
| --- | --- |
| Источники | Направленный (солнце), точечный, прожектор. Прямоугольные площадные (`AreaRect`) пока игнорируются |
| Много источников | Clustered forward+: до 4096 локальных источников на вид, до 256 на кластер |
| Тени солнца | Каскадные карты теней (CSM): до 4 каскадов, стабилизация (без «дрожания» при движении камеры), плавный переход между каскадами |
| Тени ламп | Прожекторы — тайлы в общем атласе, размер по важности. Точечные — 6 граней в массиве текстур. Кэш: перерисовка только при движении |
| Мягкость | PCF по 16 точкам Пуассона с поворотом на пиксель, PCSS (контактное затвердевание) по размеру источника |
| Окружение | Процедурное небо, HDRI-куб или небо Preetham из модуля `world`; IBL: префильтрованный куб + SH9 для диффуза |
| Экспозиция | Физическая камера (диафрагма, выдержка, ISO) или ручной EV100; автоэкспозиция — в [главе 25](25-upscalers-postprocess.md) |

Всё настраивается cvar'ами и группами масштабируемости ([глава 04](04-cvars-quality.md)). Изменение любого cvar'а пересобирает граф кадра на следующем кадре, перезапуск не нужен.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `LightComponent` | Источник света на сущности. Позиция и направление берутся из трансформа: свет летит вдоль **локальной −Z** |
| `EnvironmentComponent` | Небо (HDRI или процедурное), яркость IBL, ссылка на солнце, высотный туман |
| `CameraComponent` | Проекция и физическая экспозиция (`aperture`, `shutterSpeed`, `iso`, `exposureCompensation`) |
| Кластер | Ячейка сетки 16×9×24 по экрану и глубине со списком источников, которые её касаются |
| CSM | Cascaded Shadow Maps: несколько карт теней солнца, каждая покрывает свой отрезок глубины |
| Атлас теней | Одна большая depth-текстура, поделённая на тайлы для прожекторов |
| PCF / PCSS | Фильтрация границы тени / мягкая тень, ширина которой растёт с расстоянием до препятствия |
| IBL | Image Based Lighting: освещение от неба через префильтрованный куб (блики) и SH9 (диффуз) |
| `RenderSettings` | Снимок всех cvar'ов рендера, который делается раз в кадр (`RenderSettings::fromCVars()`) |

## Шаг 1. Источники света

Свет — это компонент на сущности. Позиция и поворот берутся из трансформа. Направленный свет и прожектор светят вдоль локальной −Z, поэтому поворот удобно задавать через `lookRotation`.

```cpp
#include <oxwald/render/render.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>
using namespace ox;

// Солнце: направленный свет в люксах.
Entity sun = world.create("Sun");
sun.setRotation(lookRotation(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f))));
auto& sl = sun.add<LightComponent>();
sl.type = LightType::Directional;
sl.intensity = 100000.0f;  // лк: прямое солнце в ясный полдень
sl.color = {1.0f, 0.96f, 0.9f};
sl.sourceRadius = 0.27f;   // для солнца — угловой радиус в градусах (мягкость PCSS)

// Лампа: точечный свет в люменах (лампа накаливания 60 Вт ≈ 800 лм).
Entity bulb = world.create("Bulb");
bulb.setPosition({2.0f, 2.5f, 0.0f});
auto& pl = bulb.add<LightComponent>();
pl.type = LightType::Point;
pl.intensity = 800.0f;     // лм
pl.range = 6.0f;           // м: дальше свет обрезается
pl.sourceRadius = 0.05f;   // м: радиус колбы (мягкость тени)
pl.castShadows = true;

// Прожектор: тоже люмены, конус задаётся внутренним и внешним углом.
Entity spot = world.create("Spot");
spot.setPosition({0.0f, 4.0f, 2.0f});
spot.setRotation(lookRotation(glm::normalize(glm::vec3(0.0f, -1.0f, -0.5f))));
auto& sp = spot.add<LightComponent>();
sp.type = LightType::Spot;
sp.intensity = 2000.0f;
sp.range = 12.0f;
sp.innerConeAngle = 20.0f; // градусы: полная яркость внутри
sp.outerConeAngle = 30.0f; // градусы: ноль снаружи
sp.shadowResolution = 512; // подсказка размера тайла в атласе (0 = решает рендер)
```

Полный пример: `samples/guide_examples/20-lighting-shadows/lighting_cpu.cpp` (`GuideLighting.LightsAndEnvironmentInTheScene`). Тест извлекает сцену в `RenderSnapshot` и проверяет, что направление солнца — это мировая −Z сущности.

| Поле `LightComponent` | По умолчанию | Смысл |
| --- | --- | --- |
| `type` | `Point` | `Directional`, `Point`, `Spot`, `AreaRect` (пока не рендерится) |
| `color` | (1, 1, 1) | Цвет, линейный RGB |
| `intensity` | 800 | **Люксы** для `Directional`, **люмены** для `Point`/`Spot`. В инспекторе диапазон 0–200 000 |
| `range` | 10 м | Радиус действия точечного и прожектора; влияет и на стоимость (в скольких кластерах источник) |
| `innerConeAngle` / `outerConeAngle` | 20° / 30° | Конус прожектора: полная яркость внутри `inner`, плавный спад до нуля к `outer` (не больше 89°) |
| `castShadows` | `true` | Отбрасывает ли тень |
| `shadowResolution` | 0 | Подсказка размера тайла тени в текселях; 0 — по важности на экране |
| `shadowBias` | 0.0005 | Постоянное смещение глубины. **Действует только на солнце** |
| `shadowNormalBias` | 0.02 | Смещение по нормали: 0.01 ≈ 1 тексель карты теней |
| `sourceRadius` | 0 | Физический размер источника: метры для ламп, **градусы углового радиуса** для солнца. Задаёт ширину полутени PCSS |
| `volumetric`, `volumetricIntensity` | `true`, 1 | Видны в инспекторе в категории Volumetrics, но рендер их пока не читает (см. [главу 22](22-volumetrics.md)) |

![Инспектор источника света](images/editor/inspector_light.png)

**Какой свет — солнце.** Тени CSM, PCSS по угловому радиусу, яркость процедурного неба и объёмные лучи считаются только для одного направленного источника — солнца. Это тот, на который ссылается `EnvironmentComponent::sun`. Если ссылки нет, солнцем считается самый яркий направленный источник. Остальные направленные источники освещают сцену, но теней не дают.

## Шаг 2. Физические единицы и экспозиция

Рендер работает в абсолютных единицах:

| Величина | Единица | Как переводится внутри |
| --- | --- | --- |
| Направленный свет | лк (люкс) | Освещённость перпендикулярной поверхности |
| Точечный, прожектор | лм (люмен) | Сила света в канделах = лм / 4π. **Конус прожектора не концентрирует свет**: прожектор с теми же люменами светит так же ярко, как точечный, только в конусе |
| Небо (процедурное) | кд/м² | Зенит ≈ 10 % освещённости от солнца |
| Камера | EV100 | `exposure = 1 / (1.2 · 2^EV100)` |
| Emissive материала | относительно дисплея | 1 = белый при текущей экспозиции ([глава 19](19-materials.md)) |

Типичные значения для ориентира: солнце в ясный полдень около 100 000 лк, лампа накаливания 60 Вт около 800 лм. EV100 около 15 подходит для солнечного дня, около 5–7 — для освещённого интерьера.

Экспозиция берётся из камеры: `EV100 = log2(N² / t · 100 / ISO) − exposureCompensation`.

```cpp
CameraComponent cam;              // f/16, 1/125 с, ISO 100 — правило «солнечных 16»
cam.ev100();                      // ≈ 14.97
cam.exposureCompensation = 1.0f;  // +1 EV = в два раза светлее кадр
cam.ev100();                      // ≈ 13.97

CameraComponent indoor;
indoor.aperture = 2.8f;
indoor.shutterSpeed = 1.0f / 60.0f;
indoor.iso = 800.0f;
indoor.ev100();                   // ≈ 5.88: интерьер

// Рендер берёт EV100 из CameraParams (CameraParams::fromComponent копирует ev100()).
const CameraParams p = CameraParams::fromComponent(indoor, glm::mat4(1.0f));
```

Полный пример: `samples/guide_examples/20-lighting-shadows/lighting_cpu.cpp` (`GuideLighting.PhysicalCameraExposure`).

Экспозицию можно переопределить cvar'ами, например для скриншотов, отладки или сцен с фиксированным светом:

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.Exposure.Mode` | `Camera` | `Camera` — EV100 из камеры, `Manual` — из `r.Exposure.EV100` |
| `r.Exposure.EV100` | 10 | Ручная экспозиция (−10…24) |
| `r.Exposure.Compensation` | 0 | Добавочная компенсация в EV (−16…16). Положительная делает кадр светлее |
| `r.Tonemapper` | `ACES` | `ACES`, `AgX`, `Neutral`, `Linear` — подробно в [главе 25](25-upscalers-postprocess.md) |

```cpp
ScopedCVar mode("r.Exposure.Mode", "Manual");   // игнорировать EV100 камеры
ScopedCVar ev("r.Exposure.EV100", "15");        // +3 EV = в 8 раз меньше света на «сенсоре»
```

`ScopedCVar` — маленький помощник из примеров (`guide_render_scene.hpp`). Он ставит значение через `CVarRegistry::set` и восстанавливает прежнее при выходе из области видимости. В игре то же самое делается строкой консоли `r.Exposure.Mode Manual`.

## Шаг 3. Много источников: кластеры

Forward+ не освещает каждый пиксель всеми источниками. Видимый объём делится на сетку **16×9 плиток × 24 среза** по глубине, срезы растут экспоненциально. Компьют-проход `LightCulling` раскладывает источники по кластерам: сфера источника (у прожектора — сфера вокруг конуса) проверяется против AABB кластера. Затем пиксель перебирает только список своего кластера.

```cpp
ClusterGrid g;
g.nearPlane = 0.1f;
g.farPlane = 500.0f;          // = min(camera far, r.Clusters.MaxDistance)
g.clusterCount();             // 16 × 9 × 24 = 3456
g.slice(1000.0f);             // 23: дальше far — последний срез
g.bufferSize();               // (1 + maxLightsPerCluster) u32 на кластер ≈ 3.5 МБ при 256
```

Полный пример: `samples/guide_examples/20-lighting-shadows/lighting_cpu.cpp` (`GuideLighting.ClusterGrid`).

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.Lights.Max` | 4096 | Сколько локальных источников загружается на вид. Лишние отбрасываются: сначала дальние (по расстоянию до края `range`) |
| `r.Clusters.MaxLightsPerCluster` | 256 | Ёмкость списка кластера (16–1024). Переполнение — источники «пропадают» пятнами |
| `r.Clusters.MaxDistance` | 500 м | Дальняя граница сетки, если у камеры бесконечная дальняя плоскость или она дальше |

Стоимость определяется не общим числом источников, а **числом источников на пиксель**. Его показывает `r.DebugView LightComplexity` (тепловая карта). Главный рычаг — `range`: огромный радиус у слабой лампы заставляет её участвовать в тысячах кластеров.

## Шаг 4. Тени солнца: каскады

Солнце отбрасывает тень через CSM. Дистанция тени `r.Shadows.CSM.Distance` делится на `r.Shadows.CSM.Cascades` отрезков по «практической» схеме. Это смесь равномерного и логарифмического разбиения с весом `r.Shadows.CSM.Lambda`. Каскад — ортографическая проекция вокруг ограничивающей сферы своего отрезка. Поэтому его размер не зависит от поворота камеры, а начало координат привязано к целым текселям, и тени не «дрожат» при движении.

```cpp
const std::vector<f32> splits = cascadeSplits(0.1f, 120.0f, 4, 0.75f); // дальние границы 4 каскадов

CascadeInput in;
in.cameraWorld = CameraParams::lookAt({0, 5, 10}, {0, 0, 0}).world;
in.aspect = 16.0f / 9.0f;
in.shadowDistance = 120.0f; // r.Shadows.CSM.Distance
in.cascadeCount = 4;        // r.Shadows.CSM.Cascades
in.resolution = 2048;       // r.Shadows.CSM.Resolution
in.lightDirection = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.2f));
const std::vector<Cascade> cascades = computeCascades(in);
cascades[0].texelWorld;     // размер текселя в метрах = 2 · radius / resolution
```

Эти функции — та же математика, что использует фича `ShadowsRaster`. Они пригодятся, чтобы оценить, сколько сантиметров приходится на тексель на нужной дистанции. Полный пример: `samples/guide_examples/20-lighting-shadows/lighting_cpu.cpp` (`GuideLighting.CascadeSplitsAndStableCascades`).

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.Shadows` | `true` | Все растровые тени |
| `r.Shadows.CSM.Resolution` | 2048 | Размер каждого каскада (текстура-массив D32) |
| `r.Shadows.CSM.Cascades` | 4 | Число каскадов, 1–4 |
| `r.Shadows.CSM.Distance` | 120 м | Дальность теней солнца (не дальше дальней плоскости камеры) |
| `r.Shadows.CSM.Lambda` | 0.75 | 0 — равномерные отрезки, 1 — логарифмические (больше разрешения вблизи) |
| `r.Shadows.CSM.Blend` | 0.1 | Доля каскада, в которой он плавно переходит в следующий |

Каскады рисуются **каждый кадр**: кэша для солнца нет. Экранная маска `ShadowMask` (видимость солнца на пиксель) строится по буферу глубины отдельным проходом. Режим отладки `r.DebugView ShadowCascades` раскрашивает каскады, `r.DebugView ShadowMask` показывает маску.

## Шаг 5. Прожекторы и точечные: атлас, бюджет и кэш

Локальные тени распределяются **по важности**. Важность — это размер сферы действия источника на экране (в пикселях высоты) × приоритет. Если камера внутри `range`, важность равна полной высоте экрана.

- **Прожектор** получает квадратный тайл в атласе `r.Shadows.AtlasSize`. Размер тайла — степень двойки, пропорциональная важности, в пределах `SpotMinResolution…SpotMaxResolution`. Если атлас полон, тайлы менее важных источников сначала уменьшаются вдвое, и только потом источники лишаются тени.
- **Точечный** занимает слот из 6 граней размером `r.Shadows.PointResolution`. Грани рисуются одним инстансированным проходом (на устройствах с `shaderOutputLayer`), FOV граней слегка расширен, чтобы ядро PCF не выходило за грань.
- Всего теней не больше `r.Shadows.MaxShadowedLights`, из них точечных не больше `r.Shadows.MaxPointShadows`.

```cpp
ShadowBudget budget;            // как в ShadowsRaster, из r.Shadows.* (уровень High)
budget.atlasSize = 4096;
budget.minResolution = 128;     // r.Shadows.SpotMinResolution
budget.maxResolution = 1024;    // r.Shadows.SpotMaxResolution
budget.maxShadowedLights = 16;
budget.maxPointLights = 8;
budget.pointResolution = 512;

ShadowAtlasAllocator atlas(budget.atlasSize, budget.minResolution);
const std::vector<ShadowAllocation> allocs =
    allocateShadows(requests, cameraPos, glm::radians(60.0f), 1080.0f, budget, atlas);
// allocs отсортированы по важности; у прожектора — tile {x, y, size}, у точечного — cubeSlot
```

Полный пример: `samples/guide_examples/20-lighting-shadows/lighting_cpu.cpp` (`GuideLighting.ShadowAtlasBudget`).

**Кэш теней** (`r.Shadows.Caching`, по умолчанию включён). Тайл или куб локального источника перерисовывается, только если изменилось его размещение в атласе или сам источник, либо если двигавшийся (или удалённый) объект задел сферу действия источника — старыми или новыми границами. В статичной сцене локальные тени почти бесплатны. Проверить кэш можно по статистике:

```cpp
render(cam, 128, 128, 3);
renderer->stats().shadowedLights;     // 2: прожектор и точечный
renderer->stats().shadowMapsRendered; // 0: ничего не двигалось
renderer->stats().shadowMapsCached;   // 2: оба взяты из кэша
```

Полный пример: `samples/guide_examples/20-lighting-shadows/lighting_gpu.cpp` (`GuideLightingGpu.LocalLightShadowsAreCached`).

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.Shadows.AtlasSize` | 4096 | Сторона атласа прожекторов |
| `r.Shadows.SpotMinResolution` / `SpotMaxResolution` | 128 / 1024 | Границы размера тайла прожектора |
| `r.Shadows.PointResolution` | 512 | Размер грани куба точечного источника |
| `r.Shadows.MaxShadowedLights` | 16 | Всего локальных теней на вид |
| `r.Shadows.MaxPointShadows` | 8 | Из них точечных (каждая = 6 граней) |
| `r.Shadows.Caching` | `true` | Кэш локальных теней |

## Шаг 6. Фильтрация: PCF и PCSS

Все карты теней читаются аппаратным сравнением глубины (`sampler2DShadow`). Поверх него работает PCF: `r.Shadows.PCFTaps` точек диска Пуассона в радиусе `r.Shadows.FilterRadius` текселей, диск поворачивается на каждый пиксель. При 0 точек остаётся одно аппаратное сравнение — жёсткая, «лесенкой», но самая дешёвая тень.

**PCSS** (`r.Shadows.PCSS`) сначала ищет блокеры, затем расширяет фильтр пропорционально расстоянию от блокера до приёмника и размеру источника. У основания столба тень резкая, вдали — размытая. Размер источника задаёт `LightComponent::sourceRadius`: для ламп в метрах, для солнца в градусах углового радиуса (реальное Солнце ≈ 0.27°). Ширина полутени PCSS ограничена: 1–24 текселя каскада для солнца и 1–16 текселей для ламп, угловой радиус солнца — не больше 0.2 рад (≈ 11.5°). У лампы с `sourceRadius = 0` PCSS не включается, остаётся обычный PCF. У солнца с нулевым радиусом полутень сужается до 1 текселя, то есть тень становится резче, чем при чистом PCF.

**Смещения (bias).** Против «теневых прыщей» (shadow acne) работают наклонное растровое смещение и смещение по нормали `shadowNormalBias` (0.01 ≈ 1 тексель). `shadowBias` дополнительно сдвигает глубину, но только для солнца. Если тень «отрывается» от основания объекта (peter-panning), уменьшайте `shadowNormalBias`. Если по освещённым поверхностям ползут полосы, увеличивайте его.

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.Shadows.PCFTaps` | 16 | Число точек PCF, 0–16 (0 = одно аппаратное сравнение) |
| `r.Shadows.FilterRadius` | 1.5 | Радиус PCF в текселях |
| `r.Shadows.PCSS` | `true` | Мягкие тени с контактным затвердеванием |

## Шаг 7. Небо, IBL и окружение

`EnvironmentComponent` (одна на сцену, берётся первая активная) задаёт фон и непрямой свет:

```cpp
Entity envEntity = world.create("Environment");
auto& env = envEntity.add<EnvironmentComponent>();
env.sun = EntityRef(sun.get<IdComponent>().id); // какой направленный свет — солнце
env.skyIntensity = 1.0f;     // множитель процедурного неба / HDRI
env.ambientIntensity = 1.0f; // множитель IBL (диффуз и отражения неба)
// env.skybox = <Uuid кубической HDRI-текстуры>; без него — процедурное небо
```

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `skybox` | нет | Кубическая HDR-текстура неба. Равнопромежуточные (equirect) HDRI в куб **не конвертируются** — нужен куб |
| `skyIntensity` | 1 | Яркость неба (фон и источник IBL) |
| `sun` | нет | Ссылка на направленный свет-солнце (иначе — самый яркий) |
| `ambientIntensity` | 1 | Множитель IBL (вместе с `r.IBL.Intensity`) |
| `fogEnabled`, `fogColor`, `fogDensity`, `fogHeightFalloff`, `fogStartDistance` | выкл., …, 0.01, 0.2, 0 | Высотный туман — [глава 22](22-volumetrics.md) |

Источник неба выбирается так: HDRI-куб (если задан `skybox`), иначе небо Preetham, если его передал модуль `world` (время суток, [глава 28](28-world-rendering.md)), иначе процедурное небо. Из неба строится IBL: GGX-префильтрованный куб размера `r.IBL.Resolution` для бликов и SH9 для диффуза. Пересчёт идёт только при изменении окружения (режим неба, HDRI, коэффициенты Preetham, квантованное направление и цвет солнца, яркости). Если вы правите окружение «на месте» из инструментов, вызовите `renderer->invalidateEnvironment()`.

Локальные отражения (пробы, SSR, планарные зеркала) и запечённый непрямой свет заменяют IBL там, где они есть. Это тема [главы 21](21-reflections-gi.md).

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.Sky` | `true` | Рисовать небо (фон) |
| `r.IBL` | `true` | Освещение от окружения |
| `r.IBL.Intensity` | 1 | Глобальный множитель IBL (0–16) |
| `r.IBL.Resolution` | 128 | Размер грани префильтрованного куба |
| `r.Shading.MultiScatter` | `true` | Компенсация энергии многократного рассеяния GGX: шероховатые металлы не темнеют |

## Шаг 8. Проверка на GPU

Примеры главы рендерят сцену без окна: headless-устройство, `Renderer`, offscreen-текстура. Цикл кадра тот же, что у рантайма ([глава 18](18-rendering-overview.md)):

```cpp
extract(*world, snapshot);                  // ECS → RenderSnapshot (игровой поток)
for (u32 f = 0; f < frames; ++f) {
    device->beginFrame();
    renderer->beginFrame(snapshot);         // снимок cvar'ов, загрузки, инстансы
    ViewRenderRequest req;
    req.view = view;
    req.camera = camera;                    // CameraParams: lookAt(...) или fromComponent(...)
    req.target.texture = target;
    req.target.finalAccess = rhi::Access::TransferRead;
    renderer->renderView(req);
    renderer->endFrame();                   // RenderStats
    device->endFrame();
}
```

Тест `SunShadowsOnlyDarken` рендерит сцену с солнцем, проверяет, что в кадре были проходы `Shadow.Cascades` и `ShadowMask`, затем выключает `r.Shadows`. Граф перестраивается без теневых проходов, а картинка становится светлее. Тест `ShadowQualityLevelsRender` проходит все уровни группы `Shadows` и проверяет `renderer->settings().pcss`.

Полный пример: `samples/guide_examples/20-lighting-shadows/lighting_gpu.cpp`. Обвязка headless-рендера: `samples/guide_examples/20-lighting-shadows/guide_render_scene.hpp` (её используют и главы 21–22).

## Уровни качества

Группа `Shadows` (Low / Medium / High / Ultra), значения из `engine/render/src/settings.cpp`:

| CVar | По умолчанию | Low | Medium | High | Ultra |
| --- | --- | --- | --- | --- | --- |
| `r.Shadows.CSM.Resolution` | 2048 | 1024 | 2048 | 2048 | 4096 |
| `r.Shadows.CSM.Cascades` | 4 | 2 | 3 | 4 | 4 |
| `r.Shadows.CSM.Distance` | 120 | 60 | 100 | 150 | 250 |
| `r.Shadows.AtlasSize` | 4096 | 2048 | 4096 | 4096 | 8192 |
| `r.Shadows.PointResolution` | 512 | 256 | 512 | 512 | 1024 |
| `r.Shadows.MaxShadowedLights` | 16 | 4 | 8 | 16 | 32 |
| `r.Shadows.MaxPointShadows` | 8 | 2 | 4 | 8 | 12 |
| `r.Shadows.PCFTaps` | 16 | 0 | 8 | 16 | 16 |
| `r.Shadows.PCSS` | `true` | выкл. | выкл. | вкл. | вкл. |
| `r.Shadows.SpotMaxResolution` | 1024 | 512 | 1024 | 1024 | 2048 |

Группа `Shading`:

| CVar | По умолчанию | Low | Medium | High | Ultra |
| --- | --- | --- | --- | --- | --- |
| `r.IBL.Resolution` | 128 | 64 | 128 | 128 | 256 |
| `r.Shading.MultiScatter` | `true` | выкл. | вкл. | вкл. | вкл. |

Без групп: `r.Shadows`, `r.Shadows.CSM.Lambda` (0.75), `r.Shadows.CSM.Blend` (0.1), `r.Shadows.FilterRadius` (1.5), `r.Shadows.SpotMinResolution` (128), `r.Shadows.Caching`, `r.Lights.Max`, `r.Clusters.*`, `r.IBL`, `r.IBL.Intensity`, `r.Sky`, `r.Exposure.*`, `r.Tonemapper`.

```cpp
scalability::setGroup(Scalability::Shadows, QualityLevel::Low);
RenderSettings low = RenderSettings::fromCVars(); // снимок, который рендер делает раз в кадр
low.csmResolution;  // 1024
low.pcfTaps;        // 0: одно аппаратное сравнение

CVarRegistry::instance().execute("r.Shadows.CSM.Distance 400"); // правка поверх уровня
scalability::currentLevel(Scalability::Shadows);                 // Custom
```

Полный пример: `samples/guide_examples/20-lighting-shadows/lighting_cpu.cpp` (`GuideLighting.ShadowCVarsAndScalability`). Автоопределение качества по бенчмарку GPU (`autoDetectQuality`, на M4 Pro ≈ High) и меню настроек описаны в [главе 26](26-quality-settings.md).

## Стоимость

Замер `RendererTest.PerfReport1080p` (Apple M4 Pro, 1080p): 400 инстансов (548 draw call'ов, 1.08 млн треугольников), солнце с 4 каскадами, 64 точечных источника, из них 8 с тенями (кэш выключен: 52 карты за кадр), IBL. **GPU 3.4 мс** на кадр.

| Проход | мс |
| --- | --- |
| `DepthPrepass` | 0.14 |
| `LightCulling` | 0.02 |
| `Shadow.Cascades` | 0.18 |
| `Shadow.Points` | 0.14 |
| `ShadowMask` | 0.19 |
| `ForwardOpaque` | 2.63 |
| `Sky` | 0.06 |
| `Tonemap` / `Final` | 0.06 / 0.02 |

Основная стоимость — `ForwardOpaque`: в нём для каждого пикселя считаются источники кластера, PCF/PCSS и IBL. На больших сценах тени солнца становятся самым дорогим проходом: каскады растеризуют все отбрасывающие тень объекты внутри себя. Цифры для десятков тысяч инстансов приведены в [главе 27](27-gpu-driven-performance.md).

## Отладка

| `r.DebugView` | Что показывает |
| --- | --- |
| `LightComplexity` | Тепловая карта числа источников на кластер |
| `ShadowCascades` | Раскраска каскадов CSM |
| `ShadowMask` | Экранная видимость солнца |
| `Albedo`, `Normals`, `Roughness`, `Metallic`, `AO`, `Emissive` | Каналы поверхности |

Время каждого прохода — `renderer->stats().passes` (имена вида `<вид>/<проход>`, данные последнего завершённого GPU-кадра). Счётчики теней — `shadowedLights`, `shadowMapsRendered`, `shadowMapsCached`. В редакторе и игре всё это видно в оверлее статистики ([глава 29](29-ui.md)).

## Типичные ошибки и подводные камни

- **Солнце в люменах, лампа в люксах.** Направленный свет задаётся в люксах (десятки тысяч), точечный и прожектор — в люменах (сотни–тысячи). Лампа на 100 000 лм выжжет кадр.
- **Кадр чёрный или белый.** Экспозиция камеры по умолчанию (f/16, 1/125, ISO 100 → EV100 ≈ 15) рассчитана на солнечный день. Для интерьера или ночи откройте диафрагму или поставьте `r.Exposure.Mode Manual` с подходящим `r.Exposure.EV100`.
- **Прожектор «тусклее, чем ожидалось».** Конус не концентрирует световой поток: при тех же люменах прожектор не ярче точечного.
- **Свет направлен не туда.** Направленный свет и прожектор светят вдоль **локальной −Z**. Используйте `lookRotation(direction)`, а не вектор «вверх».
- **Нет тени от второго солнца.** Тени CSM есть только у одного направленного источника — того, что в `EnvironmentComponent::sun` (или самого яркого).
- **`shadowBias` у лампы ничего не меняет.** Для локальных источников он не используется — крутите `shadowNormalBias`.
- **Огромный `range`.** Источник попадает в тысячи кластеров, а его тень получает высокую важность. Ставьте `range` по реальной зоне освещения.
- **Тени ламп «исчезают» при большом их числе.** Работает бюджет: `MaxShadowedLights`/`MaxPointShadows` и место в атласе. Самые далёкие и мелкие на экране источники теряют тень первыми. Важному источнику можно подсказать размер через `shadowResolution`.
- **Тень солнца обрывается вдали.** Это граница `r.Shadows.CSM.Distance` (на Low — 60 м). На уровне High значение 150 м, хотя объявленное значение по умолчанию — 120 м.
- **Движущийся объект рядом с десятком ламп.** Кэш перерисует тени всех ламп, чьих сфер он касается. Анимированные объекты держите подальше от «статичных» теневых ламп или выключайте им `castShadows`.
- **Equirect-HDRI вместо неба.** Используется только кубическая текстура. Конвертируйте панораму в куб при импорте.
- **Площадные источники.** `LightType::AreaRect` пока игнорируется рендером.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`scene/components.hpp`](../../engine/scene/include/oxwald/scene/components.hpp) | `LightComponent`, `LightType`, `EnvironmentComponent`, `CameraComponent` (`ev100`, `exposure`) |
| [`render.hpp`](../../engine/render/include/oxwald/render/render.hpp) | Зонтичный заголовок рендера |
| [`render_settings.hpp`](../../engine/render/include/oxwald/render/render_settings.hpp) | `RenderSettings`, `fromCVars`, `registerRenderCVars`, `Tonemapper`, `ExposureMode`, `DebugView` |
| [`shadows.hpp`](../../engine/render/include/oxwald/render/shadows.hpp) | `cascadeSplits`, `computeCascades`, `CascadeInput`, `ShadowAtlasAllocator`, `ShadowRequest`, `ShadowBudget`, `allocateShadows`, `cubeFaceViewProj` |
| [`clusters.hpp`](../../engine/render/include/oxwald/render/clusters.hpp) | `ClusterGrid`, `sphereIntersectsAabb` |
| [`snapshot.hpp`](../../engine/render/include/oxwald/render/snapshot.hpp) | `extract`, `RenderSnapshot`, `SnapshotLight`, `SnapshotEnvironment` |
| [`render_view.hpp`](../../engine/render/include/oxwald/render/render_view.hpp) | `CameraParams` (`ev100`, `fromComponent`, `lookAt`) |
| [`render_stats.hpp`](../../engine/render/include/oxwald/render/render_stats.hpp) | `RenderStats` (`passes`, `shadowedLights`, `shadowMapsRendered`, `shadowMapsCached`, `lights`) |
| [`quality.hpp`](../../engine/render/include/oxwald/render/quality.hpp) | `autoDetectQuality`, `applyQuality`, `levelsForScore` |

Шейдерные включения (`engine/shaders/render/common/`): `lighting.glsl` (`oxEvaluateLighting`), `shadows.glsl`, `clusters.glsl`, `pbr.glsl`. Заметки для разработчиков модуля: [`docs/dev/modules/render.md`](../dev/modules/render.md), §5 и §7.

## Что дальше

- [18. Обзор рендеринга](18-rendering-overview.md) — кадр, фичи, снимок сцены, рантайм и редактор.
- [19. Материалы](19-materials.md) — PBR-параметры поверхностей, которые освещает этот свет.
- [21. Отражения и GI](21-reflections-gi.md) — пробы отражений, SSR, AO и запечённый непрямой свет поверх IBL.
- [22. Объёмные эффекты](22-volumetrics.md) — туман, лучи света и облака.
- [24. Трассировка лучей](24-ray-tracing.md) — RT-тени вместо карт теней.
- [26. Настройки качества](26-quality-settings.md) — пресеты, автоопределение, меню графики.
- [Оглавление](README.md).
