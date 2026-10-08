# 23. Прозрачность, вода и частицы

> Модуль `render`, таргет `Oxwald::render`, пространство имён `ox::render`. Компоненты — `<oxwald/render/components/translucency.hpp>`, API области — `<oxwald/render/features/translucency/translucency.hpp>`, материалы — `<oxwald/assets/material.hpp>` (`ox::assets`).

## Зачем

Всё, что нельзя нарисовать непрозрачным G-проходом, рендерер собирает в одну область «translucency — water — particles». Она работает после непрозрачной геометрии (точка `AfterOpaque`) и в точке `Translucency` перед постобработкой:

- **прозрачные материалы** (`Transparent`): стёкла, голограммы, лёгкие занавески. Порядок их наложения решает либо Weighted Blended OIT (без сортировки), либо классическая сортировка от дальних к ближним;
- **преломление** (`Refractive`): толстое стекло, лёд, жидкость в колбе. Здесь учитываются толщина объекта, поглощение по закону Бугера — Ламберта, Френель и полное внутреннее отражение;
- **вода**: поверхность из волн Герстнера, отражения (планарные / SSR / небо), пена у берега и на гребнях, каустики на дне и подводный режим камеры;
- **GPU-частицы**: дым, искры, пыль, магия. Симуляция идёт целиком на GPU, отрисовка — в буфер пониженного разрешения с мягкими краями.

Всё это включается компонентами на сущностях и полями материала. Писать собственные проходы не нужно.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `assets::BlendMode` | Режим материала: `Opaque`, `AlphaTest`, `Transparent`, `Refractive` |
| OIT | Weighted Blended Order-Independent Transparency (McGuire 2013): прозрачные слои смешиваются без сортировки, за два буфера (накопление RGBA16F + revealage R8) |
| Сортированный путь | Premultiplied-смешивание объектов от дальних к ближним. Точный порядок, но сортируются объекты целиком, а не пиксели |
| `SceneColorRefraction` | Цепочка мипов HDR-кадра (до прозрачных). Её читают преломление и SSR воды; шероховатость выбирает мип |
| `ParticleEmitterComponent` | Эмиттер GPU-частиц (имя в редакторе — `ParticleEmitter`) |
| `WaterSurfaceComponent` | Водная поверхность (имя в редакторе — `WaterSurface`) |
| `WaterWave` | Одна волна Герстнера: направление, длина, амплитуда, крутизна, фаза |
| `TranslucencySnapshot` | Расширение `RenderSnapshot`: эмиттеры и вода, скопированные из мира функцией `extract()` |
| Фичи | `Translucency`, `Water`, `Underwater`, `Particles`; у каждой есть выключатель `r.Feature.<Имя>` |

## Шаг 1. Прозрачные и преломляющие материалы

Прозрачность задаётся материалом, а не компонентом. Материал `.oxmat` — это JSON с полями `assets::MaterialAsset`; неуказанные поля берут значения по умолчанию.

Тонированное оконное стекло (`Transparent`):

```json
{
  "blendMode": "Transparent",
  "baseColor": [0.15, 0.3, 1.0, 0.45],
  "roughness": 0.05,
  "renderQueueOffset": 1
}
```

Толстое зелёное стекло с поглощением (`Refractive`):

```json
{
  "blendMode": "Refractive",
  "baseColor": [1.0, 1.0, 1.0, 1.0],
  "roughness": 0.02,
  "ior": 1.5,
  "absorptionColor": [0.45, 0.9, 0.55],
  "absorptionDistance": 1.0
}
```

```cpp
#include <oxwald/assets/material.hpp>

Result<assets::MaterialAsset> glass = assets::loadMaterialFile(dir / "glass.oxmat");
assert(glass->blendMode == assets::BlendMode::Refractive);
renderer->resources().addMaterial(glassId, *glass);   // в игре это делает AssetManager
```

Полный пример: `samples/guide_examples/23-transparency-water-particles/translucency.cpp` (тест `MaterialsFromOxmat`), файлы `glass.oxmat` и `tinted_window.oxmat`.

Поля материала, которые важны для этой главы:

