# 16. Открытый мир

> Модуль `world` (таргет `Oxwald::world`, пространство имён `ox::world`, зонтичный заголовок `<oxwald/world/world.hpp>`). Это CPU-часть открытого мира: данные, алгоритмы и GLSL-включения, повторяющие CPU-математику. Модуль зависит только от `core` и glm, ничего не знает про ECS и **не линкует физику**. Отрисовка ландшафта, растительности, неба и воды описана в главе о рендеринге (*скоро*). Компоненты сцены для этих систем описаны в главе о геймплейных компонентах ECS (*скоро*).

## Зачем

Для большого мира нужно много независимых систем. Модуль даёт их как набор библиотек, которые можно использовать по отдельности:

| Задача | Что есть |
| --- | --- |
| Ландшафт | Карта высот (heightfield): импорт PNG16/R16/EXR, процедурная генерация шумом, гидравлическая и термическая эрозия, кисти, дыры, splat map материалов |
| LOD ландшафта | CDLOD: квадродерево, выбор патчей по расстоянию и фрустуму, общий сеточный меш, морфинг без трещин |
| Растительность | Разброс (scattering) по Poisson-диску с правилами, бесшовный между чанками, ячейки для каллинга, LOD, коллайдеры деревьев |
| Небо и время суток | Положение Солнца (NOAA) и Луны, фазы Луны, звёзды, небо Preetham, освещённость, кривые атмосферы, события восхода и заката |
| Погода | Ветер с порывами, плавные переходы погодных пресетов, намокание и снежный покров |
| Вода | Волны Герстнера (Gerstner), одинаковые на CPU и GPU, и плавучесть (buoyancy) |
| Стриминг | Асинхронная подгрузка и выгрузка чанков вокруг игрока с приоритетами и бюджетами, бинарный формат чанка |
| Физика | Заголовочный мост `physics_bridge.hpp`: коллайдеры ландшафта и деревьев → `ox::physics::ShapeDesc` |

**Мировая конвенция:** Y вверх, **север = −Z, восток = +X**, азимут отсчитывается от севера по часовой стрелке.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `Heightfield` | Квадратная сетка высот. Хранит **нормализованные** значения: `мировая высота = heightOffset + h·heightScale` |
| `IRect` | «Грязный» прямоугольник в отсчётах `[x0, x1) × [z0, z1)`: что заливать в GPU и какие коллайдеры пересобирать |
| `SplatMap` | Веса до 8 слоёв материалов на тексель (u8, сумма 255) → две RGBA8-текстуры |
| `TerrainQuadtree` | CDLOD-дерево с min/max высотами узлов; `select` выдаёт список патчей для отрисовки |
| `VegetationScatterer` | Раскладывает экземпляры растительности по чанку по правилам слоёв |
| `TimeOfDay` / `SkyState` | Часы мира: дата, время, место → солнце, луна, свет, небо, атмосфера |
| `WindField` / `WeatherController` | Ветер в точке и времени; текущая погода и накопленные эффекты |
| `GerstnerWaves` | Набор волн; высота и нормаль воды в точке |
| `ChunkStreamer` / `ChunkData` | Подгрузка чанков вокруг наблюдателей; полезная нагрузка чанка и его сериализация |

## Шаг 1. Ландшафт: генерация и эрозия

```cpp
#include <oxwald/world/terrain_gen.hpp>
using namespace ox::world;

HeightfieldDesc d;
d.resolution = 257;            // отсчётов на сторону → 256 квадов
d.worldSize = 512.f;           // метров по X и Z → шаг 2 м
d.heightScale = 120.f;         // высота = heightOffset + normalized * heightScale
d.origin = {-256.f, -256.f};   // мировые XZ отсчёта (0, 0) — минимальный угол
d.format = HeightFormat::Float32;   // или UNorm16 (R16_UNORM на GPU, вдвое меньше памяти)
Heightfield hf(d);

TerrainNoiseSettings ns;
ns.fractal.basis = NoiseBasis::Simplex;
ns.fractal.type = FractalType::Ridged;   // хребты; Fbm — холмы, Billow — «пухлые» формы
ns.fractal.seed = 7;
ns.fractal.frequency = 1.f / 300.f;      // циклов на метр (первая октава)
ns.fractal.octaves = 5;
ns.fractal.warpStrength = 40.f;          // domain warp — «закрученный» рельеф
generateNoise(hf, ns);                   // шум в мировых координатах: соседние тайлы стыкуются

erodeHydraulic(hf, {.droplets = 20000, .seed = 1});           // капли размывают склоны, несут осадок
erodeThermal(hf, {.iterations = 20, .talusAngleDeg = 38.f}); // осыпание круче угла естественного откоса
terrace(hf, /*steps*/ 12, /*sharpness*/ 0.6f, /*blend*/ 0.3f); // по желанию: террасы

const float h = hf.sampleHeight({10.f, -20.f});   // билинейно, в метрах
const glm::vec3 n = hf.sampleNormal({10.f, -20.f});
const float slope = hf.sampleSlope({10.f, -20.f}); // радианы
const HeightStats stats = computeStats(hf);        // min/max, сумма, макс. уклон, «шероховатость»
```

