# 22. Объёмные эффекты: туман, лучи света, облака

> Модуль `render` (таргет `Oxwald::render`, пространство имён `ox::render`, функции фичи — `ox::render::volumetrics`). Заголовки: компоненты — `<oxwald/render/components/volumetrics.hpp>`, фича и снимок — `<oxwald/render/features/volumetrics/volumetrics.hpp>`. Одна фича `Volumetrics` (переключатель `r.Feature.Volumetrics`).

## Зачем

Воздух в кадре — один из самых сильных инструментов настроения: дымка на горизонте, туман в низине, пыльные лучи из окон собора, конус фонаря в ночном тумане, облака над открытым миром. Фича `Volumetrics` делает это физически: свет рассеивается в среде с учётом теней, а не рисуется спрайтами.

| Задача | Что есть |
| --- | --- |
| Дымка и высотный туман | Глобальный туман из `EnvironmentComponent`: плотность у земли, экспоненциальное убывание с высотой |
| Туман в отдельных местах | `FogVolumeComponent`: коробка, сфера или эллипсоид с мягким краем, 3D-шумом и ветром |
| Лучи света (god rays) | Тени солнца, прожекторов и точечных источников внутри тумана; рассеяние Хеньи–Гринстейна (вперёд/назад) |
| Облака | `CloudLayerComponent`: ray marching слоя облаков (в духе Nubis), многократное рассеяние, «серебряная кайма» |
| Дальний план | Аналитический туман за сеткой и «воздушная перспектива» неба до `r.VolumetricFog.SkyDistance` |

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| Фроксель (froxel) | Ячейка 3D-сетки, привязанной к камере: X×Y по экрану, Z — срезы по глубине (frustum + voxel) |
| Экстинкция | Сколько света теряется на метр, 1/м. `density` компонентов задаётся именно в ней |
| Альбедо | Доля экстинкции, которая рассеивается, а не поглощается: `scattering = density · albedo` |
| Анизотропия `g` | Параметр фазовой функции Хеньи–Гринстейна: > 0 — рассеяние вперёд (лучи ярче против солнца), < 0 — назад |
| Пропускание | Доля света фона, дошедшая до камеры. Композит: `цвет = фон · T + рассеянный свет` |
| `VolumetricFog` | 3D-текстура RGBA16F: накопленный рассеянный свет и пропускание до дальнего края каждого среза |
| Шахматка облаков | Каждый кадр трассируется только 1/16 (или 1/4) пикселей облаков, остальные восстанавливаются по истории |

## Шаг 1. Высотный туман

Глобальный туман задаётся полями `EnvironmentComponent` — теми же, что использует простой туман без фроксельной фичи:

```cpp
#include <oxwald/render/features/volumetrics/volumetrics.hpp>
#include <oxwald/render/register_types.hpp>
using namespace ox;
using namespace ox::render;

// Глобальный высотный туман живёт в EnvironmentComponent.
auto& env = world.create("Environment").add<EnvironmentComponent>();
env.fogEnabled = true;
env.fogDensity = 0.02f;        // экстинкция (1/м) на высоте y = 0
env.fogHeightFalloff = 0.1f;   // экспоненциальное убывание с высотой
env.fogColor = {0.8f, 0.85f, 0.9f}; // альбедо рассеяния
env.fogStartDistance = 5.0f;   // первые 5 м от камеры без тумана
```

Полный пример: `samples/guide_examples/22-volumetrics/volumetrics_cpu.cpp` (`GuideVolumetrics.FogAndCloudsOnTheScene`).

| Поле `EnvironmentComponent` | По умолчанию | Смысл |
| --- | --- | --- |
| `fogEnabled` | `false` | Включить высотный туман |
| `fogDensity` | 0.01 | Экстинкция у земли (y = 0), 1/м. 0.01 — лёгкая дымка, 0.05–0.1 — густой туман |
| `fogHeightFalloff` | 0.2 | Скорость убывания плотности с высотой; 0 — однородный туман на любой высоте |
| `fogColor` | (0.6, 0.7, 0.8) | Альбедо рассеяния. Цвет тумана в кадре даёт **свет**, а `fogColor` его окрашивает |
| `fogStartDistance` | 0 м | Расстояние от камеры, с которого начинается туман |

