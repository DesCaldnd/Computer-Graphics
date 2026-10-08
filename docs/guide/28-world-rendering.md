# 28. Рендеринг мира: ландшафт, растительность, небо, скиннинг

> Модуль `render`, область world-skinning (таргет `Oxwald::render`, пространство имён `ox::render`, заголовок
> `<oxwald/render/features/world/world_skinning.hpp>`, компоненты — `<oxwald/render/components/world.hpp>`). Нужен
> модуль `world` (`OX_RENDER_HAS_WORLD`); мост из ECS — модуль `gameplay` (`<oxwald/gameplay/world.hpp>`). CPU-часть
> мира (карты высот, CDLOD, разброс растительности, время суток) — в [главе 16](16-world.md), анимация и палитры
> скиннинга — в [главе 10](10-animation.md). Здесь — как всё это попадает на экран.

## Зачем

Открытый мир на GPU — это четыре фичи рендера, которые встраиваются в обычный кадр:

| Фича | Точка / порядок | Что делает |
| --- | --- | --- |
| `WorldGeometry` | AfterDepth + AfterOpaque, −10000 | ландшафт (CDLOD, до 8 слоёв splat, трипланар, дыры, тесселяция) и растительность (GPU-отсечение, LOD, импосторы, ветер); пассы `World.Prepass`, `World.HiZ`, `World.Forward` |
| `WorldShadows` | Shadows, 100 | ландшафт и деревья в каскадах солнца: `World.ShadowCascades`, `World.ShadowMask` |
| `WorldSky` | PreDepth + AfterOpaque, −1100, группа `Sky` (приоритет 100) | небо Preetham, солнце, луна с фазой, звёзды, ночь, воздушная перспектива, куб неба для IBL; без неба мира — обычное небо ядра |
| `Skinning` | PreDepth, −2000 | compute-скиннинг (LBS или dual quaternion) в буферы вершин текущего и прошлого кадра → векторы движения, BLAS для RT |

Ландшафт и растительность рисуются сразу после depth prepass (Hi-Z перестраивается), тени — в те же каскады, шейдинг
— до неба. Поэтому SSAO, SSR, туман, TAA и постобработка работают с ними как с любой геометрией.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `WorldSnapshot` | Расширение `RenderSnapshot`: ландшафты, растительность, небо, ветер, «приминатели» травы. Заполняется мостом из gameplay или вручную |
| `TerrainSnapshot` | Неизменяемые копии карты высот и splat map + версии и «грязные» прямоугольники для частичной заливки |
| `VegetationSnapshot` | Слои (прототип, тип, LOD, тени) + батчи экземпляров `world::VegetationInstanceGpu` с ячейками |
| `WorldSkySnapshot` | Небо из `world::SkyState`: Preetham, солнце/луна, фаза, вращение звёзд, атмосфера |
| `TerrainRenderComponent` | Вид ландшафта: трипланар, смешивание по высоте, макровариация, размер тайла текстуры |
| `VegetationPrototypesComponent` | Меши LOD, материал, импосторы, параметры ветра для прототипа растительности |
| `WorldRenderData` | Контракт gameplay → render: что миру нужно нарисовать (заполняется в фазе Extract) |
| `SkinningSnapshot`, `ISkinnedOutputs` | Метод скиннинга по сущности; выходные буферы скиннинга для RT и своих фич |

Поток данных в игре:

```
TerrainComponent, VegetationComponent, SkyComponent, TimeOfDayComponent, WindComponent, WaterComponent (gameplay)
  + TerrainRenderComponent, VegetationPrototypesComponent (render)          ← редактор / .oxscene
        │  Gameplay.World.* системы (и в режиме редактора)
        ▼
  WorldRuntime (heightfield, splat, quadtree, чанки растительности, TimeOfDay)
        │  фаза Extract
        ▼
  WorldRenderData ──► render::extract(world, snapshot, {.services}) ──► WorldSnapshot ──► кадр
                      (хук моста gameplay → render, копирует карту высот один раз на версию)
```

## Шаг 1. Мир из компонентов сцены

Обычный путь — компоненты на сущностях; рантайм и редактор делают остальное. Полный справочник компонентов —
[глава 32](32-gameplay-components.md); здесь — минимальная сцена:

```cpp
#include <oxwald/gameplay/world.hpp>
#include <oxwald/render/features/world/world_skinning.hpp>

Entity terrain = world.create("Terrain");
auto& t = terrain.add<gameplay::TerrainComponent>();
t.resolution = 129;
t.worldSize = 256.f;
t.heightScale = 20.f;
t.noise.fractal.frequency = 1.f / 128.f;
t.lod = {.leafNodeSize = 16, .lodCount = 4, .viewDistance = 1200.f};
t.layers = {Uuid::fromName("guide.grass"), Uuid::fromName("guide.rock")}; // материалы слоёв splat
t.splatRules = {{.layer = 1, .minSlopeDeg = 25.f}};                    // скалы на склонах
terrain.add<render::TerrainRenderComponent>().triplanarSlopeDeg = 30.f;  // вид — только для рендера

auto& veg = terrain.add<gameplay::VegetationComponent>();
world::VegetationLayer trees{.name = "tree", .kind = world::VegetationKind::Tree, .prototype = 0,
                             .minDistance = 8.f, .boundingRadius = 4.f};
veg.layers.push_back(trees);
veg.scatterRadius = 96.f;                       // разбрасывается вокруг наблюдателей (камера, StreamingSource)

Entity env = world.create("Environment");
env.add<EnvironmentComponent>();
env.add<gameplay::SkyComponent>();
env.add<gameplay::TimeOfDayComponent>().localHours = 17.5;   // ведёт солнце, туман, экспозицию
env.add<gameplay::WindComponent>().speed = 6.f;              // качает деревья и траву
env.get<gameplay::TimeOfDayComponent>().sun = sun.ref();      // какой направленный свет вести
```

После тика планировщика `WorldRenderData` содержит ландшафт, батчи растительности, небо и ветер, а
`render::extract(world, snapshot, {.services = &services})` + `finalizeWorldSnapshot(snapshot)` превращают их в
`WorldSnapshot` — в рантайме это делает `IRenderer::extract()` движка.

Полный пример: `samples/guide_examples/28-world-rendering/world_components.cpp` (CPU, без GPU: проверяет, что
`TerrainRenderComponent` доехал до снимка).

## Шаг 2. Ландшафт на GPU

На GPU ландшафт — это R32F/R16 карта высот, RGBA8 карта нормалей (считается compute-шейдером, с мипами), R8 маска
дыр и две RGBA8 текстуры splat (до 8 слоёв). Отбор патчей CDLOD идёт на потоке рендера **для каждого вида и
каскада**; каскады теней берут LOD главной камеры, чтобы тени совпадали с геометрией.

Без gameplay (инструменты, свой мир) снимок заполняется вручную — так делает пример:

```cpp
auto fill = [&](ox::render::RenderSnapshot& s, ox::u32) {
    ox::render::WorldSnapshot& w = s.extension<ox::render::WorldSnapshot>();
    ox::render::TerrainSnapshot t;
    t.entityId = 1;                      // для пикинга (encodeEntityId), 0 = нет
    t.heightfield = hf;                  // std::shared_ptr<const world::Heightfield>, неизменяемый
    t.heightfieldVersion = 1;            // новая версия → заливка
    t.splat = splat;
    t.splatVersion = 1;
    t.layerMaterials = layers;           // материал на слой splat (≤ 8)
    t.lod = {.leafNodeSize = 16, .lodCount = 4, .viewDistance = 1200.0f};
    w.terrains.push_back(t);
};
// после заполнения: ox::render::finalizeWorldSnapshot(snapshot);
```

Полный пример: `samples/guide_examples/28-world-rendering/world_gpu.cpp`, тест `TerrainVegetationSky`.

**Правки кистью и частичная заливка.** После мазка кисти (`WorldRuntime::applyBrush` или свой
`world::applyBrush`, [глава 16](16-world.md)) новая версия карты высот приходит с `dirtyRect` и
`dirtySinceVersion` = версия, которую рендерер уже видел: заливается только прямоугольник. Если рендерер эту версию
не видел (пропущенный снимок, первый кадр) или стоит `fullUpload`, заливается всё. Тест движка
`TerrainDirtyRectUpload` проверяет, что частичная заливка даёт ту же картинку, что и полная.

**Шейдинг слоёв.** Каждый слой splat — обычный материал сцены ([глава 19](19-materials.md)), читается через bindless
(слои могут быть разного размера). На пиксель смешиваются `r.Terrain.MaxLayers` самых сильных слоёв, с
учётом высоты (ORM occlusion или яркость альбедо как «высота»), трипланаром на крутых склонах, вторым повёрнутым
отсчётом против повторяемости и низкочастотной макровариацией.