Полный пример: `samples/guide_examples/16-world/terrain.cpp`. Тест проверяет, что эрозия уменьшает максимальный уклон и почти сохраняет массу.

Важные настройки:

| Структура | Поле | По умолчанию | Смысл |
| --- | --- | --- | --- |
| `FractalSettings` | `frequency` | 1/512 | Частота первой октавы, циклов на метр |
| | `octaves`, `lacunarity`, `gain` | 6, 2, 0.5 | Октавы и их масштабирование |
| | `warpStrength`, `warpFrequency` | 0, 1/256 | Искажение домена (в метрах) |
| `TerrainNoiseSettings` | `exponent` | 1 | `h = h^exponent`: больше 1 — плоские долины и острые пики |
| | `normalizeRange` | `false` | Растянуть в [0, 1]. **Ломает стыковку тайлов** |
| `HydraulicErosionSettings` | `droplets`, `seed` | 50000, 1 | Число капель (≈ 0.3 с на 100k на 129²); детерминировано по seed |
| | `erosionRadius` | 3 | Радиус размытия в отсчётах |
| | `loseSedimentAtBorder` | `false` | `true` — открытая граница, осадок уходит с карты |
| `ThermalErosionSettings` | `iterations`, `talusAngleDeg`, `rate` | 50, 35°, 0.5 | Осыпание |

Эрозия нормализует высоты по рельефу, поэтому её параметры не зависят от `heightScale`. Отсчёты хранятся нормализованными, и `hf.setScale(scale, offset)` меняет масштаб высот без пересчёта данных.

**Импорт и экспорт:**

```cpp
std::string error;
std::optional<Heightfield> imported =
    Heightfield::load("terrain.png", {.worldSize = 512.f, .heightScale = 120.f, .origin = {-256.f, -256.f}}, &error);
// .png (16/8 бит), .r16/.raw (размер выводится), .exr (значения в метрах). flipZ — если строка 0 = +Z.
hf.savePng16("out.png");   // или saveRaw16
```

## Шаг 2. Кисти, дыры и splat map

Кисти нужны редактору и игровым механикам: воронки от взрывов, копание. Каждый мазок возвращает точный прямоугольник изменённых отсчётов:

```cpp
#include <oxwald/world/terrain_brush.hpp>

BrushSettings raise{.op = BrushOp::Raise, .radius = 15.f, .strength = 4.f, .falloff = BrushFalloff::Smooth};
const IRect dirty = applyBrush(hf, cursorXZ, raise, dt);   // strength × dt: мазок не зависит от FPS

// Что делать с dirty:
std::vector<uint16_t> upload = hf.extractR16(dirty);        // частичная заливка GPU-текстуры R16_UNORM
quadtree.updateBounds(hf, dirty);                           // min/max узлов CDLOD (шаг 3)
for (glm::ivec2 t : physicsTilesOverlapping(dirty, 64, hf.resolution()))
    rebuildCollider(buildPhysicsTile(hf, t.x, t.y, 64));    // коллайдеры (шаг 8)

applyBrush(hf, {60.f, 60.f}, {.op = BrushOp::Flatten, .radius = 10.f, .strength = 1.f, .hardness = 0.5f,
                              .targetHeight = 30.f});      // площадка под здание
applyBrush(hf, {-60.f, 0.f}, {.op = BrushOp::SetHole, .radius = 3.f});   // вход в пещеру
```

| `BrushOp` | `strength` означает |
| --- | --- |
| `Raise`, `Lower`, `Noise` | Метры за применение (× dt) |
| `Smooth`, `Flatten` | Коэффициент смешивания 0..1 (× dt); `Flatten` тянет к `targetHeight` (в мировых метрах) |
| `SetHole`, `ClearHole` | Дыры по отсчётам: квад считается дырой, если дырой помечен любой его угол |

Спад кисти (`BrushFalloff`): `Constant`, `Linear`, `Smooth`, `Spherical`. Параметр `hardness` задаёт долю радиуса с полной силой.

**Splat map** хранит веса материалов. Она покрывает ту же площадь, что и карта высот, но может иметь другое разрешение.