Цвет тумана не «рисуется», а получается из освещения: солнце, небо (IBL) и локальные источники рассеиваются в среде. Ночью туман тёмный, а у фонаря светится. При сильном боковом солнце видны лучи в тенях объектов.

Если фича `Volumetrics` выключена (`r.VolumetricFog 0` или `r.Feature.Volumetrics 0`), те же поля дают дешёвый аналитический туман прямо в шейдере освещения: без теней и лучей, но тоже с высотным спадом.

## Шаг 2. Локальные объёмы тумана

`FogVolumeComponent` — туман в форме: низина, пар над водой, дым над костром, пыль в комнате. Форма центрирована на сущности и следует её трансформу целиком, включая поворот и масштаб.

```cpp
// Локальный объём: туман в низине, с шумом и ветром.
Entity mist = world.create("SwampMist");
mist.setPosition({10.0f, 1.0f, -20.0f});
auto& fv = mist.add<FogVolumeComponent>();
fv.shape = FogVolumeShape::Ellipsoid;
fv.extents = {12.0f, 2.0f, 8.0f};    // полуразмеры (м), масштаб сущности тоже учитывается
fv.density = 0.25f;
fv.albedo = {0.85f, 0.9f, 0.8f};
fv.falloff = 0.4f;                   // мягкий край: 40 % формы от границы внутрь
fv.noiseIntensity = 0.7f;            // клочья
fv.noiseScale = 6.0f;
fv.noiseVelocity = {0.3f, 0.0f, 0.0f};
```

| Поле `FogVolumeComponent` | По умолчанию | Смысл |
| --- | --- | --- |
| `shape` | `Box` | `Box`, `Sphere` (радиус = `extents.x`), `Ellipsoid` |
| `extents` | (2, 2, 2) м | Полуразмеры в локальных осях |
| `density` | 0.1 | Экстинкция в центре, 1/м |
| `albedo` | (1, 1, 1) | Альбедо рассеяния |
| `emission` | (0, 0, 0) | Собственное свечение (как emissive материала: 1 = белый при текущей экспозиции). Лава, магический туман |
| `falloff` | 0.25 | Доля формы от границы внутрь, на которой плотность нарастает от 0 |
| `anisotropy` | 0 | `g` Хеньи–Гринстейна этого объёма (−0.95…0.95) |
| `noiseIntensity` | 0 | 0 — однородный, 1 — полностью модулирован 3D-шумом Перлина–Уорли |
| `noiseScale` | 6 м | Размер тайла шума |
| `noiseVelocity` | (0, 0, 0) | Скорость прокрутки шума, м/с |
| `windInfluence` | 1 | Множитель глобального ветра мира |

Объёмов в кадре — до `r.VolumetricFog.MaxVolumes` (64, ближайшие первыми). Каждый проверяется в каждом фрокселе: разбиения по тайлам пока нет, поэтому сотни мелких объёмов дороги.

**Ветер.** Глобальный ветер мира прокручивает шум объёмов и облака. Его передаёт мост мира ([глава 28](28-world-rendering.md)) через `volumetrics::setWorldWind(snapshot, {dir.x · speed, 0, dir.y · speed})` в extract hook. Свой источник ветра можно подключить так же:

```cpp
volumetrics::setWorldWind(snap, {4.0f, 0.0f, 1.0f}); // м/с, мировые оси; действует до следующего extract
```

## Шаг 3. Глобальные настройки тумана и лучи света

`VolumetricFogComponent` необязателен (используется первый активный в мире). Он переопределяет cvar'ы для конкретного уровня: в пещере нужны другие лучи, чем в поле.

```cpp
// Глобальные переопределения фроксельного тумана (первый активный в мире).
auto& vf = world.create("FogSettings").add<VolumetricFogComponent>();
vf.anisotropy = 0.7f;   // сильнее рассеяние вперёд → ярче лучи против солнца
vf.distance = 96.0f;    // дальность сетки (0 = r.VolumetricFog.Distance)
```

| Поле `VolumetricFogComponent` | По умолчанию | Смысл |
| --- | --- | --- |
| `anisotropy` | 0.6 | `g` высотного тумана (у объёмов — свой). Без компонента — `r.VolumetricFog.Anisotropy` |
| `ambientIntensity` | 1 | Вклад света неба (IBL) |
| `directionalIntensity` | 1 | Вклад солнца и других направленных источников |
| `localLightIntensity` | 1 | Вклад точечных и прожекторов |
| `emission` | (0, 0, 0) | Свечение высотного тумана, масштабируется его плотностью |
| `distance` | 0 | Дальность сетки фроксов, м (0 = `r.VolumetricFog.Distance`) |