| `TerrainRenderComponent` | По умолч. | Смысл |
| --- | --- | --- |
| `triplanarSlopeDeg` | 35 | склоны круче — в трипланарной проекции (при `r.Terrain.Triplanar`) |
| `heightBlend` | 0.2 | глубина смешивания слоёв по высоте (0 — просто веса) |
| `macroVariation` | 0.35 | низкочастотная вариация альбедо (ломает повтор вдали) |
| `tilingBreakup` | 0.5 | второй повёрнутый/масштабированный отсчёт, смешанный шумом |
| `layerTileMeters` | 4 | метров на один повтор текстуры при `uvTiling` материала = 1 |
| `castShadows` | true | ландшафт отбрасывает тени в каскады |
| `tessellationHeight` | 0.08 | амплитуда смещения тесселяции, м |

**Тесселяция** (`r.Terrain.Tessellation`, только Ultra и только при `DeviceCaps::tessellationShader`): патчи LOD 0
ближе 40 м, коэффициенты падают до 1 к краю диапазона (без трещин со стыками), смещение — высота доминирующего слоя ×
`tessellationHeight`. Тесселируются prepass и forward, тени используют базовую сетку.

## Шаг 3. Растительность на GPU

Экземпляры живут в одной арене. Для каждого вида и каскада CPU отсекает **ячейки** (фрустум + дальность → диапазон LOD
→ ёмкость слотов), затем `veg_cull.comp` проверяет **каждый экземпляр** (сфера против плоскостей, прореживание по
`r.Foliage.Density`, LOD и затухание — как `world::selectVegetationLod`) и раскладывает их по слотам (прототип, LOD);
`veg_args.comp` пишет число экземпляров в фиксированный список indirect-команд.

```cpp
const world::VegetationChunk treeChunk =
    world::VegetationScatterer({trees}).scatter({-120.0f, -120.0f}, 240.0f, sc, /*cellSize*/ 32.0f);
auto treeGpu = std::make_shared<std::vector<world::VegetationInstanceGpu>>();
for (const world::VegetationInstance& i : treeChunk.instances) treeGpu->push_back(world::toGpu(i, trees));
auto treeCells = std::make_shared<std::vector<world::VegetationCell>>(treeChunk.cells);

ox::render::VegetationSnapshot tv;
tv.layers.push_back({0, world::VegetationKind::Tree, trees.boundingRadius, trees.lod, /*castsShadow*/ true});
tv.batches.push_back({/*key*/ 1, /*version*/ 1, treeGpu, treeCells});   // новая version → перезаливка
w.vegetation.push_back(tv);
w.hasWind = true;
w.wind = wind.toGpu(10.0f);                                               // world::WindField
w.interactors.push_back({0.0f, groundY, 60.0f, 1.0f});                    // xyz + радиус: трава расступается (≤ 8)
```

Полный пример: тот же `TerrainVegetationSky` (≈350 деревьев с импосторами, ≈1500 кустов травы, ветер, небо).

| Механизм | Как работает |
| --- | --- |
| LOD и переходы | расстояния из `world::VegetationLodSettings` (`lodDistances`, `impostorDistance`, `cullDistance`, `fadeRange`) × `r.Foliage.DrawDistanceScale`; перекрёстное затухание — дизерингом с дополняющими масками |
| Импосторы | полу-октаэдрические 8×8 кадров по 128² (альбедо + нормаль), печатаются в кадре (`Vegetation.ImpostorBake`) один раз на набор мешей прототипа; дистанция × `r.Foliage.ImpostorDistanceScale` |
| Ветер | качание ствола ∝ (высота)², трепет веток/листьев по цвету вершин R/G, фаза — B; трава отклоняется от `interactors` |
| Листва | двусторонняя, нормали «наружу кроны»; подсвеченные сзади листья получают солнце через `translucency` |
| Тени | только деревья (`castsShadow`), в каскады солнца; трава теней не отбрасывает |

**Свои меши растительности.** Без описания прототипа рисуется встроенный процедурный меш по типу слоя (дерево, куст
травы, кустарник). Свои меши задаются компонентом:

| `VegetationPrototypeDesc` | По умолч. | Смысл |
| --- | --- | --- |
| `prototype` | 0 | совпадает с `world::VegetationLayer::prototype` |
| `lods` | пусто | меш на LOD 0..2 (недостающие повторяют предыдущий); пусто — встроенный |
| `material` | nil | заменяет материалы всех сабмешей |
| `impostor` | true | печь импостор из последнего LOD (деревья, крупные кусты) |
| `windSway`, `windFlutter` | 1, 1 | множители качания ствола и трепета |
| `translucency` | 0.6 | просвечивание листьев сзади |
| `castShadows` | true | только для деревьев |

Конвенции меша для ветра: Y вверх, начало координат — у основания; цвет вершины R — вес трепета веток, G — листьев,
B — сдвиг фазы, A — множитель непрозрачности.

## Шаг 4. Небо и день-ночь

`WorldSky` рисует небо Preetham с сумеречным множителем (декада яркости на каждые 2.5° под горизонтом), ночное и
лунное небо, звёзды в небесной системе координат, диск луны с фазой (освещён солнцем по геометрии) и диск солнца с
потемнением к краю. Купол без дисков рендерится в куб — он же источник IBL для всей сцены и прозрачных объектов.

```cpp
world::TimeOfDay tod({.location = {52.37, 4.90}, .year = 2024, .month = 6, .day = 21, .localHours = 16.5,
                      .utcOffsetHours = 2.0, .timeScale = 0.0, .paused = true});
const world::SkyState sky = tod.state();
w.sky = skyFromState(sky);        // preetham.toGpu(), направления, фаза луны, атмосфера, свет солнца/луны
// солнце сцены — из sky.mainLightDirection / mainLightIlluminance / mainLightColor
```

**Троттлинг IBL.** Куб неба перерисовывается, только когда солнце или луна сдвинулись больше чем на
`r.Sky.IBLUpdateDegrees` (по умолчанию 1°): ключ `worldSkyIblKey(sky, degrees)` квантует их положение.
`finalizeWorldSnapshot` пишет его в `SnapshotEnvironment::iblKey` и добавляет окружение по умолчанию, если небо есть,
а `EnvironmentComponent` нет.

```cpp
EXPECT_EQ(worldSkyIblKey(noon, 1.0f), worldSkyIblKey(halfMinuteLater, 1.0f)); // ~0.1° — тот же куб
ox::render::finalizeWorldSnapshot(snapshot);
snapshot.environment->iblKey;                                                 // задан
```

Полный пример: `samples/guide_examples/28-world-rendering/world_render_cpu.cpp`.

**Воздушная перспектива**: рэлеевское рассеяние + дымка с масштабом высоты 8 км, подсветка из куба неба. Если
активен объёмный туман ([глава 22](22-volumetrics.md)), она пропускается — туман делает то же точнее. В игре
`TimeOfDayComponent` ведёт и направленный свет, и туман/ambient `EnvironmentComponent`, и экспозицию камеры; ветер
моста передаётся и в объёмный туман.

## Шаг 5. Compute-скиннинг

Скелетные меши (`SkinnedMeshComponent` из gameplay, палитра из модуля animation) скиннятся compute-шейдером **до**
depth prepass в двойной буфер: текущий и прошлый кадр. Все проходы ядра читают вершины оттуда (`oxFetchVertex`),
поэтому у анимированных персонажей корректные векторы движения (TAA, motion blur) и тени без «позы привязки».

| Метод | Как выбрать |
| --- | --- |
| Linear blend (LBS) | по умолчанию |
| Dual quaternion (DQS) | `SkinnedMeshComponent::skinningMethod = anim::SkinningMethod::DualQuaternion`; вручную — `SkinningSnapshot::methods[entityId] = GpuSkinningMethod::DualQuaternion`. Сохраняет объём на сгибах; нужен `r.Skinning.Compute` |

Без gameplay меш со скиннингом кладётся в снимок напрямую — палитра `palette[j] = model[j] · inverseBind[j]`
([глава 10](10-animation.md)) и палитра прошлого кадра:

```cpp
ox::render::SnapshotMesh m;
m.entityId = ox::render::encodeEntityId(41);
m.mesh = tube;                                   // меш с данными скиннинга (assets::MeshData::skin)
m.materialOffset = u32(s.materials.size());
m.materialCount = 1;
s.materials.push_back(orange);
m.paletteOffset = u32(s.palettes.size());
m.paletteCount = u32(cur.size());
s.palettes.insert(s.palettes.end(), cur.begin(), cur.end());
m.prevPaletteOffset = u32(s.palettes.size());   // прошлый кадр → векторы движения
s.palettes.insert(s.palettes.end(), prev.begin(), prev.end());
s.meshes.push_back(m);
s.extension<ox::render::SkinningSnapshot>().methods[m.entityId] = ox::render::GpuSkinningMethod::DualQuaternion;
```

**Выходы для трассировки лучей и своих фич.** Фича `Skinning` реализует `ISkinnedOutputs`: буфер вершин
`GpuSkinnedVertex` (model space, 40 байт: позиция, нормаль, тангент) и для каждой сущности смещения текущего и
прошлого кадра. Ресурс графа — `SkinnedVertices` (`kSkinnedVerticesResource`): объявите его чтение, чтобы получить
барьер. Модуль RT уже использует это для refit BLAS ([глава 24](24-ray-tracing.md)).

```cpp
const auto* skinning = dynamic_cast<const ox::render::ISkinnedOutputs*>(renderer->features().find("Skinning"));
const ox::render::SkinnedOutputs& out = skinning->skinnedOutputs();
// out.buffer, out.stride == 40, out.items[i]: entityId, meshInfoIndex, vertexCount, currentOffset, previousOffset
```

Полный пример: тест `ComputeSkinningOutputs` в `world_gpu.cpp` (труба на трёх суставах сгибается, вершина верха
уходит в −X). Без compute (`r.Skinning.Compute 0`) скиннит вершинный шейдер — только линейно.

## Шаг 6. Качество и стоимость

Cvar'ы области (полная таблица всех групп — [глава 26](26-quality-settings.md)):

| CVar | По умолч. | Группа: Low / Medium / High / Ultra | Смысл |
| --- | --- | --- | --- |
| `r.Terrain.LODScale` | 1 | ViewDistance: 0.5 / 0.75 / 1 / 1.5 | масштаб дальностей CDLOD |
| `r.Terrain.MaxLayers` | 8 | Shading: 2 / 3 / 4 / 8 | слоёв splat на пиксель |
| `r.Terrain.Triplanar` | true | Shading: off / on / on / on | трипланар на склонах |
| `r.Terrain.Tessellation` | false | Shading: off / off / off / on | тесселяция вблизи |
| `r.Sky.CubeSize` | 128 | Shading: 64 / 128 / 128 / 256 | куб неба (IBL, перспектива) |
| `r.Foliage.Density` | 1 | Foliage: 0.35 / 0.6 / 0.85 / 1 | доля экземпляров |
| `r.Foliage.DrawDistanceScale` | 1 | Foliage: 0.5 / 0.75 / 1 / 1.5 | дальности LOD и отсечения |
| `r.Foliage.ImpostorDistanceScale` | 1 | Foliage: 0.5 / 0.75 / 1 / 1.5 | переход на импосторы |
| `r.Foliage.Grass`, `r.Foliage.Shadows` | true | Foliage: off / on / on / on | трава; тени деревьев |
| `r.Sky.AerialPerspective` | true | Volumetrics: off / on / on / on | воздушная перспектива |
| `r.Terrain`, `r.Terrain.Shadows`, `r.Foliage`, `r.Foliage.Impostors` | true | — | выключатели |
| `r.Sky.AerialPerspective.Density` | 1 | — | множитель экстинкции чистого воздуха |
| `r.Sky.IBLUpdateDegrees` | 1 | — | сдвиг солнца/луны (°), обновляющий куб неба |
| `r.Skinning.Compute` | true | — | compute-скиннинг (иначе вершинный шейдер, только LBS) |

Стоимость на Apple M4 Pro, 1080p (`WorldSkinningTest.PerfReport1080p`: ландшафт 2 км с 4 слоями, ≈20k деревьев,
≈25k кустов травы, небо): **≈7.5 мс GPU** на кадр, из них `World.Forward` ≈4.5 мс. Основная цена — трава (тонкие
травинки, перерисовка квадов): `r.Foliage.Grass` и `r.Foliage.Density` — первые ручки. В маленькой сцене примера
главы (320×180) на первом кадре заметны `Vegetation.ImpostorBake` (~1 мс, один раз) и `World.ShadowCascades`.

## Типичные ошибки и подводные камни