```cpp
#include <oxwald/world/splat_map.hpp>

SplatMap splat(129, /*layers*/ 4, hf.desc().origin, hf.desc().worldSize);  // 0 трава, 1 скалы, 2 снег, 3 дорога
splat.fill(0);
const SplatRule rules[] = {
    {.layer = 1, .minSlopeDeg = 35.f, .maxSlopeDeg = 90.f},                     // скалы на крутых склонах
    {.layer = 2, .minHeight = 90.f, .heightBlend = 10.f, .maxSlopeDeg = 30.f}, // снег на вершинах
};
autoPaint(splat, hf, rules);   // правила накладываются по порядку (поздние «сверху»), сумма = 255
paintSplat(splat, cursorXZ, 3, {.radius = 8.f, .strength = 1.f, .falloff = BrushFalloff::Constant});

splat.dominantLayer(xz);                                // для звука шагов, частиц, физматериала
splat.sampleLayer(xz, 2);                               // сколько «снега» в точке, 0..1
std::vector<uint8_t> tex0 = splat.packRgba8(0, splat.fullRect());   // слои 0–3; индекс 1 — слои 4–7
```

`SplatRule` также умеет `noiseAmount`/`noiseFrequency`, чтобы разбить слишком ровные переходы.

Полный пример: `samples/guide_examples/16-world/terrain.cpp` (`BrushesReturnDirtyRects`, `AutoPaintSplatMap`, `ExportAndImport16BitPng`).

## Шаг 3. LOD ландшафта (CDLOD)

CDLOD (Strugar, 2010) выбран вместо geometry clipmaps по трём причинам. Узлы дерева хранят min/max высоты, что даёт точный каллинг. Переходы между LOD непрерывны: вершины морфятся, сшивать T-стыки не нужно. Один статический сеточный меш инстансится на все патчи.

```cpp
#include <oxwald/world/terrain_lod.hpp>

TerrainLodSettings ls;
ls.leafNodeSize = 16;      // квадов на сторону узла LOD 0 (= размер сетки-меша)
ls.lodCount = 4;           // LOD 0 — самый детальный; размер узла удваивается на уровень
ls.viewDistance = 1000.f;  // дальность самого грубого LOD
const TerrainQuadtree tree(hf, ls);                         // предупредит, если дальности не дают «без трещин»
const TerrainGridMesh grid = generateTerrainGrid(ls.leafNodeSize, /*skirts*/ true);

TerrainSelection sel;
tree.select({.cameraPosition = camPos, .frustum = Frustum::fromViewProjection(viewProj)}, sel);
std::vector<TerrainPatchGpu> instances;                     // 32 байта на патч — инстанс-буфер
for (const TerrainPatch& p : sel.patches) instances.push_back(toGpu(p));
tree.debugDraw(sel, debugLine);
```

| `TerrainLodSettings` | По умолчанию | Смысл |
| --- | --- | --- |
| `leafNodeSize` | 32 | Квадов на сторону листа (чётное) |
| `lodCount` | 6 | Число уровней |
| `viewDistance` | 4000 м | Дальность последнего уровня |
| `detailBalance` | 2 | Отношение дальностей соседних LOD (не меньше 2) |
| `morphStartRatio` | 0.66 | Морфинг идёт в последней трети каждой полосы |

Условие «без трещин» выполняется примерно при `viewDistance ≥ 1.2 · leafWorldSize · (2^lodCount − 1)`, где `leafWorldSize = leafNodeSize × шаг`. Конструктор дерева предупреждает в логе, если условие нарушено. `Frustum::fromViewProjection` работает и с reversed-Z, и с бесконечной дальней плоскостью.

Полный пример: `samples/guide_examples/16-world/lod_vegetation.cpp` (`GuideWorldLod`).

## Шаг 4. Растительность

Каждый слой (деревья, кусты, трава) раскладывается по **тайлящемуся Poisson-диску** со своим смещением. Минимальная дистанция поэтому соблюдается и через границы чанков, а результат не зависит от нарезки мира на чанки. Правила отбраковывают точки: дыры, полоса высот, уклон, вес splat-слоя, зоны исключения. Карты плотности и `customDensity` прореживают их детерминированно.