**Как получить выразительные лучи.** Лучи — это тени внутри освещённого тумана. Чтобы они читались:

- туман должен быть достаточно плотным в зоне лучей: `fogDensity` 0.03–0.1, малый `fogHeightFalloff`;
- камера смотрит **в сторону** источника, а анизотропия положительная (0.6–0.8) — рассеяние вперёд;
- небо и ambient не должны «заливать» тень: уменьшите `ambientIntensity` (компонента или окружения);
- нужны тени: `r.VolumetricFog.SunShadows` для солнца, `r.VolumetricFog.LocalLightShadows` и `castShadows` у ламп.

```cpp
// Прожектор в однородном ночном тумане: виден конус.
EnvironmentComponent& env = fogEnvironment(0.06f, 0.0f); // однородный туман: falloff 0
env.skyIntensity = 0.002f;                               // ночь: видно только конус
env.ambientIntensity = 0.002f;

const Image img = render(cam, 128, 128, 8); // туман копит историю: несколько кадров
ranPass("Volumetrics.FogScatter");          // true
```

Полный пример: `samples/guide_examples/22-volumetrics/volumetrics_gpu.cpp` (`GuideVolumetricsGpu.SpotLightConeInHeightFog`). Тест проверяет, что конус ярче тумана рядом с ним. `GuideVolumetricsGpu.LocalFogVolume` проверяет, что объём тумана рассеивает свет лампы.

## Шаг 4. Как устроен кадр тумана

| Проход (точка внедрения) | Что делает |
| --- | --- |
| `Volumetrics.FogInject` (Lighting) | Плотность во фрокселях: высотный туман + объёмы (спад, шум) → среда, свечение, `g` |
| `Volumetrics.FogTileDepth` | Ближайшая глубина на колонку фроксов — защита от протечки света |
| `Volumetrics.FogScatter` | Свет во фрокселе: солнце (CSM), другие направленные, кластерные точечные и прожекторы (тени из атласа и кубов), IBL неба, тени облаков; смешивание с историей |
| `Volumetrics.FogIntegrate` | Интеграция спереди назад (с сохранением энергии) → `VolumetricFog` |
| `Volumetrics.Composite` (AfterOpaque) | Наложение на `SceneColorHDR`: фроксели → аналитический туман за сеткой / небо → облака |

**Сетка.** По умолчанию 160×90×64 фроксела. По экрану сетка равномерная, по глубине экспоненциальная до `r.VolumetricFog.Distance` (128 м): `depth(s) = (2^(s·log2(1 + far·k)) − 1) / k`, где `k = r.VolumetricFog.DepthDistributionScale` (32). Чем больше `k`, тем больше срезов у камеры. C++-зеркало сетки — `volumetrics::FroxelGrid`:

```cpp
volumetrics::FroxelGrid g;            // по умолчанию 160 × 90 × 64, 128 м, scale 32 (уровень High)
g.froxelCount();                      // 921 600
g.sliceToDepth(1.0f / 64.0f);         // первый срез тоньше полуметра
g.depthToSlice(40.0f);                // в каком срезе (0..1) точка на 40 м
volumetrics::froxelGridFromCVars(&snap).farDistance; // 96: с учётом VolumetricFogComponent::distance
```

Полный пример: `samples/guide_examples/22-volumetrics/volumetrics_cpu.cpp` (`GuideVolumetrics.FroxelGrid`).

**Временное сглаживание.** Точка выборки внутри фроксела каждый кадр сдвигается по Halton(2, 3, 5), а результат смешивается с репроецированной историей (`r.VolumetricFog.HistoryWeight`). История сбрасывается за пределами прошлого фрустума, при резкой смене камеры, изменении размера и раскладки сетки. Поэтому туману нужно несколько кадров, чтобы «осесть»: в примерах рендерится 8 кадров.

**За пределами сетки.** От конца сетки до поверхности туман считается аналитически. Небо затуманивается до `r.VolumetricFog.SkyDistance` (20 км) — это дымка у горизонта. Пока фроксельный туман активен, собственный туман прямого прохода и воздушная перспектива неба мира (`r.Sky.AerialPerspective`) отключаются, чтобы не учитывать туман дважды.

