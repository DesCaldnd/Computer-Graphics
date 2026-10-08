# 27. GPU-driven рендеринг и производительность

> Модуль `render` (таргет `Oxwald::render`, пространство имён `ox::render`, зонтичный заголовок
> `<oxwald/render/render.hpp>`); помощники области — `<oxwald/render/features/gpu_driven/gpu_driven.hpp>`, статистика
> — `<oxwald/render/render_stats.hpp>`. Всё описанное включено по умолчанию; глава объясняет, что происходит, какие
> ручки есть и как измерять.

## Зачем

Классический рендерер тратит CPU на каждый объект: отсечение, выбор LOD, по draw call на батч. На десятках тысяч
объектов CPU становится узким местом раньше GPU. В Oxwald отсечение, выбор LOD и формирование команд отрисовки
выполняет **GPU** (compute-шейдеры), а CPU лишь отправляет несколько indirect-вызовов. Плюс:

- **двухфазное отсечение перекрытых объектов по Hi-Z** — то, что закрыто стеной, не рисуется;
- **мешлеты** — отсечение кусков меша по 64–128 треугольников (опционально);
- **async compute** — отсечение света и Hi-Z на отдельной очереди, где это выгодно;
- **стриминг мипов** — текстуры держат в памяти столько деталей, сколько видно, в рамках бюджета;
- **параллельный extract и запись команд** на job system;
- **профилирование**: время каждого пасса на GPU, счётчики отсечения, VRAM, Tracy-зоны.

Пример выигрыша (стресс-сцена, Apple M4 Pro, 1080p, 50 000 экземпляров): CPU-время рендерера **11.4 → 2.1 мс**,
GPU-кадр **31.1 → 24.0 мс**, draw calls **1418 → 32**.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| Draw set | Постоянные батчи (корзина, вариант пайплайна, меш, материал) с одной indirect-командой на LOD. Перестраиваются только при изменении состава сцены (`GpuScene::structureVersion()`), а не каждый кадр |
| Задание отсечения (cull job) | Один вид: камера, каскад, прожектор, грань куба. Все задания пасса — один dispatch на стадию (RESET → CULL → PREFIX → SCATTER) |
| Hi-Z | Пирамида минимальной глубины (R32F). `HiZ.Early` строится по глубине фазы 1 в половинном разрешении |
| Двухфазное отсечение | Фаза 1 рисует видимое в прошлом кадре, фаза 2 (`GpuCull.Late`) проверяет остальное по Hi-Z и дорисовывает ставшее видимым — **в том же кадре** |
| Экранная ошибка LOD | Выбирается самый грубый LOD, чья ошибка упрощения на экране ≤ `r.GpuDriven.LODErrorPixels · 2^LODBias` пикселей |
| Мешлет | Кусок меша с конусом нормалей и сферой; отсекается по фрустуму, «спиной к камере» и Hi-Z |
| Async compute | Пассы на отдельной compute-очереди; render graph сам расставляет семафоры между очередями |
| Стриминг мипов | Текстура стартует с «хвоста» ≤ 64 px, недостающие мипы догружаются по плотности текселей на экране |
| `RenderStats` | Статистика последнего кадра: время пассов, draw calls, счётчики отсечения, стриминг, VRAM |

Порядок кадра (упрощённо, полный — в [главе 18](18-rendering-overview.md)):

```
SceneUpload → GpuCull (фаза 1: видимые в прошлом кадре) → DepthPrepass
           → HiZ.Early → GpuCull.Late (фаза 2: всё остальное против Hi-Z) → DepthPrepass.Late
           → HiZ, LightCulling (могут уйти на async compute) → Shadows (GpuCull по каскадам/источникам)
           → ForwardOpaque (обе фазы) → …
```

## Шаг 1. GPU-driven путь и его ручки

Ничего включать не нужно: `r.GpuDriven` по умолчанию `true`. CPU-путь оставлен запасным (и включается сам, если
пайплайн отсечения недоступен) — удобно для сравнения.