```cpp
#include <oxwald/world/vegetation.hpp>

VegetationLayer pine;
pine.name = "pine";
pine.kind = VegetationKind::Tree;
pine.prototype = 0;          // индекс меша/материала у рендера
pine.minDistance = 6.f;      // деревья не ближе 6 м друг к другу
pine.maxSlopeDeg = 25.f;
pine.minScale = 0.8f; pine.maxScale = 1.3f;
pine.boundingRadius = 6.f;
pine.collider = true;        // деревьям — капсулы для физики
pine.colliderRadius = 0.4f; pine.colliderHalfHeight = 3.f;
pine.seed = 11;

VegetationLayer grass;
grass.name = "grass";
grass.prototype = 1;
grass.minDistance = 1.5f;
grass.maxSlopeDeg = 40.f;
grass.alignToNormal = 1.f;          // трава ложится по нормали склона
grass.lod.cullDistance = 80.f;
grass.lod.impostorDistance = 0.f;   // без импостеров
grass.seed = 12;

const VegetationScatterer scatter({pine, grass});
const ExclusionZone village{.shape = ExclusionZone::Shape::Circle, .center = {0.f, 0.f},
                            .halfExtents = {30.f, 0.f}, .layerMask = 1u << 0};   // поляна без деревьев
ScatterContext ctx;
ctx.heightfield = &hf;              // обязательно
ctx.splat = &splat;                 // для splatLayer
ctx.exclusions = std::span(&village, 1);

const VegetationChunk chunk = scatter.scatter({-64.f, -64.f}, 128.f, ctx);   // чанк 128 × 128 м
chunk.instances;   // позиция, поворот, масштаб, оттенок, random, слой, прототип
chunk.colliders;   // позы и формы стволов
chunk.cells;       // группы экземпляров с AABB для каллинга

std::vector<VisibleVegetationCell> visible;
cullVegetationCells(chunk, scatter.layers(), frustum, camPos, visible);   // фрустум + дистанция
VegetationInstanceGpu gpu = toGpu(chunk.instances[0], scatter.layers()[chunk.instances[0].layer]); // 64 байта
```

Основные поля `VegetationLayer`:

| Поле | Смысл |
| --- | --- |
| `minDistance`, `density` | Радиус Poisson-диска и вероятность принятия точки |
| `minHeight`/`maxHeight`, `minSlopeDeg`/`maxSlopeDeg` | Полосы высоты (м) и уклона |
| `splatLayer`, `minSplatWeight` | Требуемый слой материала и порог веса |
| `densityMapIndex` | Индекс карты плотности в `ScatterContext::densityMaps` |
| `minScale`/`maxScale`, `randomYaw`, `alignToNormal`, `sinkDepth`, `tintA`/`tintB` | Вариативность экземпляров |
| `lod` | `lodDistances[2]`, `impostorDistance`, `cullDistance`, `fadeRange` |
| `collider`, `colliderShape`, `colliderRadius`, `colliderHalfHeight` | Коллайдер (Capsule/Cylinder/Box) |

`selectVegetationLod(settings, distance)` возвращает LOD меша (0..2), `kImpostor` или `kCulled` и коэффициент кросс-фейда `fade` для дизеринга.

Полный пример: `samples/guide_examples/16-world/lod_vegetation.cpp` (`GuideWorldVegetation`).

## Шаг 5. Небо, Солнце, Луна и время суток

Астрономия считается по реальным алгоритмам. Положение Солнца — NOAA/Meeus (точность ~0.01°), Луны — теория Шлютера с параллаксом (~0.3°). Поэтому восход в Амстердаме 21 июня будет ровно в 05:18.

```cpp
#include <oxwald/world/time_of_day.hpp>

const SolarPosition sun = computeSunPosition({.latitudeDeg = 48.85, .longitudeDeg = 2.35}, {2024, 6, 21, 10.5 /*UTC*/});
const glm::vec3 toSun = horizontalToWorld(sun.elevationDeg, sun.azimuthDeg);
const CelestialLight light = sunLight(sun.elevationDeg, /*turbidity*/ 2.5f);   // люксы + цвет после атмосферы
const PreethamSky sky = PreethamSky::compute(toSun, 2.5f);                     // sky.toGpu() → UBO 128 байт
const MoonPhase phase = computeMoonPhase({2024, 4, 23, 23.8});                 // illuminatedFraction ≈ 1
```

`TimeOfDay` — это «часы мира». Он продвигает время, считает всё небо и шлёт события:

```cpp
TimeOfDaySettings s;
s.location = {52.37, 4.90};   // Амстердам
s.year = 2024; s.month = 6; s.day = 21;
s.localHours = 0.0;
s.utcOffsetHours = 2.0;       // местное = UTC + 2
s.timeScale = 60.0;           // игровых секунд за реальную: сутки за 24 минуты
TimeOfDay tod(s);

tod.addListener([](TimeOfDayEvent e, const SkyState& st) {
    if (e == TimeOfDayEvent::Sunset) { /* зажечь фонари */ }
});
tod.curves().fogDensity = Curve({{-10.f, 0.02f}, {10.f, 0.004f}}, CurveInterp::Smooth);  // по высоте солнца

tod.update(realDt);                     // реальные секунды × timeScale; большие шаги дробятся
const SkyState& st = tod.state();
st.mainLightDirection; st.mainLightColor; st.mainLightIlluminance;   // днём солнце, ночью луна
st.atmosphere.fogDensity; st.atmosphere.ambientColor; st.atmosphere.exposureCompensation;
st.starsRotation;                       // поворот кубмапы звёзд
st.isDay;

SkyState preview = TimeOfDay::evaluate(s, tod.curves());   // чистая функция без событий — превью в редакторе
```

| Член | Смысл |
| --- | --- |
| `TimeOfDaySettings::timeScale`, `paused`, `turbidity` | Скорость времени, пауза, мутность воздуха (2 — ясно, 10 — дымка) |
| `setLocalTime`, `setDate`, `setLocation`, `advanceGameTime` | Прыжок без событий и продвижение на игровые секунды (с событиями) |
| `TimeOfDayEvent` | `Sunrise`/`Sunset` (центр солнца на −0.833°), `Noon`, `Midnight` |
| `AtmosphereCurves` | `fogDensity`, `ambientIntensity`, `exposureCompensation`, `starsIntensity` (`Curve`), `ambientColor`, `fogColor` (`Gradient`) |
| `AtmosphereCurves::driver` | `SunElevation` (по умолчанию; работает на любой широте и в любой сезон) или `LocalHour` (тогда у кривых `wrap = true`) |

Полный пример: `samples/guide_examples/16-world/sky_weather_water.cpp` (`GuideWorldSky`).

## Шаг 6. Ветер и погода

```cpp
#include <oxwald/world/weather.hpp>

WindSettings base;
base.direction = {1.f, 0.f};   // куда дует (XZ): на восток
base.speed = 6.f;              // м/с
base.gustStrength = 0.5f;      // порывы — доля скорости; фронты порывов бегут по ветру
const WindField wind(base);
glm::vec3 v = wind.sample(position, time);    // горизонтальный ветер: ткань, частицы, паруса, звук
WindGpu gpu = wind.toGpu(time);               // те же формулы в шейдере world/wind.glsl

WeatherController weather(WeatherPreset::clear());
weather.setTarget(WeatherPreset::rainy(), /*seconds*/ 10.f);   // плавный переход (smoothstep)
weather.update(dt, /*temperature °C*/ 15.f);
weather.state().current.rain;       // интенсивность дождя 0..1 → частицы, звук, мокрые материалы
weather.state().wetness;            // накопленная влажность поверхностей
weather.state().snowCover;          // снежный покров (копится ниже 0 °C, тает выше)
WindSettings now = weather.wind(base);   // скорость/порывы из текущей погоды поверх base
```

Готовые пресеты: `clear`, `overcast`, `rainy`, `storm`, `snowy`. Свой пресет — это просто `WeatherPreset{cloudCover, rain, snow, fogDensityBoost, windSpeed, gustStrength}`. Скорости намокания, высыхания, накопления снега и таяния задаются в `WeatherSettings`.

## Шаг 7. Вода и плавучесть

Вода — сумма волн Герстнера. В шейдере (`world/gerstner.glsl`) и на CPU используется одна и та же математика, поэтому плавающие объекты качаются ровно на тех волнах, которые видит игрок.

```cpp
#include <oxwald/world/water.hpp>

GerstnerWaves waves = GerstnerWaves::fromWind({1.f, 0.f}, /*wind m/s*/ 7.f, /*count*/ 8, /*seed*/ 3);
waves.baseHeight = 2.f;                   // уровень моря
float h = waves.heightAt({x, z}, t);      // высота воды в мировой точке
glm::vec3 n = waves.normalAt({x, z}, t);
GerstnerParamsGpu gpu = waves.toGpu(t);   // UBO 528 байт для шейдера
```

Волны можно задать и вручную: `GerstnerWave{direction, wavelength, amplitude, steepness, phase, speedScale}`, до 16 штук. Если сумма `steepness` больше 1, гребни начинают закручиваться в петли.

**Плавучесть.** Тело приближается набором точек-кубиков. Для каждой точки считается погружённая доля, сила Архимеда и сопротивление относительно воды. Результат — сила и момент, которые надо приложить к телу физики:

```cpp
const BuoyancySettings hull = BuoyancySettings::fromBox(halfExtents, /*subdivisions*/ 4);   // 4³ точек
const BuoyancyResult r = computeBuoyancy(hull, physicsWorld.getCenterOfMassPosition(body),
                                         physicsWorld.getRotation(body), physicsWorld.getLinearVelocity(body),
                                         physicsWorld.getAngularVelocity(body),
                                         [&](glm::vec2 xz) { return waves.heightAt(xz, t); });
physicsWorld.addForce(body, r.force);     // силы копятся до следующего step()
physicsWorld.addTorque(body, r.torque);
physicsWorld.step(dt);
```

`BuoyancySettings`: `fluidDensity` (1000 — пресная вода, 1025 — морская), `gravity`, `linearDrag`, `angularDrag`. Последний аргумент `computeBuoyancy` — скорость течения: оно несёт тело.

Полные примеры: `samples/guide_examples/16-world/sky_weather_water.cpp` (`GuideWorldWater`) и `physics_bridge.cpp` (`CrateFloatsOnWaves`: ящик плотностью 400 кг/м³ 15 секунд держится на волнах).

## Шаг 8. Мост в физику

Модуль `world` физику не линкует. Он выдаёт простые данные: `PhysicsHeightfieldTile` (высоты тайла в метрах + смещение и масштаб) и `VegetationCollider` (поза + форма). Заголовок `physics_bridge.hpp` переводит их в `ox::physics::ShapeDesc`. Подключайте его только там, где слинкован `Oxwald::physics`.

```cpp
#include <oxwald/physics/physics.hpp>
#include <oxwald/world/physics_bridge.hpp>

// Ландшафт: тайлы по 64 квада (65 × 65 отсчётов, края общие с соседями).
for (const PhysicsHeightfieldTile& tile : buildPhysicsTiles(hf, 64)) {
    physics::BodyDesc ground;
    ground.shape = physics::createShape(toShapeDesc(tile));
    ground.motionType = physics::MotionType::Static;
    physicsWorld.createBody(ground);
}

// Деревья: поза тела из VegetationCollider, форма — toShapeDesc.
for (const VegetationCollider& c : chunk.colliders) {
    physics::BodyDesc trunk;
    trunk.shape = physics::createShape(toShapeDesc(c));
    trunk.position = c.position;
    trunk.rotation = c.rotation;
    trunk.motionType = physics::MotionType::Static;
    physicsWorld.createBody(trunk);
}
```

Дыры в ландшафте помечаются в тайле значением `kPhysicsHeightHole` (это `ox::physics::kHeightFieldHole`), так что в пещеру можно провалиться. После правки кистью пересобирайте только тайлы из `physicsTilesOverlapping(dirty, 64, hf.resolution())`.

Полный пример: `samples/guide_examples/16-world/physics_bridge.cpp` (таргет `ox_guide_world_physics`). Луч из Jolt попадает в ту же высоту, что и `sampleHeight`, с точностью до сантиметров.

## Шаг 9. Стриминг чанков

`ChunkStreamer` делит плоскость XZ на квадратные чанки. Он грузит те, что ближе `loadRadius` к любому наблюдателю (viewer), и выгружает те, что дальше `unloadRadius`. Загрузка идёт на рабочих потоках, а активация (`onLoaded`) — на главном потоке внутри `update()`, с бюджетом на кадр.

```cpp
#include <oxwald/world/chunk_data.hpp>
#include <oxwald/world/streaming.hpp>

ChunkCallbacks cb;
// Рабочий поток: чтение и распаковка. Никакого ECS/рендера здесь.
cb.load = [&](ChunkCoord c, const std::atomic<bool>& cancelled) -> std::unique_ptr<ChunkPayload> {
    if (cancelled) return nullptr;                    // чанк уже не нужен
    std::vector<uint8_t> bytes = readChunkFile(c);
    auto data = std::make_unique<ChunkData>();
    return deserializeChunk(bytes, *data) ? std::move(data) : nullptr;   // nullptr → повтор позже
};
// Главный поток: создать сущности, залить высоты в GPU, коллайдеры, растительность.
cb.onLoaded = [&](ChunkCoord c, ChunkPayload& p) { spawnChunk(static_cast<ChunkData&>(p)); };
cb.onUnload = [&](ChunkCoord c, ChunkPayload& p) { despawnChunk(c); };
// cb.save — по желанию, на рабочем потоке перед уничтожением (чанк остаётся Unloading).

ChunkStreamerSettings settings;
settings.chunkSize = 64.f;
settings.loadRadius = 100.f;     // грузим ближе 100 м…
settings.unloadRadius = 150.f;   // …выгружаем дальше 150 м (гистерезис)
ChunkStreamer streamer(settings, cb);   // исполнитель по умолчанию — ThreadPoolExecutor(2)

StreamingViewer player{.position = playerPos, .forward = playerForward};
streamer.update(std::span(&player, 1));        // раз в кадр
if (!streamer.isAreaReady(playerPos, 100.f)) { /* экран загрузки */ }
streamer.flush(std::span(&player, 1));         // дождаться всего (загрузка, тесты)
streamer.unloadAll();                          // перед уничтожением, если onUnload должен отработать
```