**Прозрачные объекты** (стекло, вода, частицы) берут туман из той же 3D-текстуры через `render/volumetrics/fog_sample.glsl` ([глава 23](23-transparency-water-particles.md)).

## Шаг 5. Облака

`CloudLayerComponent` добавляет слой облаков. Рендерится первый активный слой.

```cpp
// Слой облаков.
auto& clouds = world.create("Clouds").add<CloudLayerComponent>();
clouds.altitude = 1500.0f;
clouds.thickness = 1800.0f;
clouds.coverage = 0.6f;     // 0 — ясно, 1 — сплошная облачность
clouds.cloudType = 0.5f;    // 0 слоистые, 0.5 кучевые, 1 кучево-дождевые
clouds.windSpeed = 12.0f;
```

Слой — сферическая оболочка над планетой радиусом 6360 км. Плотность складывается из покрытия по карте погоды (2D), высотного профиля по типу облака и базовой формы из шума Перлина–Уорли (128³), края которой размывает детальный шум Уорли (32³). Шумы генерируются компьютом один раз. Освещение: марш к солнцу (`r.VolumetricClouds.LightSteps`), три октавы многократного рассеяния, эффект «powder», двухлепестковая фазовая функция и окружающий свет неба по высоте. Солнце — это солнце сцены ([глава 20](20-lighting-shadows.md)). При времени суток из модуля `world` его двигает мост мира, и облака краснеют на закате сами.

| Поле `CloudLayerComponent` | По умолчанию | Смысл |
| --- | --- | --- |
| `altitude` / `thickness` | 1500 / 1800 м | Нижняя граница слоя над y = 0 и толщина |
| `coverage` | 0.45 | 0 — ясное небо, 1 — пасмурно |
| `cloudType` | 0.5 | 0 — слоистые (плоские), 0.5 — кучевые, 1 — кучево-дождевые (башни) |
| `density` | 1 | Множитель экстинкции (1 ≈ 0.02/м в ядрах) |
| `albedo` | (1, 1, 1) | Альбедо |
| `windDirection` / `windSpeed` | (1, 0) / 8 м/с | Ветер облаков в XZ; глобальный ветер мира добавляется |
| `weatherScale` / `shapeScale` / `detailScale` | 24 000 / 4500 / 600 м | Размеры тайлов карты погоды, базовой формы и детали |
| `detailStrength` | 0.35 | Насколько детальный шум «размывает» края |
| `ambientIntensity` / `sunIntensity` | 1 / 1 | Вклад неба и солнца |
| `forwardScattering` | 0.75 | `g` лепестка «серебряной каймы» |
| `shadowStrength` | 0.8 | Тени облаков **на объёмном тумане** (0 = выкл.) |
| `weatherOffset` | (0, 0) | Сдвиг карты погоды — разное небо на разных уровнях |

**Шахматка.** Облака трассируются в разрешении `рендер / r.VolumetricClouds.Downsample`. Каждый кадр обновляется только 1 из `Checkerboard` пикселей (16 → 1/16), остальные репроецируются на расстоянии облака, а при раскрытии сцены отбрасываются по глубине. Поэтому после резкого поворота камеры облака «дорисовываются» несколько кадров.

```cpp
const Image cloudy = render(cam, 128, 96, 16); // шахматка 1/16: полная картинка за 16 кадров
ranPass("Volumetrics.CloudTrace");             // true
```

Полный пример: `samples/guide_examples/22-volumetrics/volumetrics_gpu.cpp` (`GuideVolumetricsGpu.CloudLayer`).

## Уровни качества

Группа `Volumetrics` (по умолчанию все значения = High):