```cpp
const ox::render::CameraParams cam = camera({0, 1.5f, 10}, {0, 1.5f, 0});
{
    guide::CVarOverride off("r.GpuDriven", "false");   // CPU-путь: по draw call на батч
    render(cam, 256, 192, 4);
}
render(cam, 256, 192, 4);                               // GPU-driven
const ox::render::RenderStats& s = renderer->stats();
s.gpuDriven;                                            // true
s.indirectDrawCalls;                                    // > 0
const ox::render::GpuCullingStats& g = s.gpuCulling;    // читается с опозданием на g.latencyFrames кадров
g.instancesTested; g.instancesFrustumCulled; g.instancesOccluded; g.instancesVisible;
```

Полный пример: `samples/guide_examples/27-gpu-driven-performance/gpu_driven.cpp` (сцена: стена, 200 кубиков за ней и
10 сфер перед ней; тест проверяет, что Hi-Z отбросил ≥ 150 кубиков). `guide::RenderScene` и `guide::CVarOverride` —
маленький headless-стенд из `guide_render_scene.hpp` рядом с примером.

| CVar | По умолч. | Группа: Low / Medium / High / Ultra | Смысл |
| --- | --- | --- | --- |
| `r.GpuDriven` | true | — | GPU-отсечение и indirect-отрисовка; `false` — CPU-путь |
| `r.GpuDriven.Occlusion` | true | — | двухфазное отсечение по Hi-Z для камер |
| `r.GpuDriven.Shadows` | true | — | GPU-отсечение для каскадов и источников (по фрустуму/сфере, без Hi-Z) |
| `r.GpuDriven.LODErrorPixels` | 1 | ViewDistance: 2 / 1.5 / 1 / 0.75 | допустимая экранная ошибка LOD |
| `r.ViewDistance.LODBias` | 0 | ViewDistance: 1 / 0.5 / 0 / −0.5 | +1 = порог ×2 (переход на грубый LOD вдвое ближе) |
| `r.ViewDistance.DrawDistance` | 0 | ViewDistance: 400 / 1000 / 2500 / 0 | дальность отрисовки экземпляров, м |
| `r.GpuDriven.Meshlets` | false | — | отсечение мешлетов (нужны мешлеты в ассете) |
| `r.GpuDriven.MeshShaders` | false | — | task/mesh-шейдеры вместо compute + index buffer (только где есть VK_EXT_mesh_shader) |
| `r.GpuDriven.MeshletIndexBudgetMB` | 64 | — | буфер индексов мешлет-пути |
| `r.GpuDriven.DrawCount` | Auto | — | `drawIndirectCount` где есть, иначе фиксированное число команд с пустыми |
| `r.AsyncCompute` | Auto | — | `Off`/`On`/`Auto` (Auto = выкл. на MoltenVK) |
| `r.ParallelRecording` (+ `.MinBatches` 256) | Auto | — | запись команд CPU-пути на job system (Auto = выкл. на MoltenVK) |

**Цифры** (стресс-сцена `GpuDrivenTest.StressSceneTimings`: поле 200 м, 64 материала, 12 стен, солнце с 4 каскадами
+ 8 точечных теней, 1080p, M4 Pro):

| 50 000 экземпляров | GPU кадр, мс | CPU рендерера, мс | draw calls | видимых экз. |
| --- | --- | --- | --- | --- |
| CPU-отсечение + instancing (`r.GpuDriven 0`) | 31.09 | 11.42 | 1418 | 36 829 |
| GPU-отсечение (фрустум + LOD) | 31.68 | 1.95 | 28 | 36 829 |
| + двухфазный Hi-Z (**по умолчанию**) | **24.03** | **2.12** | 32 | 14 601 |
| + мешлеты | 23.87 | 2.20 | 48 | 14 601 |

Стоимость самого отсечения на 50k: `GpuCull` 0.14 мс (камера + 4 каскада + 8 источников), `HiZ.Early` 0.09,
`GpuCull.Late` 0.07 мс. На маленькой сцене (400 экземпляров) двухфазный проход стоит ~0.15 мс и почти ничего не
отсекает — он окупается от нескольких тысяч объектов.

**LOD.** Функция `selectLod` общая для CPU-списков и шейдера отсечения — её удобно использовать в инструментах и
тестах:

```cpp
ox::render::LodSelection sel;
sel.projScale = 0.5f * 1080.0f / std::tan(glm::radians(60.0f) * 0.5f); // пикселей на метр на расстоянии 1 м
sel.thresholdPixels = 1.0f;                                             // r.GpuDriven.LODErrorPixels · 2^LODBias
const ox::u32 lod = ox::render::selectLod(lods, 3, /*instanceScale*/ 1.0f, /*distance*/ 12.0f, sel);
// LOD l выбирается, пока lods[l].error · scale · projScale / distance ≤ thresholdPixels
```

Полный пример: `samples/guide_examples/27-gpu-driven-performance/lod_and_queues.cpp`. Цепочки LOD и мешлеты создаёт
импорт мешей ([глава 31](31-assets.md)); у меша без LOD всегда LOD 0. Тени используют LOD камеры.

**Для авторов фич.** Свои списки отрисовки (тени, захваты, планарные отражения) берите через
`FeatureContext::cullDrawList(filter, instanceMultiplier)` — отсечение пойдёт на GPU, когда возможно, иначе на CPU —
и рисуйте **только** через `drawBatches()`, который понимает и обычные, и indirect-списки. `drawLists()` по-прежнему
отдаёт CPU-списки всех четырёх корзин (строятся лениво). См. [главу 18](18-rendering-overview.md).

## Шаг 2. Async compute

Пассы, у которых все ресурсы объявлены в render graph, можно отправить на отдельную compute-очередь — граф сам
вставит семафоры и передачу владения. Встроенные async-пассы: `LightCulling` и `HiZ`. Своя фича делает так:

```cpp
#include <oxwald/render/features/gpu_driven/gpu_driven.hpp>

const ox::rhi::QueueType queue = ox::render::asyncComputeHint(ctx.caps()); // Compute или Graphics
```

Результат передайте подсказкой очереди своему compute-пассу: `graph.addPass(...)` → `PassBuilder::queue(queue)`
(так объявлены встроенные `HiZ` и `LightCulling`; render graph — [глава 17](17-rhi-vulkan.md)).

| `r.AsyncCompute` | Отдельная compute-очередь есть | MoltenVK (portability) | Нет очереди |
| --- | --- | --- | --- |
| `Off` | Graphics | Graphics | Graphics |
| `On` | Compute | Compute | Graphics |
| `Auto` (по умолч.) | Compute | **Graphics** | Graphics |

Почему `Auto` выключен на Mac: Metal сериализует очереди (отслеживание хазардов по bindless-куче), а межочередные
семафоры только добавляют задержку. Замер `AsyncComputeOverlapsGraphics` (1280×720): sync 2.95–3.85 мс против async
3.14–4.75 мс, перекрытие 0.000 мс. Не async: `GpuCull`, `GpuCull.Late`, `HiZ.Early` — у них неотслеживаемые
indirect-буферы и они на критическом пути depth prepass. Перекрытие измеряется: `RenderStats::asyncComputeMs`,
`asyncOverlapMs`, `gpuFrameWallMs`, у пасса — `PassTiming::asyncCompute`.

## Шаг 3. Стриминг мипов

Текстуры стартуют с «хвоста» мипов ≤ `r.Streaming.TailSize` (64 px). Каждый кадр стример оценивает нужный мип по
экранной плотности текселей (ограничивающая сфера × тайлинг UV), укладывается в бюджет, огрубляя наименее важные
текстуры, и догружает мипы асинхронно; подмена происходит кадром позже — **без ожиданий**.

```cpp
guide::CVarOverride pool("r.Streaming.PoolSizeMB", "2");     // бюджет (в игре — группа Textures)
renderer->resources().addTexture(id, textureData);           // полная цепочка мипов в CPU-копии
renderer->resources().residentTexture(id)->width;            // ≤ 64 до первого кадра
render(cam, 640, 360, 10);
const ox::render::TextureStreamingStats& st = renderer->stats().streaming;
// st.streamedTextures, st.residentBytes ≤ st.budgetBytes, st.wantedBytes, st.pendingRequests, st.mipsLoaded/Evicted
```

Полный пример: `TextureStreamingBudget` в `gpu_driven.cpp`. Замер движка (`TextureStreamingRespectsBudget`): 24 текстуры
1024² (128 МиБ с мипами), бюджет 4 МиБ — нужно 12.6 МиБ, резидентно 3.4 МиБ, ближняя текстура 512 px, дальняя 64 px;
после увеличения бюджета ближняя за несколько кадров доходит до полного размера.