| Поле `MaterialAsset` | По умолчанию | Смысл |
| --- | --- | --- |
| `blendMode` | `Opaque` | `Transparent` — смешивание по альфе; `Refractive` — преломление фона |
| `baseColor.a` | 1 | Непрозрачность `Transparent`-материала |
| `alphaCutoff` | 0.5 | Порог для `AlphaTest` (листва, решётки) |
| `ior` | 1.5 | Показатель преломления: вода 1.33, стекло 1.5, алмаз 2.42. Из него же считается Френель: `F0 = ((ior−1)/(ior+1))²` |
| `transmission` | 0 | Доля пропускания; для `Refractive` значение 0 считается равным 1 |
| `thickness` | 0 | Толщина в метрах, если глубину задних граней не удалось получить |
| `absorptionColor` | (1, 1, 1) | Цвет, который останется после прохождения `absorptionDistance` метров |
| `absorptionDistance` | 0 | 0 — поглощения нет |
| `roughness` | 0.5 | Для стекла — степень матовости: выбирает мип размытого фона (`lod = roughness × (mips−1)`) |
| `renderQueueOffset` | 0 | ≠ 0 — прозрачный материал всегда идёт сортированным путём, а не через OIT |

Как рисуется преломляющий объект. Луч `refract(−V, N, 1/ior)` проходит сквозь объект на глубину его толщины: это разность глубин передних и задних граней (проход `RefractionBackDepth`) или `material.thickness`. Затем он упирается в фон не дальше `r.Refraction.MaxDistance`. Если в точке выборки оказался объект *перед* стеклом, выборка отбрасывается: это защищает от «протекания» переднего плана. Полное внутреннее отражение даёт отражение окружения. Отражения у прозрачных материалов берутся из предфильтрованного IBL; пиксельные reflection probes им пока недоступны.

## Шаг 2. OIT или сортировка

Способ отрисовки прозрачных (не преломляющих) объектов выбирает `r.Translucency.Method`:

| Значение | Как работает | Когда выбирать |
| --- | --- | --- |
| `Auto` (по умолчанию) | Сортировка, если видно не больше `r.Translucency.SortedMaxInstances` (4) прозрачных инстансов, иначе OIT | Почти всегда |
| `OIT` | Weighted Blended OIT: порядок не важен, нет мерцания при пересечении объектов | Много пересекающихся стёкол, листва с прозрачностью, голограммы |
| `Sorted` | Premultiplied-смешивание от дальних к ближним | Несколько крупных стёкол, где важен точный цвет наложения |

У OIT веса зависят от глубины, поэтому он лишь приближает результат: на стопке ярких слоёв цвета немного «сплющиваются». Сортировка точна для непересекающихся объектов, но сортирует инстансы целиком. Пересекающиеся стёкла при движении камеры «перескакивают». Если конкретному материалу нужен точный порядок, а остальной сцене подходит OIT, задайте ему `renderQueueOffset ≠ 0`.

```cpp
CVarRegistry::instance().set("r.Translucency.Method", "OIT");   // или из консоли: r.Translucency.Method OIT
```

Освещение прозрачных объектов такое же, как у непрозрачных: кластерные источники, тени CSM/spot/point, SH и IBL. Плюс туман: объёмный, если включены объёмные эффекты ([глава 22](22-volumetrics.md)), иначе высотный.

**Alpha test.** Материалы `AlphaTest` (листва, трава) при `r.AlphaTest.Dither` = `Auto` (по умолчанию) рисуются с хэшированным (стохастическим) альфа-тестом, когда включён TAA. Край листа превращается в шум, который TAA сглаживает в мягкую кромку. `On` включает режим всегда, `Off` — жёсткий порог `alphaCutoff`. Alpha-to-coverage недоступен: в рендерере нет MSAA.

Полный пример: `samples/guide_examples/23-transparency-water-particles/translucency_gpu.cpp`, тест `GlassPanesAndRefractiveSphere`. В нём при `r.Translucency.Method = OIT` зелёное стекло идёт проходом `Translucency.OIT`, а окно из `tinted_window.oxmat` (`renderQueueOffset = 1`) всё равно рисуется проходом `Translucency.Sorted`. Сфера из `glass.oxmat` рисуется проходом `Translucency.Refractive`.

## Шаг 3. Вода

Вода — компонент `WaterSurfaceComponent` на сущности. Базовая высота поверхности равна мировой Y сущности, `size` задаёт прямоугольник XZ с центром в сущности.