| CVar | По умолчанию | Low | Medium | High | Ultra |
| --- | --- | --- | --- | --- | --- |
| `r.VolumetricFog.GridSizeX` / `Y` / `Z` | 160 / 90 / 64 | 96·54·32 | 128·72·48 | 160·90·64 | 240·135·128 |
| `r.VolumetricFog.Distance` | 128 м | 64 | 96 | 128 | 192 |
| `r.VolumetricFog.HistoryWeight` | 0.9 | 0.85 | 0.9 | 0.9 | 0.95 |
| `r.VolumetricFog.LocalLightShadows` | `true` | выкл. | вкл. | вкл. | вкл. |
| `r.VolumetricFog.CloudShadows` | `true` | выкл. | выкл. | вкл. | вкл. |
| `r.VolumetricClouds` | `true` | выкл. | вкл. | вкл. | вкл. |
| `r.VolumetricClouds.Downsample` | 2 | 4 | 4 | 2 | 2 |
| `r.VolumetricClouds.Checkerboard` | 16 | 16 | 16 | 16 | 4 |
| `r.VolumetricClouds.Steps` | 64 | 32 | 48 | 64 | 96 |
| `r.VolumetricClouds.LightSteps` | 6 | 4 | 5 | 6 | 8 |
| `r.VolumetricClouds.MaxDistance` | 40 000 м | 25 000 | 30 000 | 40 000 | 50 000 |

Без групп:

| CVar | По умолчанию | Смысл |
| --- | --- | --- |
| `r.VolumetricFog` | `true` | Фроксельный туман (облака управляются отдельно) |
| `r.VolumetricFog.DepthDistributionScale` | 32 | Распределение срезов по глубине |
| `r.VolumetricFog.TemporalReprojection` | `true` | Дрожание выборок + история |
| `r.VolumetricFog.Anisotropy` | 0.6 | `g` высотного тумана (если нет `VolumetricFogComponent`) |
| `r.VolumetricFog.SunShadows` | `true` | Тени солнца в тумане — лучи |
| `r.VolumetricFog.MaxVolumes` | 64 | Объёмов тумана на вид |
| `r.VolumetricFog.SkyDistance` | 20 000 м | До какой дистанции затуманивается небо |
| `r.VolumetricClouds.Temporal` | `true` | Временная реконструкция облаков (выкл. — трассировать каждый пиксель) |

В группу `Volumetrics` входит и `r.Sky.AerialPerspective` неба мира (выкл. на Low, [глава 28](28-world-rendering.md)). При автоопределении качества группа ставится на ступень ниже остальных, если счёт GPU меньше 250 ([глава 26](26-quality-settings.md)).

```cpp
scalability::setGroup(Scalability::Volumetrics, QualityLevel::Low);
volumetrics::FroxelGrid low = volumetrics::froxelGridFromCVars(); // 96 × 54 × 32, 64 м
CVarRegistry::instance().find("r.VolumetricClouds")->toString();  // "false": Low без облаков
```

Полный пример: `samples/guide_examples/22-volumetrics/volumetrics_cpu.cpp` (`GuideVolumetrics.QualityLevels`) и на GPU — `volumetrics_gpu.cpp` (`GuideVolumetricsGpu.QualityLevelsAndToggles`): на каждом уровне есть `Volumetrics.FogIntegrate`, а `Volumetrics.CloudTrace` есть везде, кроме Low.

## Производительность

Замер `VolumetricsTest.PerfReport1080p` (Apple M4 Pro, 1080p). Сцена: высотный туман и 8 объёмов, 64 точечных источника (4 с тенями), солнце с CSM, слой облаков. GPU, мс:

| Уровень | Inject | Scatter | Integrate | Cloud trace | Cloud reconstruct | Composite | Всего |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Low | 0.03 | 0.06 | 0.02 | — | — | 0.01 | 0.17 |
| Medium | 0.05 | 0.14 | 0.03 | 0.30 | 0.02 | 0.06 | 0.72 |
| High | 0.09 | 0.28 | 0.06 | 0.41 | 0.07 | 0.06 | 1.14 |
| Ultra | 0.38 | 1.08 | 0.28 | 1.89 | 0.07 | 0.06 | 3.80 |

Что видно из таблицы:

- Стоимость тумана пропорциональна числу фроксов, а не разрешению экрана. Ultra даёт в 4.5 раза больше фроксов, чем High, и Scatter дорожает почти в 4 раза.
- Облака — самая дорогая часть. На Ultra их удорожают шахматка 1/4 вместо 1/16 и 96 шагов марша.
- Главные рычаги при нехватке бюджета: `GridSizeZ` и `Distance` для тумана, `Checkerboard`, `Downsample` и `Steps` для облаков.

## Типичные ошибки и подводные камни