| CVar | По умолч. | Смысл |
| --- | --- | --- |
| `r.Streaming` | true | стриминг включён |
| `r.Streaming.PoolSizeMB` | 1024 | бюджет; группа Textures: 256 / 512 / 1024 / 2048 |
| `r.Streaming.TailSize` | 64 | мипы до этого размера всегда резидентны |
| `r.Streaming.MipBias` | 0 | сдвиг нужного мипа (+ = мутнее, меньше памяти) |
| `r.Streaming.MaxUploadMBPerFrame` | 32 | лимит загрузки за кадр |
| `r.Streaming.DropDelayFrames` | 30 | сколько кадров текстура должна «хотеть меньше», прежде чем мипы выгрузятся |

Ограничения: эвристика по расстоянию, без GPU-feedback-пасса; данные берутся из CPU-копии, которую держит стример.

## Шаг 4. CPU: extract, параллельная запись, асинхронные PSO

```cpp
ox::JobSystem jobs(4);
renderer = ox::render::Renderer::create(*device, {.jobs = &jobs});  // загрузка ассетов, PSO, запись команд
ox::render::extract(world, snapshot, {.jobs = &jobs});              // parallelFor от parallelThreshold мешей
guide::CVarOverride parallel("r.ParallelRecording", "On");          // вторичные command lists на job system
renderer->stats().parallelRecordedChunks;                           // > 0
```

Полный пример: `JobsForExtractAndParallelRecording` в `gpu_driven.cpp`. В игре job system передаёт рантайм.

| Механизм | Когда выгоден (M4 Pro) |
| --- | --- |
| Параллельный extract (`ExtractOptions::jobs`, `parallelThreshold = 32768`) | extract упирается в память; параллельный выигрывает от ~30–50k мешей: 50k — 0.72–0.94 мс serial против 0.51–0.84 мс |
| `r.ParallelRecording` | на MoltenVK **только добавляет работу** (вторичные списки перекодируются в Metal последовательно): 1396 draws — 1.67 мс serial против 3.38 мс; на нативном Vulkan — включайте `On` и меряйте |
| Асинхронные PSO | пайплайны мешлетов и mesh shaders компилируются на job system; пока не готовы — рисует instanced-путь (`RenderStats::pipelinesCompiling`). Постоянный кэш: `DeviceDesc::pipelineCachePath` |
| Без аллокаций | extract и списки камеры в установившемся режиме не выделяют память (тест со счётчиком `operator new`); кадр целиком — ~1170 (CPU-путь) / ~1990 (GPU-путь) аллокаций, в основном объявления render graph |

## Шаг 5. Профилирование

**RenderStats** — всё, что показывает оверлей статистики, доступно из кода (`renderer->stats()`, последний
завершённый кадр; GPU-времена отстают на число кадров в полёте):

```cpp
const ox::render::RenderStats& s = renderer->stats();
std::printf("%s", s.toString().c_str());     // сводка: GPU/CPU мс, draws, треугольники, экземпляры, тени, VRAM
std::vector<ox::render::PassTiming> passes = s.passes;   // {name, gpuMs, startMs, asyncCompute}
std::sort(passes.begin(), passes.end(), [](auto& a, auto& b) { return a.gpuMs > b.gpuMs; });
for (const ox::render::VramCategory& c : s.vram) { /* c.name, c.bytes: геометрия, текстуры, буферы GPU-driven, цели */ }
```