```cpp
#include <oxwald/render/components/translucency.hpp>

Entity lake = world.create("Lake");
lake.setPosition({0.0f, 0.25f, 0.0f});          // уровень воды
auto& w = lake.add<WaterSurfaceComponent>();
w.size = {40.0f, 40.0f};                        // <= 0 — бесконечный океан
w.waves = {
    {.direction = {1.0f, 0.3f}, .wavelength = 6.0f, .amplitude = 0.06f, .steepness = 0.5f},
    {.direction = {0.6f, -0.8f}, .wavelength = 3.1f, .amplitude = 0.035f, .steepness = 0.5f, .phase = 1.1f},
    {.direction = {-0.2f, 1.0f}, .wavelength = 1.7f, .amplitude = 0.015f, .steepness = 0.4f, .phase = 2.3f},
};
w.absorption = {0.45f, 0.09f, 0.06f};           // красный гаснет быстрее всех → бирюзовая глубина
```

Полный пример: `samples/guide_examples/23-transparency-water-particles/translucency.cpp` (тест `WaterSurfaceAndGerstnerWaves`).

**Волны** (`WaterWave`, до 16 штук):

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `direction` | (1, 0) | Направление бега волны в плоскости XZ |
| `wavelength` | 10 м | Длина волны λ; волновое число `k = 2π/λ`, частота `ω = √(g·k)·speedScale` |
| `amplitude` | 0.2 м | Высота гребня над базовым уровнем |
| `steepness` | 0.5 | 0 — синусоида, 1 — острые гребни; делится между всеми волнами (`qa = steepness/(k·N)`), поэтому петель не возникает |
| `phase` | 0 | Сдвиг фазы (рад) |
| `speedScale` | 1 | Множитель скорости |

Если список `waves` пуст, вода получает спокойную зыбь по умолчанию. Набирайте волны от длинных к коротким: 1–2 длинные задают «характер», 3–6 коротких добавляют рябь. Мелкую рябь мельче полуметра дешевле рисовать картой нормалей (`normalMap`, `detail*`), чем геометрией.

**Оптика, пена, каустики, подводный режим** (`WaterSurfaceComponent`):

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `visible` | true | Рисовать ли поверхность |
| `absorption` | (0.45, 0.09, 0.06) | Коэффициенты Бугера — Ламберта, 1/м: чем больше, тем быстрее гаснет канал |
| `scatterColor` | (0.02, 0.11, 0.13) | Цвет рассеяния глубокой воды (как альбедо, 0..1) |
| `refractionStrength` | 0.05 | Сила искажения дна |
| `roughness` | 0.04 | Шероховатость поверхности (размытость отражений) |
| `normalMap` | — | Детальная карта нормалей (пусто — процедурная) |
| `detailNormalStrength` | 0.35 | Сила детальных нормалей |
| `detailScale` | 0.12 | Повторов на метр |
| `detailSpeed` | 0.04 м/с | Скорость прокрутки |
| `shoreFoamDistance` | 0.35 м | Глубина воды, на которой ещё видна пена у берега |
| `crestFoam` | 0.5 | Пена на гребнях (0 — нет) |
| `foamIntensity` | 1 | Общая яркость пены |
| `causticsIntensity` | 1 | Яркость каустик на подводной геометрии |
| `causticsScale` | 0.3 | Повторов на метр |
| `causticsFalloff` | 0.25 | Затухание каустик на метр глубины |
| `underwaterColor` | (0.03, 0.16, 0.2) | Цвет тумана под водой |
| `underwaterDensity` | 0.06 1/м | Плотность подводного тумана |

Как устроен кадр воды:

1. `Water.Caustics` (AfterOpaque). Затемняет подводную геометрию по толщине водяного столба и накладывает анимированные каустики. Проход идёт до копии `SceneColorRefraction`, поэтому дно с каустиками видно и сквозь поверхность.
2. `Water.Surface` (Translucency). Плотная сетка вокруг камеры: ячейки растут с расстоянием, разрешение — `r.Water.GridResolution`. Вершины смещаются волнами Герстнера. Дно видно через преломление с поглощением и рассеянием. Отражение берётся из планарного отражения (если есть, см. [главу 21](21-reflections-gi.md)), иначе из SSR по `SceneColorRefraction` (`r.Water.SSRSteps` шагов), иначе из неба. Френель считается по Шлику с F0 = 0.02; снизу работает полное внутреннее отражение. Пена у берега считается по глубине воды над `SceneDepthCopy`. Поверхность пишет глубину, так что частицы и туман после неё ведут себя правильно.
3. `Water.Underwater` (фича `Underwater`). Включается сама, когда камера ниже поверхности: подводный туман `underwaterColor`/`underwaterDensity` и лёгкое «дрожание».

**Камера под водой и плавучесть.** Волны считаются одинаково в шейдере (`world/gerstner.glsl`), в рендерере на CPU и в плавучести gameplay. Поэтому проверить «камера под водой?» или «где поверхность под лодкой?» можно на CPU:

```cpp
#include <oxwald/render/features/translucency/translucency.hpp>

const GerstnerParams p = packGerstnerWaves(w.waves, /*baseHeight*/ 0.25f, /*time*/ t);
const f32 surface = gerstnerHeight(p, {cam.x, cam.z}, t);  // высота воды в точке XZ
const bool underwater = cam.y < surface;
const f32 maxSwell = gerstnerAmplitudeSum(p);              // ±граница поверхности вокруг базовой высоты
```

**Gameplay-вода.** Компонент gameplay `WaterComponent` (плавучесть, [глава 16](16-world.md) и [глава 32](32-gameplay-components.md)) тоже попадает в рендер через мост `water_bridge.cpp`, с теми же волнами. Чтобы задать вид такой воды, поставьте на ту же сущность ещё и `WaterSurfaceComponent`. Если данные о воде у вас свои, добавьте поверхность в снапшот после `extract()`: `addWaterSurface(snapshot, SnapshotWater{...})`.

## Шаг 4. GPU-частицы

Эмиттер — компонент `ParticleEmitterComponent`. Форма спавна задаётся в локальном пространстве сущности (ось эмиссии +Y). Сами частицы живут в мировом пространстве: если сдвинуть эмиттер, уже выпущенный дым за ним не поедет.

Дым (освещённый, мягкий, отсортированный):

```cpp
auto& s = smoke.add<ParticleEmitterComponent>();
s.maxParticles = 512;
s.spawnRate = 40.0f;
s.lifetime = {2.5f, 3.5f};
s.shape = ParticleShape::Sphere;
s.radius = 0.25f;
s.speed = {0.1f, 0.3f};
s.velocity = {0.0f, 0.6f, 0.0f};          // подъём
s.drag = 0.4f;
s.turbulence = 0.6f;                       // curl-noise, м/с²
s.size = {0.5f, 0.8f};
s.sizeOverLife = {{0.0f, 0.6f}, {1.0f, 2.2f}};
s.colorOverLife = {{0.0f, {0.9f, 0.9f, 0.92f, 0.0f}},
                   {0.15f, {0.9f, 0.9f, 0.92f, 0.6f}},
                   {1.0f, {0.8f, 0.8f, 0.82f, 0.0f}}};
s.sprite = ParticleSprite::Smoke;
s.lit = true;                              // освещённый дым…
s.emissive = 0.0f;                         // …без собственного свечения
s.softDistance = 0.5f;
s.sort = true;
```

Искры (залпы, растянутые по скорости, с отскоком от пола):

```cpp
auto& k = sparks.add<ParticleEmitterComponent>();
k.spawnRate = 0.0f;                        // только залпы
k.bursts = {{.time = 0.0f, .count = 80, .cycles = 0, .interval = 0.5f}};   // 80 искр каждые 0.5 с
k.lifetime = {0.8f, 1.4f};
k.shape = ParticleShape::Cone;
k.coneAngle = 40.0f;
k.speed = {1.5f, 3.0f};
k.gravityScale = 1.0f;
k.size = {0.03f, 0.05f};
k.color = {1.0f, 0.55f, 0.15f, 1.0f};
k.colorOverLife = {{0.0f, {1, 1, 1, 1}}, {1.0f, {1.0f, 0.3f, 0.1f, 0.0f}}};
k.emissive = 6.0f;                         // ярче сцены → подхватит bloom
k.blend = ParticleBlend::Additive;
k.renderMode = ParticleRenderMode::StretchedBillboard;
k.stretch = 0.06f;
k.sprite = ParticleSprite::Spark;
k.collision = ParticleCollision::Bounce;
k.bounce = 0.35f;
```