Порядок и бюджеты:

| `ChunkStreamerSettings` | По умолчанию | Смысл |
| --- | --- | --- |
| `chunkSize`, `loadRadius`, `unloadRadius` | 128, 512, 640 м | Сетка и гистерезис |
| `maxLoadRequestsPerUpdate`, `maxInFlightLoads` | 4, 8 | Сколько загрузок запускать за кадр и держать одновременно |
| `maxActivationsPerUpdate`, `maxUnloadsPerUpdate` | 4, 8 | Сколько `onLoaded`/`onUnload` вызывать за кадр (цена на главном потоке) |
| `viewDirectionWeight` | 0.5 | Бонус приоритета чанкам перед наблюдателем |
| `teleportDistance`, `teleportBudgetMultiplier` | 256 м, 4 | Прыжок наблюдателя отменяет устаревшие загрузки и на 30 кадров умножает бюджеты |
| `failedRetryUpdates` | 60 | Через сколько кадров повторить неудачную загрузку |

Состояния чанка: `Unloaded → Loading → Loaded → (Unloading, пока идёт save) → Unloaded`. Сначала грузятся ближайшие чанки. Загрузка, которая вышла из радиуса, отменяется: выставляется флаг `cancelled`, а опоздавший результат отбрасывается. Наблюдателей может быть несколько (`radiusScale < 1` для второстепенных: удалённые игроки, камера катсцены).

**Формат чанка.** `ChunkData` — готовая полезная нагрузка: тайл высот (с общим краем, например 65×65 на 64 квада), splat map, экземпляры растительности и ваш блоб `userData`. `serializeChunk`/`deserializeChunk` пишут бинарный формат `"OXCH"` из секций `HFLD`, `HOLE`, `SPLT`, `VEGI`, `USER`. У каждой секции свой CRC32, неизвестные секции пропускаются (совместимость вперёд), порча данных обнаруживается. Свою полезную нагрузку можно сделать, унаследовав её от `ChunkPayload`.

Полный пример: `samples/guide_examples/16-world/streaming.cpp`. Для детерминизма в нём используется `InlineExecutor`. Свой пул или job system ([глава 01](01-core.md)) подключается через `IChunkExecutor::submit`.

## Типичные ошибки и подводные камни