- **Забыли `finalizeWorldSnapshot`.** Без него не применятся `TerrainRenderComponent`, не будет ключа IBL неба, а
  небо без `EnvironmentComponent` останется без окружения. Вызывайте после заполнения `WorldSnapshot`.
- **Изменяемая карта высот в снимке.** `TerrainSnapshot::heightfield` должен быть неизменяемым, пока рендерер может
  его читать. Мост gameplay копирует карту на каждую новую версию — на ландшафте 2k мазки кисти стоят несколько мс
  за кадр правки.
- **Одинаковая `version` батча растительности после пересборки.** Рендерер перезаливает экземпляры только при смене
  `version` (и `key` — устойчивый идентификатор батча).
- **Ландшафт и растительность не в тенях прожекторов и точечных источников.** Пока только каскады солнца; и они не
  участвуют в GPU-driven отсечении ([глава 27](27-gpu-driven-performance.md)) — у `WorldGeometry` свои пассы.
- **Импосторы «плывут» на первом кадре.** Они печатаются в кадре при первом появлении прототипа; один кадр дерево
  рисуется мешем.
- **Трещины тесселяции.** Внутри патча возможны точечные «проколы» (порядок барицентрик); тени не тесселируются.
- **DQS без compute.** `r.Skinning.Compute 0` откатывает все меши на линейный скиннинг в вершинном шейдере.
- **Воздушная перспектива «пропала».** Включён объёмный туман — она намеренно пропускается.
- **Время pass'а `Skinning` не видно в `RenderStats::passes`.** Скиннинг записывается вне графа вида; судите по
  `ISkinnedOutputs` и общему времени кадра.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`features/world/world_skinning.hpp`](../../engine/render/include/oxwald/render/features/world/world_skinning.hpp) | `WorldSnapshot`, `TerrainSnapshot`, `VegetationSnapshot`, `VegetationLayerSnapshot`, `VegetationBatchSnapshot`, `WorldSkySnapshot`, `finalizeWorldSnapshot`, `worldSkyIblKey`, `SkinningSnapshot`, `GpuSkinningMethod`, `ISkinnedOutputs`, `SkinnedOutputs`, `kSkinnedVerticesResource`, `registerWorldSkinningFeatures` |
| [`components/world.hpp`](../../engine/render/include/oxwald/render/components/world.hpp) | `TerrainRenderComponent`, `VegetationPrototypesComponent`, `VegetationPrototypeDesc`, `registerWorldSkinningTypes` |
| [`gpu_types.hpp`](../../engine/render/include/oxwald/render/gpu_types.hpp) | `GpuSkinnedVertex` |
| [`snapshot.hpp`](../../engine/render/include/oxwald/render/snapshot.hpp) | `SnapshotMesh` (палитры), `extract`, `ExtractOptions::services` |
| [`gameplay/world.hpp`](../../engine/gameplay/world/include/oxwald/gameplay/world.hpp) | компоненты мира, `WorldRuntime`, `WorldRenderData`, `addWorldSystems` |
| [`gameplay/world/render_data.hpp`](../../engine/gameplay/world/include/oxwald/gameplay/world/render_data.hpp) | `WorldRenderData`, `TerrainRenderItem`, `VegetationRenderItem`, `SkyRenderData` |

Шейдеры: `engine/shaders/render/world/`, общие с CPU включения — `world/wind.glsl`, `world/preetham.glsl`,
`world/cdlod.glsl`. Для разработчиков: [`docs/dev/modules/render.md`](../dev/modules/render.md) (раздел «World:
terrain, vegetation, sky, skinning»), [`docs/dev/modules/world.md`](../dev/modules/world.md),
[`docs/dev/modules/gameplay.md`](../dev/modules/gameplay.md) (раздел World).

## Что дальше

- [16. Открытый мир](16-world.md) — генерация ландшафта, кисти, CDLOD, разброс, время суток, стриминг.
- [10. Анимация](10-animation.md) — скелеты, клипы, палитры скиннинга.
- [22. Объёмный туман и облака](22-volumetrics.md) — атмосфера поверх неба мира.
- [23. Прозрачность, вода и частицы](23-transparency-water-particles.md) — вода `WaterComponent` в кадре.
- [26. Качество графики](26-quality-settings.md) — группы Foliage, Shading, ViewDistance.
- [32. Компоненты ECS](32-gameplay-components.md) — полный справочник компонентов мира.
- [Оглавление](README.md)