| Поле `RenderStats` | Смысл |
| --- | --- |
| `gpuFrameMs`, `gpuFrameWallMs` | сумма пассов верхнего уровня / от первого до последнего timestamp (async учтён один раз) |
| `cpuRenderMs` | CPU рендерера: setup + запись команд |
| `asyncComputeMs`, `asyncOverlapMs` | пассы на compute-очереди и их перекрытие с графикой |
| `drawCalls`, `indirectDrawCalls`, `indirectCommands` | API-вызовы (indirect multi-draw — один), из них indirect, отправленные команды (с пустыми) |
| `triangles`, `instances`, `visibleInstances` | треугольники, экземпляры GPU-сцены, видимые после отсечения |
| `lights`, `shadowedLights`, `shadowMapsRendered`, `shadowMapsCached` | свет и кэш теней |
| `renderGraphPasses`, `renderGraphCompiles`, `featuresEnabled` | пассов, перестроений графа в этом кадре (≠ 0 после смены cvar'ов), включённых фич |
| `vramUsageBytes`, `vramBudgetBytes`, `vram`, `textures`, `buffers`, `geometryBytes`, `pendingAssetLoads` | память |
| `gpuDriven`, `gpuCulling`, `streaming`, `pipelinesCompiling`, `parallelRecordedChunks` | состояние областей этой главы |

Пример вывода `toString()` и самых дорогих пассов для сцены со стеной (256×192, M4 Pro):

```
frame 7: GPU 0.71 ms, CPU 0.35 ms, 1 views, 4 draws, 9716 tris, 13/211 instances, 1 lights (0 shadowed, …), 13 passes (0 graph compiles), …
  gpu culling (2 frames late): 211 tested, 0 frustum, 198 occluded, 13 visible, 3 draws, 9716 tris, avg LOD 0.00; …
  indirect: 4 calls, 12 commands
  ForwardOpaque 0.309 ms · HiZ.Early 0.138 ms · GpuCull.Late 0.075 ms · GpuCull 0.069 ms · DepthPrepass 0.066 ms
```

GPU-времена приходят из timestamp'ов render graph и отстают на число кадров в полёте (2): рендерьте 3+ кадра,
прежде чем их читать. Пассы, записанные вне графа вида (например, compute-скиннинг), в `passes` не попадают.

**Отладочный оверлей** (модуль `ui`, [глава 29](29-ui.md)) рисует эти же данные: окно Stats (кадр, проходы графа,
таблица GPU-пассов по убыванию времени), окно Render graph, cvar'ы по группам. Cvar `ui.ShowStats 1` оставляет
компактный HUD в углу и при скрытом оверлее. Консольная команда движка `stats` печатает время игры/рендера.

**Tracy.** При сборке с `OX_ENABLE_TRACY=ON` (и найденном пакете Tracy) каждый шаг рендерера — CPU-зона
(`OX_PROFILE_ZONE`), а каждый пасс render graph — GPU-зона. Подключитесь программой Tracy Profiler к запущенной игре.
Свои зоны — макросы из `<oxwald/core/profile.hpp>` ([глава 01](01-core.md), GPU-зоны — [глава 17](17-rhi-vulkan.md)).
Захват кадра GPU: RenderDoc на Windows/Linux (`rhi::RenderDocCapture`), на macOS — Xcode (`MTL_CAPTURE_ENABLED=1`).

**Правило измерений** (из `docs/dev/perf.md`): каждая оптимизация начинается с замера и заканчивается замером;
усредняйте 8+ кадров после прогрева; на MoltenVK времена соседних compute-пассов «размазываются» (±0.1 мс), а
шум между запусками на тяжёлой сцене — около ±1 мс.

### Чек-лист «кадр тормозит»

| Симптом в `RenderStats` | Что крутить |
| --- | --- |
| высокий `cpuRenderMs`, `gpuDriven == false` | проверьте `r.GpuDriven` и что пайплайн отсечения скомпилировался |
| `gpuCulling.instancesOccluded` ≈ 0 в закрытой сцене | `r.GpuDriven.Occlusion`; крупные окклюдеры должны быть непрозрачными мешами |
| дорогие `Shadow.Cascades` | группа Shadows (`r.Shadows.CSM.Cascades`, `.Distance`, `.Resolution`), `r.Shadows.Caching` для локальных источников |
| дорогой `ForwardOpaque` при большом `triangles` | LOD в ассетах, `r.GpuDriven.LODErrorPixels`/группа ViewDistance, мешлеты |
| `renderGraphCompiles` > 0 каждый кадр | кто-то меняет cvar или `isEnabled()` фичи каждый кадр |
| `streaming.wantedBytes` ≫ `budgetBytes`, мыло вблизи | поднять `r.Streaming.PoolSizeMB` (группа Textures) или `r.Textures.MaxSize` вниз |
| `pendingAssetLoads` долго > 0 | передайте `RendererDesc::jobs`, проверьте ассеты |

## Типичные ошибки и подводные камни

- **Сравнение `drawCalls` между путями.** Indirect multi-draw считается одним вызовом; число реальных команд —
  `indirectCommands` (включая пустые: на MoltenVK нет `drawIndirectCount`, команды идут фиксированным числом с
  нулевыми экземплярами, ≈ 20–60 нс на пустую).
- **Счётчики отсечения «отстают».** `GpuCullingStats` читается с GPU через `latencyFrames` кадров. Проверяйте `valid`
  и рендерьте несколько кадров перед чтением.
- **Hi-Z на маленьких сценах.** Двухфазный проход стоит ~0.15 мс; если перекрытий нет, он ничего не даст. Отключать
  его не обязательно — он окупается от нескольких тысяч объектов.
- **Тени не отсекаются по Hi-Z.** Каскады растеризуют все затеняющие объекты внутри каскада — в плотных сценах тени
  становятся самым дорогим пассом. Используйте группу Shadows и `castShadows` у мелочи.
- **`r.AsyncCompute On` / `r.ParallelRecording On` на Mac.** Медленнее, чем выключено. `Auto` выбирает правильно.
- **Своя фича рисует списки вручную.** Indirect-списки рисуются только через `drawBatches()`; самодельный цикл по
  `batches` на GPU-пути ничего не нарисует.
- **Ландшафт и растительность — вне GPU-driven пути.** `WorldGeometry` рисует свои пассы и своё отсечение
  растительности ([глава 28](28-world-rendering.md)); счётчики `gpuCulling` их не включают.
- **Замеры в Debug и с validation.** Таблицы выше — RelWithDebInfo с validation; в Release CPU-цифры ниже.
  Сравнивайте только сопоставимые сборки.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`render_stats.hpp`](../../engine/render/include/oxwald/render/render_stats.hpp) | `RenderStats`, `PassTiming`, `GpuCullingStats`, `TextureStreamingStats`, `VramCategory` |