- **Тумана нет.** Нужен `fogEnabled = true` в `EnvironmentComponent` или хотя бы один `FogVolumeComponent` (см. `volumetrics::fogActive`). Проверьте также `r.VolumetricFog` и `r.Feature.Volumetrics`.
- **Туман «серый и плоский», лучей нет.** Слишком яркое небо или ambient «заливает» тени, слабая анизотропия, или камера смотрит по направлению света, а не против него. Уменьшите `ambientIntensity`, поднимите `anisotropy`.
- **Полосы и «ступени» в тумане у стен.** Чаще всего это мало срезов по глубине. Поднимите `GridSizeZ` или уменьшите `Distance`. Протечку света через стены сглаживает проход `FogTileDepth`.
- **Туман «плывёт» и отстаёт при быстром движении.** Это история (`HistoryWeight`). Для катсцен с резкими движениями её можно уменьшить. Резкая смена камеры (camera cut) сбрасывает историю сама.
- **Туман обрывается на границе сетки.** За `r.VolumetricFog.Distance` туман аналитический, без теней и объёмов. Объёмы тумана дальше сетки не видны: ставьте их ближе или увеличьте `distance`.
- **Флаг `volumetric` у источника ничего не меняет.** `LightComponent::volumetric` и `volumetricIntensity` пока не читаются рендером. Регулируйте вклад всех локальных источников через `VolumetricFogComponent::localLightIntensity`.
- **Тени ламп в тумане «жёсткие».** Для локальных источников в тумане используется одно сравнение глубины без PCF.
- **Облака не отбрасывают тени на землю.** Тени облаков пока есть только на объёмном тумане (`shadowStrength`, `r.VolumetricFog.CloudShadows`).
- **Облака пропали на Low.** `r.VolumetricClouds` на Low выключен. Если облака важны для геймплея (погода), включите их явно.
- **Облака «шумят» после поворота.** Это шахматка 1/16. Для скриншотов поставьте `r.VolumetricClouds.Checkerboard 1`.
- **Сотни мелких объёмов тумана.** Каждый проверяется в каждом фрокселе, а сверх `MaxVolumes` отбрасываются дальние. Объединяйте объёмы.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`components/volumetrics.hpp`](../../engine/render/include/oxwald/render/components/volumetrics.hpp) | `FogVolumeComponent`, `FogVolumeShape`, `VolumetricFogComponent`, `CloudLayerComponent` |
| [`features/volumetrics/volumetrics.hpp`](../../engine/render/include/oxwald/render/features/volumetrics/volumetrics.hpp) | `volumetrics::FroxelGrid`, `froxelGridFromCVars`, `VolumetricsSnapshot`, `setWorldWind`, `fogActive`, `extractVolumetrics`, `registerVolumetricsTypes`, имена ресурсов (`kVolumetricFogVisibility`, `kVolumetricClouds`), `kFogOrder` |
| [`scene/components.hpp`](../../engine/scene/include/oxwald/scene/components.hpp) | `EnvironmentComponent` (поля `fog*`) |
| [`register_types.hpp`](../../engine/render/include/oxwald/render/register_types.hpp) | `registerRenderTypes` — регистрирует и компоненты объёмных эффектов |

Шейдерные включения: `render/volumetrics/fog_sample.glsl` (`oxEvaluateVolumetricFog`, `oxApplyVolumetricFog` — для прозрачных объектов), `fog_common.glsl` (`oxFroxelWorldPosition`). Хук для трассировки лучей (`VolumetricFogVisibility`: видимость солнца, локальных источников и неба во фрокселе) описан в [главе 24](24-ray-tracing.md). Заметки для разработчиков: [`docs/dev/modules/render_volumetrics.md`](../dev/modules/render_volumetrics.md).

## Что дальше

- [20. Освещение и тени](20-lighting-shadows.md) — солнце, каскады и тени ламп, которые рисуют лучи в тумане.
- [23. Прозрачность, вода и частицы](23-transparency-water-particles.md) — как прозрачные объекты получают туман.
- [24. Трассировка лучей](24-ray-tracing.md) — RT-видимость в тумане: тени от объектов за кадром.
- [28. Рендеринг мира](28-world-rendering.md) — небо, время суток и ветер, которые двигают солнце и облака.
- [16. Открытый мир](16-world.md) — CPU-часть погоды и ветра.
- [Оглавление](README.md).