- **`normalizeRange = true` для тайлового мира.** Каждый тайл растягивается по-своему, и на стыках появляются ступени. Для бесшовности генерируйте все тайлы с одинаковыми настройками без нормализации.
- **`(resolution − 1)` не кратно `leafNodeSize`.** Узлы CDLOD «свисают» за край карты. Выбирайте разрешения вида `2^n + 1` (257, 1025, 2049).
- **Слишком короткий `viewDistance` при большом `lodCount`.** Появляются трещины между уровнями LOD. Смотрите предупреждение в логе при создании `TerrainQuadtree`.
- **Забыли `updateBounds` после кисти.** Каллинг CDLOD будет использовать старые min/max, и патчи начнут пропадать на краю экрана.
- **Эрозия в рантайме.** Гидравлическая эрозия однопоточная (≈ 0.3 с на 100k капель на 129²). Это инструмент для редактора или загрузки, а не для каждого кадра.
- **Растительность разной в разных чанках.** Результат зависит от `seed` слоя и `patternPeriod` скаттера. Меняйте их только вместе для всего мира.
- **Время суток в UTC.** `computeSunPosition` принимает **UTC**. `TimeOfDay` работает в местном времени и сам вычитает `utcOffsetHours`.
- **Небо Preetham ночью.** Модель неверна для солнца ниже горизонта (зажимается на 1°). Сумерки и ночь задаются кривыми атмосферы и светом луны.
- **Тяжёлая работа в `onLoaded`.** Он вызывается на главном потоке. Распаковку, декодирование и генерацию держите в `load`, а `maxActivationsPerUpdate` подберите по бюджету кадра.
- **Деструктор стримера ждёт задачи.** Исполнитель обязан выполнить каждую отправленную задачу, иначе `~ChunkStreamer` зависнет. `onUnload` при уничтожении не вызывается — сначала вызовите `unloadAll()`.
- **Огромные координаты.** Физика и рендер работают в одинарной точности. На мирах больше ~10 км нужен сдвиг начала координат (origin shifting).
- **Чанки только 2D.** Вертикальных слоёв (пещеры под пещерами) у стримера нет. Многоуровневый контент кладите в `userData` чанка.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`world.hpp`](../../engine/world/include/oxwald/world/world.hpp) | Зонтичный заголовок |
| [`common.hpp`](../../engine/world/include/oxwald/world/common.hpp) | `IRect`, `Aabb`, `Frustum`, `DebugLineFn` |
| [`noise.hpp`](../../engine/world/include/oxwald/world/noise.hpp) | `Rng` (PCG32), `Noise2D`, `FractalNoise`, `FractalSettings`, хэши |
| [`heightfield.hpp`](../../engine/world/include/oxwald/world/heightfield.hpp) | `Heightfield`, `HeightfieldDesc`, `HeightFormat`, импорт/экспорт |
| [`terrain_gen.hpp`](../../engine/world/include/oxwald/world/terrain_gen.hpp) | `generateNoise`, `addNoise`, `erodeHydraulic`, `erodeThermal`, `terrace`, `computeStats` |
| [`terrain_brush.hpp`](../../engine/world/include/oxwald/world/terrain_brush.hpp) | `BrushSettings`, `applyBrush`, `paintSplat`, `brushWeight` |
| [`splat_map.hpp`](../../engine/world/include/oxwald/world/splat_map.hpp) | `SplatMap`, `SplatRule`, `autoPaint` |
| [`terrain_lod.hpp`](../../engine/world/include/oxwald/world/terrain_lod.hpp) | `TerrainQuadtree`, `TerrainLodSettings`, `TerrainPatch(Gpu)`, `generateTerrainGrid`, `PhysicsHeightfieldTile`, `buildPhysicsTile(s)` |
| [`vegetation.hpp`](../../engine/world/include/oxwald/world/vegetation.hpp) | `PoissonDisk`, `VegetationLayer`, `VegetationScatterer`, `ScatterContext`, `VegetationChunk`, `cullVegetationCells`, `selectVegetationLod` |
| [`sky.hpp`](../../engine/world/include/oxwald/world/sky.hpp) | `computeSunPosition`, `computeMoonPosition`, `computeMoonPhase`, `starsRotation`, `sunLight`/`moonLight`, `PreethamSky` |
| [`time_of_day.hpp`](../../engine/world/include/oxwald/world/time_of_day.hpp) | `Curve`, `Gradient`, `AtmosphereCurves`, `TimeOfDay`, `SkyState` |
| [`weather.hpp`](../../engine/world/include/oxwald/world/weather.hpp) | `WindField`, `WindSettings`, `WeatherController`, `WeatherPreset` |
| [`water.hpp`](../../engine/world/include/oxwald/world/water.hpp) | `GerstnerWaves`, `GerstnerWave`, `BuoyancySettings`, `computeBuoyancy` |
| [`streaming.hpp`](../../engine/world/include/oxwald/world/streaming.hpp) | `ChunkStreamer`, `ChunkCallbacks`, `StreamingViewer`, `IChunkExecutor`, `ThreadPoolExecutor`, `InlineExecutor` |
| [`chunk_data.hpp`](../../engine/world/include/oxwald/world/chunk_data.hpp) | `ChunkData`, `serializeChunk`, `deserializeChunk` |
| [`physics_bridge.hpp`](../../engine/world/include/oxwald/world/physics_bridge.hpp) | `toShapeDesc(PhysicsHeightfieldTile)`, `toShapeDesc(VegetationCollider)` (нужен `Oxwald::physics`) |

GLSL-включения (корень `engine/shaders`): `world/gerstner.glsl`, `world/wind.glsl`, `world/preetham.glsl`, `world/cdlod.glsl`. Контракт рендерера и заметки для разработчиков модуля: [`docs/dev/modules/world.md`](../dev/modules/world.md).

## Что дальше

- [09. Физика](09-physics.md) — тела, к которым применяются коллайдеры ландшафта и силы плавучести.
- [13. ИИ](13-ai.md) — тайловый навмеш поверх стримящегося мира.
- [12. Аудио](12-audio.md) — эмбиент по погоде и времени суток, звуки шагов по `dominantLayer`.
- [17. RHI (Vulkan)](17-rhi-vulkan.md) — низкоуровневая база, на которой строится отрисовка мира.
- Глава о рендеринге (*скоро*) — ландшафт CDLOD, растительность, небо и вода на GPU.
- Глава о геймплейных компонентах ECS (*скоро*) — компоненты ландшафта, воды и стриминга на сцене.
- [Оглавление](README.md).