| [`gpu_driven.hpp`](../../engine/render/include/oxwald/render/features/gpu_driven/gpu_driven.hpp) | `asyncComputeHint`, `registerGpuDrivenFeatures` |
| [`gpu_types.hpp`](../../engine/render/include/oxwald/render/gpu_types.hpp) | `GpuMeshLod`, `LodSelection`, `selectLod`, `kMaxMeshLods` |
| [`gpu_scene.hpp`](../../engine/render/include/oxwald/render/gpu_scene.hpp) | `GpuScene`: `structureVersion`, `meshLods`, `instanceLod` |
| [`render_feature.hpp`](../../engine/render/include/oxwald/render/render_feature.hpp) | `FeatureContext::cullDrawList`, `drawBatches`, `DrawFilter`, `DrawList` |
| [`snapshot.hpp`](../../engine/render/include/oxwald/render/snapshot.hpp) | `extract`, `ExtractOptions::jobs`, `parallelThreshold` |
| [`renderer.hpp`](../../engine/render/include/oxwald/render/renderer.hpp) | `RendererDesc::jobs`, `Renderer::stats` |
| [`gpu_resource_cache.hpp`](../../engine/render/include/oxwald/render/gpu_resource_cache.hpp) | `residentTexture`, `addTexture`, хук стриминга |
| [`core/profile.hpp`](../../engine/core/include/oxwald/core/profile.hpp), [`rhi/profiling.hpp`](../../engine/rhi/include/oxwald/rhi/profiling.hpp) | `OX_PROFILE_*`, `GpuTiming`, `GpuMemoryStats`, `RenderDocCapture` |

Журнал замеров и методика: [`docs/dev/perf.md`](../dev/perf.md); детали для разработчиков —
[`docs/dev/modules/render.md`](../dev/modules/render.md) §13.

## Что дальше

- [17. RHI и Vulkan](17-rhi-vulkan.md) — render graph, очереди, timestamps, Tracy GPU-зоны.
- [18. Обзор рендеринга](18-rendering-overview.md) — фичи и `FeatureContext`.
- [26. Качество графики](26-quality-settings.md) — группы ViewDistance, Textures, Shadows и их цена.
- [28. Рендеринг мира](28-world-rendering.md) — ландшафт и растительность со своим отсечением.
- [31. Ассеты](31-assets.md) — LOD и мешлеты при импорте мешей.
- [Оглавление](README.md)