Полный пример: `samples/guide_examples/23-transparency-water-particles/translucency.cpp` (тесты `SmokeAndSparksEmitters`, `EmitterDefaults`).

### Все поля эмиттера

**Общие и спавн**

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `enabled` | true | Выключенный эмиттер не попадает в снапшот |
| `maxParticles` | 1024 | Ёмкость буфера; ограничивается сверху `r.Particles.Budget` |
| `seed` | 1 | Зерно генератора: одинаковое зерно даёт одинаковый узор |
| `spawnRate` | 50 | Частиц в секунду (0 — только залпы) |
| `bursts` | пусто | Залпы `ParticleBurst` (см. ниже) |
| `loop` | true | Повторять цикл эмиссии |
| `duration` | 5 с | Длина одного цикла; залпы повторяются вместе с ним |
| `lifetime` | (1.5, 2.5) с | Время жизни: min, max |

`ParticleBurst`: `time` — 0 с (от начала цикла), `count` — 10, `cycles` — 1 (0 = бесконечно), `interval` — 1 с между повторами.

**Форма**

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `shape` | `Point` | `Point`, `Sphere`, `Cone`, `Box`, `MeshSurface` |
| `radius` | 0.5 м | Радиус сферы / основания конуса |
| `coneAngle` | 25° | Половинный угол конуса |
| `boxExtents` | (0.5, 0.5, 0.5) м | Половинные размеры коробки |
| `shapeMesh` | — | `MeshSurface`: частицы рождаются на треугольниках этого меша (первый submesh) |
| `emitFromShell` | false | Сфера: только поверхность, без объёма |

**Движение**

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `speed` | (1, 2) м/с | Начальная скорость вдоль направления формы: min, max |
| `velocity` | (0, 0, 0) | Добавочная начальная скорость (локальное пространство) |
| `gravity` | (0, −9.81, 0) | Вектор гравитации |
| `gravityScale` | 0 | Множитель гравитации (0 — частицы её не чувствуют) |
| `drag` | 0 1/с | Сопротивление воздуха |
| `turbulence` | 0 м/с² | Ускорение от curl-noise |
| `turbulenceFrequency` | 1 1/м | Пространственная частота шума |
| `turbulenceSpeed` | 0.5 | Скорость анимации поля шума |

**Внешний вид**

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `size` | (0.1, 0.2) м | Начальный размер: min, max |
| `sizeOverLife` | пусто (= 1) | Кривая множителя размера: ключи `{time 0..1, value}` |
| `color` | (1, 1, 1, 1) | Умножается на `colorOverLife` |
| `colorOverLife` | пусто (= белый) | Градиент `{time 0..1, color rgba}`; альфа — непрозрачность |
| `emissive` | 1 | Свечение относительно экрана: 1 — цвет при текущей экспозиции; для освещённого дыма 0 |
| `rotation` | (0, 360)° | Начальный поворот: min, max |
| `rotationSpeed` | (0, 0) °/с | Скорость вращения: min, max |
| `texture` | — | Спрайт или атлас-флипбук (пусто — процедурный `sprite`) |
| `sprite` | `SoftCircle` | Процедурный спрайт: `SoftCircle`, `Smoke`, `Spark` |
| `atlasColumns`, `atlasRows` | 1, 1 | Сетка кадров атласа |
| `flipbookFps` | 0 | Кадров в секунду; 0 — атлас проигрывается один раз за жизнь частицы |
| `randomStartFrame` | false | Случайный начальный кадр |
| `blend` | `Alpha` | `Additive` (огонь, искры), `Alpha` (дым), `Premultiplied` |
| `renderMode` | `Billboard` | `Billboard`, `StretchedBillboard` (по скорости), `Mesh` |
| `stretch` | 0.05 | Растянутые спрайты: дополнительная длина на 1 м/с |
| `mesh` | — | Меш для режима `Mesh` |
| `lit` | false | Освещение: солнце с тенью CSM, кластерные источники, ambient (по вершинам) |
| `softDistance` | 0.3 м | Мягкое затухание у геометрии; 0 — жёсткий край |
| `sort` | false | Сортировка частиц этого эмиттера от дальних к ближним (не больше 2048 частиц) |

**Столкновения с буфером глубины** (работают при `r.Particles.Collision`)

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `collision` | `None` | `None`, `Bounce` (отскок), `Kill` (исчезнуть при касании) |
| `bounce` | 0.4 | Упругость отскока |
| `friction` | 0.2 | Потеря касательной скорости при ударе |

Кривые `sizeOverLife`/`colorOverLife` запекаются в градиентную текстуру 64×2 на эмиттер, так что количество ключей на цену кадра не влияет. Столкновения работают по экранному буферу глубины: частица, улетевшая за край экрана или за объект, ни с чем не столкнётся. Для гарантированной коллизии (дождь на крыше вне кадра) нужна физика, а не GPU-частицы.

Кадр частиц: `Particles.Simulate` (один раз за кадр, в первом виде) → `Particles.LowResDepth` (downsample по ближайшей глубине) → `Particles.Render` в буфер разрешения 1/`r.Particles.ResolutionDivisor` → `Particles.Composite` (depth-aware bilateral upsampling). Число отрисовываемых частиц пишет сама симуляция в indirect-аргументы, CPU их не считает.

## Шаг 5. Настройки качества и цена

| CVar | По умолчанию | Low / Medium / High / Ultra |
| --- | --- | --- |
| `r.Translucency.Method`, `.SortedMaxInstances` | Auto, 4 | — |
| `r.Refraction`, `.Strength`, `.MaxDistance` | on, 1, 4 м | — |
| `r.Refraction.Mips` | 6 | Reflections: 3, 5, 6, 7 |
| `r.Refraction.BackfaceDepth` | on | Shading: off, on, on, on |
| `r.AlphaTest.Dither` | Auto | — |
| `r.Water`, `r.Water.Underwater`, `r.Water.MaxExtent` | on, on, 2000 м | — |
| `r.Water.GridResolution` | 256 | Shading: 96, 160, 256, 384 |
| `r.Water.SSRSteps` | 12 | Reflections: 0, 8, 12, 20 |
| `r.Water.Caustics` | on | Effects: off, on, on, on |
| `r.Particles` | on | — |
| `r.Particles.Budget` (на эмиттер) | 262144 | Effects: 16384, 65536, 262144, 1048576 |
| `r.Particles.ResolutionDivisor` | 2 | Effects: 4, 2, 2, 1 |
| `r.Particles.Collision`, `.Lighting`, `.SoftParticles` | on | Effects: off, on, on, on |
| `r.Particles.Sorting` | on | Effects: off, off, on, on |

```cpp
scalability::setGroup(Scalability::Effects, QualityLevel::Low);
// r.Particles.Budget = 16384, r.Particles.ResolutionDivisor = 4, r.Water.Caustics = false
```

Полный пример: `samples/guide_examples/23-transparency-water-particles/translucency.cpp` (тест `CVarsAndScalability`). О группах масштабируемости — [глава 04](04-cvars-quality.md) и [глава 26](26-quality-settings.md).

GPU-время на Apple M4 Pro, 1080p, High (мс): копия для преломления 0.31 + копия глубины 0.12, задние грани 0.02, преломляющие объекты 0.14, сортированное стекло 0.15, поверхность воды 0.48 (Ultra 0.73), частицы: симуляция 0.03 + отрисовка 0.31 + low-res глубина 0.06 + композит 0.06.

На GPU всё это проверяет `samples/guide_examples/23-transparency-water-particles/translucency_gpu.cpp`: тесты `WaterAndParticles` (проходы `Water.Surface`, `Particles.*` и их выключение cvar'ами) и `UnderwaterCamera`.

## Типичные ошибки и подводные камни

- **Стекло выглядит непрозрачным.** У `Transparent` непрозрачность задаёт `baseColor.a`. При альфе 1 «стекло» непрозрачно. Для преломления нужен `Refractive`, а не `Transparent`.
- **Стёкла мерцают при движении камеры.** Сортированный путь сортирует объекты целиком, и пересекающиеся стёкла меняются местами. Переключите `r.Translucency.Method` на `OIT` или разнесите геометрию.
- **В OIT «пропал» цвет одного слоя.** Weighted Blended OIT — приближение. Если точный цвет конкретного материала важен, задайте ему `renderQueueOffset ≠ 0`: он всегда пойдёт сортированным путём.
- **Преломление показывает объект перед стеклом.** Обычно это тонкая или незамкнутая геометрия без задних граней. Тогда толщина берётся из `thickness`, а при 0 смещение может выйти за объект. Задайте `thickness` или замкните меш.
- **Вода «стоит».** Волны анимируются по `snapshot.time` (`ExtractOptions::time`). Если свой цикл кадра не передаёт время в `extract()`, поверхность замирает.
- **Слишком острые гребни.** `steepness` делится на все волны, но при больших амплитудах и коротких длинах сетка `r.Water.GridResolution` не успевает за формой. Уменьшите амплитуду коротких волн или перенесите их в `normalMap`.
- **Подводный режим не включается.** Камера должна быть ниже *волновой* высоты внутри прямоугольника `size`. У бесконечной воды (`size ≤ 0`) сетка вокруг камеры имеет полуразмер `r.Water.MaxExtent`.
- **Частиц меньше, чем `maxParticles`.** Ёмкость ограничена `r.Particles.Budget` текущего уровня Effects: на Low это 16384.
- **Дым светится в темноте.** У дыма оставлен `emissive = 1` по умолчанию. Для освещённых частиц ставьте `lit = true` и `emissive = 0`.
- **Искры проходят сквозь пол за кадром.** Коллизии считаются по экранному буферу глубины. Вне экрана столкновений нет.
- **`sort` не помогает.** Сортировка работает только до 2048 частиц эмиттера и только при `r.Particles.Sorting` (на Low и Medium выключено).
- **Частицы и TAA.** Прозрачные объекты и частицы не пишут векторы движения. Быстрые искры при TAA могут оставлять след: уменьшите размер или используйте `StretchedBillboard`.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`components/translucency.hpp`](../../engine/render/include/oxwald/render/components/translucency.hpp) | `ParticleEmitterComponent`, `ParticleBurst`, `ParticleCurveKey`, `ParticleColorKey`, перечисления `ParticleShape/Blend/RenderMode/Collision/Sprite`, `WaterSurfaceComponent`, `WaterWave`, `registerTranslucencyTypes` |
| [`features/translucency/translucency.hpp`](../../engine/render/include/oxwald/render/features/translucency/translucency.hpp) | `GerstnerParams`, `packGerstnerWaves`, `gerstnerDisplacement`, `gerstnerHeight`, `gerstnerAmplitudeSum`, `SnapshotWater`, `SnapshotParticleEmitter`, `TranslucencySnapshot`, `addWaterSurface`, ресурсы `res::kSceneColorRefraction`, `kSceneDepthCopy`, `kRefractionBackDepth`, `kPlanarReflection` |
| [`assets/material.hpp`](../../engine/assets/include/oxwald/assets/material.hpp) | `MaterialAsset` (`blendMode`, `ior`, `transmission`, `thickness`, `absorption*`, `renderQueueOffset`), `loadMaterialFile` |
| [`render_feature.hpp`](../../engine/render/include/oxwald/render/render_feature.hpp) | `DrawBucket::Transparent/Refractive` — для своих фич, рисующих прозрачные списки |

Шейдеры: `engine/shaders/render/translucency/`. Заметки для разработчиков модуля: [`docs/dev/modules/render.md`](../dev/modules/render.md), раздел 12.

## Что дальше

- [19. Материалы](19-materials.md) — остальные поля `.oxmat` и редактор материалов.
- [21. Отражения и GI](21-reflections-gi.md) — планарные отражения, которые подхватывает вода.
- [22. Объёмные эффекты](22-volumetrics.md) — туман, который видят прозрачные объекты.
- [24. Трассировка лучей](24-ray-tracing.md) — RT-преломление стекла и цветные тени сквозь него.
- [25. Апскейлеры и постобработка](25-upscalers-postprocess.md) — bloom для ярких частиц, TAA и прозрачность.
- [16. Мир](16-world.md) — волны Герстнера на CPU и плавучесть.
- [Оглавление](README.md).
