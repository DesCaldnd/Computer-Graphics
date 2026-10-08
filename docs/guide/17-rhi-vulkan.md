# 17. RHI и Vulkan

> Глава для разработчиков движка: тех, кто пишет рендерер, GPU-инструменты, бейкеры и новые проходы кадра. Геймплейному программисту RHI напрямую не нужен: рендерер (глава *скоро*) прячет его за сценой, материалами и камерами.

## Зачем

RHI (Render Hardware Interface, модуль `rhi`, таргет `Oxwald::rhi`, пространство имён `ox::rhi`) — тонкий слой над Vulkan 1.3. Он не пытается спрятать Vulkan целиком: форматы, layout'ы, blend-факторы остаются Vulkan-перечислениями (`VkFormat`, `VkImageLayout`…). Зато он берёт на себя всё, что в «голом» Vulkan пишут заново в каждом проекте и в чём чаще всего ошибаются:

- создание instance/device/очередей, выбор GPU и определение возможностей (`DeviceCaps`);
- память (VMA), отложенное удаление ресурсов, generational handles вместо сырых указателей;
- **bindless**: один глобальный набор дескрипторов, буферы через device address — никаких descriptor set'ов на каждый материал;
- барьеры synchronization2 по декларативному описанию «как используется ресурс» (`Access`);
- **render graph**: пассы объявляют ресурсы, граф сам расставляет барьеры, отбрасывает лишнее, переиспользует память и раскидывает работу по очередям;
- компиляцию GLSL → SPIR-V (shaderc) с кэшем и **hot reload** шейдеров;
- swapchain, трассировку лучей (BLAS/TLAS), профилирование (timestamps, Tracy, RenderDoc).

На macOS Vulkan работает поверх Metal через MoltenVK; движок везёт загрузчик, MoltenVK и validation layer с собой, так что Vulkan SDK ставить не нужно.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `Device` | Единственный объект «на GPU»: очереди, VMA, bindless-куча, кэш пайплайнов, компилятор шейдеров, кадры, загрузки. Одновременно может существовать только один (volk хранит глобальные указатели на функции). |
| `DeviceCaps` | Что умеет выбранный GPU: очереди, RT, mesh-шейдеры, сжатие текстур, лимиты bindless, timestamps. Заполняется один раз при создании. |
| Handle | `BufferHandle`, `TextureHandle`, `SamplerHandle`, `PipelineHandle`, `AccelStructHandle` — индекс слота + поколение (generation). Устаревший handle никогда не указывает на новый объект. |
| `Access` | Как ресурс используется (`SampledFragment`, `StorageWriteCompute`, `ColorAttachmentWrite`, `Present`…). Из него выводятся stage, access mask и layout для барьера. |
| `CommandList` | Обёртка над `VkCommandBuffer`: барьеры, dynamic rendering, draw/dispatch, копирования, метки, timestamps. |
| Пайплайн | Graphics / compute / ray tracing. Handle переживает hot reload: меняется только `VkPipeline` внутри. |
| Bindless | Set 0: текстуры (binding 0), storage-изображения (1), сэмплеры (2). Плюс 128 байт push constants для всех стадий. Буферы — только по адресу (buffer device address, BDA). |
| `RenderGraph` | Граф кадра: ресурсы и пассы → `RenderGraphPlan` (порядок, барьеры, aliasing, пачки по очередям) → исполнение. |
| `TimelinePoint` | Точка на timeline-семафоре очереди. `submit()` возвращает её, по ней можно ждать или строить межочередные зависимости. |

## Шаг 1. Устройство и DeviceCaps

```cpp
#include <oxwald/rhi/device.hpp>
using namespace ox::rhi;

DeviceDesc desc;
desc.appName = "MyTool";
desc.validation = true;      // nullopt: вкл. в Debug/RelWithDebInfo, выкл. в Release; env OX_VULKAN_VALIDATION=0/1
desc.surface = nullptr;      // headless: тесты, инструменты, офлайн-бейкинг
desc.framesInFlight = 2;     // 2..3
desc.pipelineCachePath = "cache/pipelines.bin"; // постоянный VkPipelineCache (пусто — только в памяти)

std::string error;
std::unique_ptr<Device> device = Device::create(desc, &error);
if (!device) { OX_LOG_ERROR("vulkan: {}", error); return; }

const DeviceCaps& caps = device->caps();
OX_LOG_INFO("{}", caps.toString());          // полный отчёт о GPU
if (!caps.rayTracingSupported()) {
    // Человекочитаемая причина — для подсказки у выключенной галочки в UI.
    ui.disableRtCheckbox(caps.whyRayTracingUnavailable());
}
```

*Пример: [`device.cpp`](../../samples/guide_examples/17-rhi-vulkan/device.cpp), тест `CapsAndFeatureDetection`.*

Правило: **проверяйте возможности по `DeviceCaps`, а не по платформе**. Полезные поля:

| Поле | Зачем |
| --- | --- |
| `asyncComputeQueue`, `dedicatedTransferQueue` | есть ли отдельные очереди (async compute, загрузки на transfer-очереди) |
| `rayTracingSupported()` (= `accelerationStructure && rayQuery`), `rayTracingPipeline` | RT-эффекты |
| `meshShader`, `taskShader`, `geometryShader`, `drawIndirectCount` | выбор пути GPU-driven рендеринга |
| `shaderOutputLayer`, `multiview` | слоистый рендер без геометрических шейдеров (кубмапы, каскады теней) |
| `textureCompressionBC/ASTC/ETC2` | какой формат текстур грузить |
| `timestampQueries`, `timestampPeriodNs` | GPU-тайминги |
| `portabilitySubset`, `unifiedMemory` | признаки MoltenVK / Apple Silicon |

Поля `DeviceDesc::enableRayTracing`, `enableMeshShaders`, `asyncCompute`, `asyncTransfer` позволяют *не* включать фичу, даже если она есть, — удобно для проверки фолбэков.

## Шаг 2. Ресурсы и handles

Все объекты живут в пулах `Device` и выдаются как handles. `destroy(handle)` не удаляет объект сразу: он попадает в очередь отложенного удаления и уничтожается, когда GPU закончит все отправки, которые могли его использовать. Bindless-индекс освобождается в тот же момент.

```cpp
// Буфер на GPU с начальными данными (синхронная загрузка через staging).
BufferHandle vb = device->createBuffer({sizeof(verts), BufferUsage::Storage, MemoryUsage::GpuOnly, "mesh.vb"}, verts);

TextureDesc td;
td.width = 1024; td.height = 1024;
td.mipLevels = 0;                                     // 0 = полная цепочка мипов
td.format = VK_FORMAT_BC7_SRGB_BLOCK;
td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
td.name = "rock.albedo";                              // имя видно в RenderDoc/Xcode и в сообщениях validation
TextureHandle albedo = device->createTexture(td);
device->uploadTexture(albedo, pixels);                // все мипы подряд, плотно упакованы

device->destroy(vb);                                  // отложенно; vb теперь «мёртвый»
assert(!device->isAlive(vb));
```

Как устроены handles, видно на CPU-примере с тем же пулом, который использует `Device`:

```cpp
HandlePool<std::string, TextureTag> pool;
TextureHandle albedo = pool.allocate("albedo");
pool.release(albedo);                           // как Device::destroy
TextureHandle normal = pool.allocate("normal"); // тот же слот, другое поколение
assert(normal.index == albedo.index && normal.generation != albedo.generation);
assert(pool.get(albedo) == nullptr);            // старый handle ничего не находит
```

*Пример: [`handles.cpp`](../../samples/guide_examples/17-rhi-vulkan/handles.cpp) — пул, `IndexAllocator` для bindless-слотов, отображение `Access` → барьер, таблица форматов (`formatInfo`, `mipLevelSize`, `fullMipCount`).*

`MemoryUsage` выбирает тип памяти:

| `MemoryUsage` | Когда |
| --- | --- |
| `GpuOnly` | всё, что только читает/пишет GPU; `mapped()` вернёт `nullptr` |
| `Upload` | staging, CPU пишет последовательно |
| `Readback` | CPU читает результаты GPU |
| `Dynamic` | CPU пишет каждый кадр, GPU читает (на Apple Silicon/ReBAR — прямо в видеопамять) |

Синхронные `writeBuffer`/`readBuffer`/`uploadTexture`/`readTexture` хороши для загрузки, инструментов и тестов. В кадре используйте `uploadBufferAsync`/`uploadTextureAsync`: данные идут через кольцевой staging-буфер (`DeviceDesc::stagingRingSize`, 32 МиБ) на transfer-очереди, а следующий graphics-submit сам дождётся копии и передаст владение ресурсом между семействами очередей.

## Шаг 3. Bindless и шейдеры движка

В шейдерах движка нет `layout(set=…, binding=…)` — всё через `#include <common/bindless.glsl>`:

```glsl
#version 460
#include <common/bindless.glsl>
layout(local_size_x = 64) in;
OX_BUFFER(Values, { uint v[]; });                              // buffer_reference, scalar layout
OX_PUSH_CONSTANTS({ Values values; uint count; uint scale; });  // ≤ 128 байт, доступно как pc.*
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < pc.count) pc.values.v[i] = i * pc.scale;
}
```

```cpp
ComputePipelineDesc cd;
cd.name = "fill";
cd.shader = ShaderStageDesc::glsl(kFillComp, ShaderStage::Compute, "fill.comp");
cd.pushConstantSize = 16;                      // сверяется с рефлексией шейдера
PipelineHandle fill = device->createComputePipeline(cd);
if (!device->vkPipeline(fill)) OX_LOG_ERROR("{}", device->lastPipelineError());

struct { VkDeviceAddress values; u32 count, scale; } pc{device->address(values), kCount, 3};
device->immediateSubmit([&](CommandList& cmd) {   // записать, отправить, дождаться
    cmd.bindPipeline(fill);                       // заодно биндит bindless-набор
    cmd.pushConstants(pc);
    cmd.dispatchThreads(kCount);                  // число групп считается из local_size
    cmd.bufferBarrier(values, Access::StorageWriteCompute, Access::TransferRead);
});
std::vector<u8> result = device->readBuffer(values);
```

*Пример: [`device.cpp`](../../samples/guide_examples/17-rhi-vulkan/device.cpp), тест `ComputeThroughBufferDeviceAddress`.*

Основные макросы `bindless.glsl`:

| Макрос | Что делает |
| --- | --- |
| `OX_PUSH_CONSTANTS({ ... })` | блок push constants (scalar layout), переменная `pc` |
| `OX_BUFFER(Name, { ... })`, `OX_READONLY_BUFFER` | тип-ссылка на буфер по адресу |
| `OX_SAMPLE_2D(tex, smp, uv)`, `OX_SAMPLE_2D_LOD`, `OX_SAMPLE_CUBE`, `OX_SAMPLE_3D`, `OX_SAMPLE_SHADOW` | выборка по bindless-индексам текстуры и сэмплера |
| `OX_FETCH_2D(tex, coord, lod)` | `texelFetch` |
| `OX_IMAGE_STORE_2D(fmt, img, coord, value)`, `OX_IMAGE_LOAD_2D` | storage-изображения (`rgba8`, `rgba16f`, `rgba32f`, `r32f`, `r32ui`) |
| `OX_SAMPLER_LINEAR_REPEAT` … `OX_SAMPLER_SHADOW` | фиксированные индексы `DefaultSampler` (0–5) |
| `OX_INVALID_INDEX` | «нет текстуры» (`kInvalidBindlessIndex` на C++) |

Индексы на стороне C++: `device->sampledIndex(tex)` (есть у текстур с `TextureUsage::Sampled`), `device->storageIndex(tex, mip)` (создаётся при первом запросе), `device->samplerIndex(device->sampler(desc))` или `defaultSampler(DefaultSampler::LinearClamp)`.

Графический пайплайн использует dynamic rendering: вместо render pass указываются форматы вложений. Вершинные данные по умолчанию читаются из буфера по адресу (vertex pulling), но `vertexBindings`/`vertexAttributes` тоже поддерживаются:

```cpp
GraphicsPipelineDesc gd;
gd.name = "tint";
gd.vertex = ShaderStageDesc::glsl(kFullscreenVert, ShaderStage::Vertex, "fullscreen.vert");
gd.fragment = ShaderStageDesc::glsl(kTintFrag, ShaderStage::Fragment, "tint.frag");
gd.colorFormats = {VK_FORMAT_R8G8B8A8_UNORM};
gd.depthFormat = VK_FORMAT_D32_SFLOAT;
gd.depth = {.test = true, .write = true};        // compare по умолчанию GREATER_OR_EQUAL (reversed-Z)
gd.blend = {BlendState::alpha()};
PipelineHandle tint = device->createGraphicsPipeline(gd);
```

## Шаг 4. Кадр без графа: command lists и submit

```cpp
device->beginFrame();                 // ждёт кадр N - framesInFlight, перерабатывает command lists,
                                      // удаляет отложенное, подменяет перезагруженные пайплайны
CommandList& cmd = device->commandList(QueueType::Graphics, "upload+clear");
cmd.transition(tex, Access::TransferWrite, /*discardContents*/ true);
cmd.clearTexture(tex, ClearColor::rgba(1, 0, 0));
cmd.transition(tex, Access::SampledFragment);     // барьер из отслеживаемого состояния
TimelinePoint done = device->submit(cmd);
device->endFrame();                   // отправляет async-загрузки, собирает Tracy-зоны
device->wait(done);                   // или device->isComplete(done)
```

- `transition()` знает текущее состояние текстуры (device-side tracking) и сам строит барьер; `textureBarrier(tex, src, dst, range)` — явный барьер на поддиапазон (например, один mip).
- `submit(cmd, SubmitInfo{.waits = {computeDone}})` — межочередная зависимость; `SubmitInfo::swapchain` — ожидание acquire и сигнал present.
- `immediateSubmit(fn)` работает и вне кадров — для загрузчиков, инструментов и тестов.

## Шаг 5. Render graph

Вместо ручных барьеров пассы *объявляют*, как они используют ресурсы. Граф:

1. отбрасывает пассы, чей результат не нужен (culling); корни — импортированные ресурсы, `markOutput()` и пассы с `sideEffect()`;
2. считает барьеры sync2 и переходы layout'ов, включая release/acquire между семействами очередей;
3. выводит `usage` транзиентных текстур и **алиасит** память ресурсов с непересекающимся временем жизни;
4. делит пассы на пачки (batch) по очередям с ожиданием по timeline-семафорам;
5. оборачивает каждый пасс в debug label, GPU timestamp и Tracy-зону.

Компиляция — чистый CPU (`compile()` → `RenderGraphPlan`), кэшируется по хэшу топологии, поэтому её удобно тестировать без GPU.

```cpp
RenderGraph graph;                    // живёт между кадрами: кэш плана и транзиентной памяти

// каждый кадр:
graph.reset();
RGTexture backbuffer = graph.importTexture(swapchain->currentTexture(),
                                           device->desc(swapchain->currentTexture()),
                                           {Access::Undefined, Access::Present});   // initial, final
RGTexture depth = graph.createTexture(depthDesc);   // транзиентные: память выделяет граф
RGTexture ao    = graph.createTexture(aoDesc);
RGTexture hdr   = graph.createTexture(hdrDesc);

graph.addPass("DepthPrepass").depth(depth)          // CLEAR, reversed-Z: очистка в 0
    .execute([&](PassContext& ctx) { drawOpaqueDepth(ctx.cmd); });

graph.addPass("SSAO", PassType::Compute).queue(QueueType::Compute)   // async compute, если есть
    .read(depth, Access::SampledCompute)
    .overwrite(ao, Access::StorageWriteCompute)
    .execute([&](PassContext& ctx) {
        ctx.cmd.bindPipeline(ssao);
        ctx.cmd.pushConstants(SsaoPC{ctx.sampledIndex(depth), ctx.storageIndex(ao)});
        ctx.cmd.dispatchThreads(w, h);
    });

graph.addPass("Lighting")
    .read(ao, Access::SampledFragment)
    .depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, /*readOnly*/ true)
    .color(hdr)                                      // dynamic rendering начинается автоматически
    .execute([&](PassContext& ctx) { drawLighting(ctx); });

graph.addPass("Tonemap").read(hdr, Access::SampledFragment).color(backbuffer)
    .execute([&](PassContext& ctx) { fullscreen(ctx, tonemap, ctx.sampledIndex(hdr)); });

graph.execute(*device, {.swapchain = swapchain.get()});   // пачки по очередям + submit
// или graph.execute(cmd) — всё в один graphics command list, отправляете сами
```

Способы объявить использование:

| Метод | Смысл |
| --- | --- |
| `read(res, access)` | читает предыдущее содержимое |
| `write(res, access)` | read-modify-write: зависит от предыдущего писателя |
| `overwrite(res, access)` | перезаписывает целиком, старое содержимое не важно |
| `color(tex, load, clear, mip, layer)` | цветовое вложение; `LOAD` = read-modify-write, `CLEAR`/`DONT_CARE` = перезапись |
| `depth(tex, load, clear, readOnly)` | вложение глубины; `readOnly` — тест без записи |
| `resolve(msaa, target)` | MSAA resolve |
| `queue(QueueType::Compute)` | подсказка: async compute |
| `sideEffect()` | никогда не отбрасывать (readback, захват, запись в «чужую» память) |

План можно разглядывать и проверять в тестах — какие пассы отброшены, какие барьеры стоят до и после каждого пасса:

```cpp
const RenderGraphPlan& plan = graph.compile();
// "Debug" никто не читает -> отброшен
EXPECT_EQ(plan.culledPasses, std::vector<u32>{3});
// перед SSAO: depth из DEPTH_ATTACHMENT_OPTIMAL в SHADER_READ_ONLY_OPTIMAL
const RGBarrier* b = findBarrier(plan.passes[1].before, depth.id); // findBarrier — помощник из примера
EXPECT_EQ(b->newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
// после последнего использования backbuffer уходит в PRESENT (RGBarrierKind::Final)
std::cout << plan.dump(graph);                        // текстовый дамп
std::ofstream("frame.dot") << graph.exportGraphviz(); // dot -Tsvg frame.dot > frame.svg
```

*Пример: [`render_graph.cpp`](../../samples/guide_examples/17-rhi-vulkan/render_graph.cpp) — culling и барьеры, aliasing (`RGBarrierKind::Aliasing`, `transientBytesAliased`), async compute (`RGCompileOptions::asyncCompute`, `queueFamilies` → пачки и release/acquire), кэш плана (`compileCount()`). Исполнение на GPU с постоянной текстурой, импортированной через `importTexture(device, tex, finalAccess)`, — [`device.cpp`](../../samples/guide_examples/17-rhi-vulkan/device.cpp), тест `RenderGraphFrames`.*

Внутри `execute` используйте `PassContext`: `ctx.cmd`, `ctx.sampledIndex(t)`, `ctx.storageIndex(t, mip)`, `ctx.address(buf)`, `ctx.extent(t)`, `ctx.texture(t)` — физические ресурсы известны только во время исполнения.

`RGCompileOptions` для отладки: `cull = false` (не отбрасывать пассы), `aliasing = false` (если подозреваете, что баг в переиспользовании памяти).

## Шаг 6. Компилятор шейдеров и hot reload

`ShaderCompiler` компилирует GLSL 4.60 в SPIR-V 1.5 через shaderc, отражает (reflection) результат через spirv-reflect и кэширует SPIR-V на диске. `Device` держит свой экземпляр (`device->shaderCompiler()`, настройки — `DeviceDesc::shaderOptions`), но его можно использовать и отдельно, например в офлайн-инструменте:

```cpp
ShaderCompilerOptions options = ShaderCompilerOptions::defaults(); // engine/shaders уже в include roots
options.cacheDirectory = "cache/spirv";                            // пусто = без дискового кэша
options.globalDefines = {{"TINT", "vec3(1.0, 0.9, 0.8)"}};
ShaderCompiler compiler(options);

ShaderCompileDesc desc;
desc.path = "shaders/tint.frag";           // стадия по расширению: .vert .frag .comp .mesh .rgen .rchit ...
desc.defines = {{"USE_FOG", "1"}};
ShaderCompileResult r = compiler.compile(desc);
if (!r.success) OX_LOG_ERROR("{}", r.errors);  // в сообщениях — имя файла и строка
// r.spirv, r.reflection.pushConstantSize, r.reflection.localSize, r.dependencies (файл + все include), r.fromCache

std::string err;
if (!validateAgainstBindlessLayout(r.reflection, kMaxPushConstantSize, err))
    OX_LOG_ERROR("{}", err);                   // шейдер объявил свои дескрипторы или >128 байт push constants
```

*Пример: [`shader_compiler.cpp`](../../samples/guide_examples/17-rhi-vulkan/shader_compiler.cpp) — inline-исходник с define и рефлексией, ошибки компиляции, несовместимый с bindless шейдер, файлы с include и кэш SPIR-V с инвалидацией при правке include.*

Правила include: `#include "x"` ищет сначала в каталоге включающего файла, потом в корнях; `#include <x>` — только в корнях. Корни: `ShaderCompilerOptions::includeRoots` + `engine/shaders` (в упакованной сборке `<exe>/shaders`, переопределяется переменной `OXWALD_SHADER_DIR`). Ключ кэша учитывает путь, стадию, entry point, define'ы, опции и исходник; запись инвалидируется при изменении любого include.

**Hot reload.** Пайплайн, созданный из файла (`ShaderStageDesc::file(path)`), перезагружается сам: `beginFrame()` раз в `hotReloadPollMs` (200 мс) проверяет mtime/размер главного файла и всех транзитивных include, перекомпилирует затронутые пайплайны и подменяет `VkPipeline` за тем же `PipelineHandle`. Старый пайплайн уходит в отложенное удаление.

```cpp
ComputePipelineDesc cd;
cd.name = "value";
cd.shader = ShaderStageDesc::file(dir / "value.comp");  // из файла -> перезагружаемый
PipelineHandle p = device->createComputePipeline(cd);

// ...правим value.glsl, который включает value.comp...
device->reloadChangedShaders(/*force*/ true);  // обычно это делает beginFrame() сам
device->pipelineVersion(p);                     // 2: handle тот же, VkPipeline новый
```

*Пример: [`device.cpp`](../../samples/guide_examples/17-rhi-vulkan/device.cpp), тест `ShaderHotReload`.*

Если правка ломает компиляцию, ошибка пишется в лог, а работает предыдущая версия. Если шейдер не скомпилировался с самого начала, handle всё равно валиден, `vkPipeline(p) == VK_NULL_HANDLE`, draw/dispatch с ним пропускаются — исправьте файл, и пайплайн «оживёт».

Оптимизация SPIR-V включена в RelWithDebInfo/Release, отладочная информация (`-g`, отладка исходника в RenderDoc) — в Debug/RelWithDebInfo.

## Шаг 7. Swapchain и окно

`Device` ничего не знает об окнах: мост — интерфейс `ISurfaceProvider`. Плеер использует GLFW (`Oxwald::rhi_glfw`, `GlfwSurfaceProvider`), редактор — Qt.

```cpp
#include <oxwald/rhi/glfw_surface.hpp>

initGlfwVulkan();                                 // до glfwInit(): загрузчик Vulkan из движка (MoltenVK на Mac)
glfwInit();
glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
GLFWwindow* window = glfwCreateWindow(1280, 720, "Game", nullptr, nullptr);

GlfwSurfaceProvider surface(window);
DeviceDesc desc;
desc.surface = &surface;
auto device = Device::create(desc);
auto swapchain = Swapchain::create(*device, {.presentMode = PresentMode::Mailbox});

RenderGraph graph;
while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    device->beginFrame();
    if (!swapchain->acquire()) { device->endFrame(); continue; } // окно свёрнуто; resize обрабатывается сам
    graph.reset();
    /* импорт swapchain->currentTexture() и пассы */
    graph.execute(*device, {.swapchain = swapchain.get()});
    swapchain->present();
    device->endFrame();
}
device->waitIdle();
graph.releaseResources(*device);
swapchain.reset();
device.reset();
```

| Настройка | Варианты |
| --- | --- |
| `PresentMode` | `VSync` (FIFO, есть всегда), `Mailbox` (низкая задержка, фолбэк на VSync), `Immediate` (с разрывами, фолбэк Mailbox → VSync); `setPresentMode()` применяется на следующем acquire |
| `SwapchainColor` | `Srgb` (шейдеры пишут линейный цвет), `Unorm`, `Hdr10` (ST.2084), `ScRgb` (RGBA16F) — HDR с фолбэком на `Srgb` |

Живой пример с окном — `tools/rhi_window_smoke` (`--frames 120 --present mailbox --resize`): вращающийся треугольник через render graph и swapchain.

## Шаг 8. Трассировка лучей: BLAS/TLAS

Код RT собирается всегда, а используется только при `caps.rayTracingSupported()`. На неподдерживаемом устройстве `createBlas`/`createTlas`/`createRayTracingPipeline` возвращают невалидный handle и пишут причину в лог.

```cpp
if (device->caps().rayTracingSupported()) {
    BlasDesc bd;
    bd.name = "rock.blas";
    BlasTriangles tri;
    tri.vertexBuffer = vb;            // буферы с BufferUsage::AccelStructInput
    tri.vertexCount = vertexCount;
    tri.indexBuffer = ib;
    tri.indexCount = indexCount;
    bd.geometries.push_back(tri);
    AccelStructHandle blas = device->createBlas(bd);  // строится сразу (+ compaction для статики)

    TlasDesc td;
    td.maxInstances = 4096;
    AccelStructHandle tlas = device->createTlas(td);
    TlasInstance inst;
    inst.blas = blas;
    inst.transform = glm::mat3x4(1.f);               // row-major 3x4
    std::vector<TlasInstance> instances{inst};
    cmd.buildTlas(tlas, instances, /*update*/ false); // каждый кадр; update = true — refit вместо rebuild
    u64 addr = device->accelStructAddress(tlas);      // в GLSL: accelerationStructureEXT(addr)
}
```

*Пример: [`device.cpp`](../../samples/guide_examples/17-rhi-vulkan/device.cpp), тест `RayTracingOnlyWithCaps` (на MoltenVK пропускается).*

Для деформируемых мешей — `BlasDesc::allowUpdate = true` и `CommandList::refitBlas(blas)`. RT-пайплайн (`RayTracingPipelineDesc`) строит таблицу шейдеров (SBT) автоматически, запуск — `cmd.traceRays(w, h)`.

## macOS и MoltenVK

Загрузчик Khronos, MoltenVK и validation layer приходят из vcpkg. `ox_deploy_vulkan_runtime(<target>)` (и `ox_add_gpu_test_env` для тестов) копирует их в `<exe dir>/vulkan/`; `configureVulkanEnvironment()` направляет туда `VK_DRIVER_FILES`/`VK_ADD_LAYER_PATH`, если пользователь не задал их сам. Для инструментов, запущенных откуда угодно, есть запасная копия в `<build>/vulkan-dev`.

Что недоступно на MoltenVK (Apple GPU) и чем заменить:

| Нет | Замена |
| --- | --- |
| Ray tracing (AS, ray query, RT-пайплайны) | растровые фолбэки; галочка RT выключена с текстом `whyRayTracingUnavailable()` |
| Геометрические шейдеры | `gl_Layer` из вершинного шейдера (`caps.shaderOutputLayer`), multiview (`RenderingDesc::viewMask`, `GraphicsPipelineDesc::viewMask`) или пасс на слой |
| Mesh/task-шейдеры | классический vertex pulling + GPU culling в compute |
| `drawIndirectCount` | компактировать на GPU и вызывать `drawIndirect` с максимумом с CPU, или `dispatchIndirect` |
| RenderDoc | Xcode GPU capture (см. ниже) |

Особенности: MoltenVK отдаёт несколько одинаковых семейств очередей, поэтому graphics, compute и transfer оказываются в *разных* семействах — передачи владения ресурсами реально работают и на Mac. `caps.portabilitySubset == true`. Уровень логов MoltenVK по умолчанию — предупреждения и ошибки (`MVK_CONFIG_LOG_LEVEL`). vcpkg переписывает install name MoltenVK и ломает подпись, поэтому deploy-шаг переподписывает библиотеку ad hoc; если macOS убивает процесс при загрузке Vulkan, проверьте именно это.

## Отладка и профилирование

**Validation layers.** Включены в Debug/RelWithDebInfo, выключены в Release, переопределяются `OX_VULKAN_VALIDATION=0/1`. Сообщения идут в лог (категория `vulkan`), счётчики `Device::validationErrorCount()`/`validationWarningCount()` позволяют валить тесты на любую ошибку — так сделаны GPU-примеры этой главы.

**Имена и метки.** Поле `name` в каждом desc становится именем объекта `VK_EXT_debug_utils`. Пассы графа автоматически обёрнуты в метки; для ручных участков:

```cpp
{
    CommandList::ScopedLabel label(cmd, "Shadows/Cascade0");
    cmd.beginTimestamp("Cascade0");
    drawCascade(cmd, 0);
    cmd.endTimestamp();
}
```

**GPU-тайминги.** Каждый пасс графа (при `RGExecuteOptions::timestamps = true`) и каждая пара `beginTimestamp/endTimestamp` попадают в `device->gpuTimings()` — список `GpuTiming{name, milliseconds, depth}` последнего *завершённого* кадра. `device->memoryStats()` даёт бюджеты и использование памяти VMA по кучам — для оверлея статистики.

**Tracy.** При сборке с `OX_ENABLE_TRACY` каждый пасс render graph на graphics-очереди становится GPU-зоной Tracy (`TracyVulkan.hpp` через указатели volk). Контекст Tracy создаётся лениво — только когда профайлер подключился (на MoltenVK его пул в 64K timestamp-запросов эмулируется и стоит дорого). Сырой контекст доступен через `device->tracyGpuContext()` (`TracyVkCtx` или `nullptr`). CPU-зоны и сам Tracy описаны в [главе 01](01-core.md).

**RenderDoc** (Windows/Linux):

```cpp
RenderDocCapture& rd = device->renderDoc();   // инициализируется при создании Device
if (!rd.available()) rd.initialize(/*loadIfMissing*/ true); // загрузить renderdoc.dll / librenderdoc.so
if (rd.available()) rd.triggerCapture();      // захват следующего показанного кадра
// для headless-работы: rd.startFrameCapture(); ... rd.endFrameCapture();
else OX_LOG_INFO("{}", rd.unavailableReason());
```

**Xcode GPU capture** (macOS, RenderDoc там нет — `unavailableReason()` об этом и скажет):

1. запустите приложение с `MTL_CAPTURE_ENABLED=1` (из Xcode: Scheme → Run → Environment Variables);
2. Debug → Capture GPU Workload в Xcode (или автоматический захват через `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE` MoltenVK);
3. в захвате видны Metal-команды с именами объектов и метками пассов из Vulkan.

## Типичные ошибки и подводные камни

- **Два `Device` одновременно.** volk хранит глобальные указатели на функции — второе устройство ломает первое. В тестах создавайте устройство в `SetUp` и уничтожайте в `TearDown`.
- **Граф переживает устройство.** Перед `device.reset()` вызовите `graph.releaseResources(*device)`; swapchain тоже уничтожайте до устройства.
- **Свои descriptor set'ы в шейдере.** Пайплайн будет отклонён валидацией bindless-раскладки (`lastPipelineError()`). Используйте макросы `bindless.glsl` и адреса буферов.
- **Push constants больше 128 байт** или `pushConstantSize` меньше, чем в шейдере, — ошибка создания пайплайна. Держите C++-структуру и GLSL-блок в синхроне (scalar layout: без выравнивания vec3 до 16 байт).
- **Сэмплинг текстуры не в `SHADER_READ_ONLY_OPTIMAL`.** Bindless-дескриптор записан с этим layout'ом. Вне графа переводите текстуру `cmd.transition(tex, Access::SampledFragment)`.
- **Два объявления одного ресурса в одном пассе** (например, `read` и `color`) не поддерживаются: одна декларация на ресурс на пасс. Для read-modify-write — `write()` или `color(..., LOAD)`.
- **Пасс «пропал».** Его отбросил culling: результат никто не читает. Отметьте ресурс `markOutput()` или пасс `sideEffect()`. Проверить — `plan.culledPasses` или `plan.dump(graph)`.
- **Чтение неинициализированного транзиента** даёт предупреждение в `plan.warnings`: у транзиентов нет содержимого с прошлого кадра — для истории (TAA) используйте постоянную текстуру и `importTexture(device, tex, …)`.
- **Барьеры на отдельные mip'ы.** Граф отслеживает ресурс целиком. Для поуровневых операций (генерация мипов, Hi-Z) ставьте `cmd.textureBarrier(tex, src, dst, range)` внутри пасса.
- **Ждать результата сразу после `submit`.** `gpuTimings()` относится к последнему *завершённому* кадру, а не к текущему; данные readback-буферов читайте после `wait(point)`.
- **Hot reload «не срабатывает».** Пайплайн создан из inline-GLSL (`ShaderStageDesc::glsl`) или SPIR-V — перезагружаются только `ShaderStageDesc::file(...)`. На ФС с секундным разрешением mtime быстрые правки могут не заметиться.
- **Проверка фич по платформе** (`#ifdef __APPLE__`) вместо `DeviceCaps` — ломается на новых драйверах MoltenVK и на Linux/Windows без нужных расширений.
- **Сырой `<vulkan/vulkan.h>` с прототипами** или линковка `Vulkan::Vulkan`. Только `<oxwald/rhi/vulkan.hpp>` (volk, `VK_NO_PROTOTYPES`).

## API

| Заголовок | Что внутри |
| --- | --- |
| [`device.hpp`](../../engine/rhi/include/oxwald/rhi/device.hpp) | `Device`, `DeviceDesc`, `SubmitInfo`, `TextureUploadDesc` |
| [`device_caps.hpp`](../../engine/rhi/include/oxwald/rhi/device_caps.hpp) | `DeviceCaps`, `GpuVendor` |
| [`handles.hpp`](../../engine/rhi/include/oxwald/rhi/handles.hpp) | `Handle<Tag>`, `HandlePool`, `IndexAllocator`, `kInvalidBindlessIndex` |
| [`types.hpp`](../../engine/rhi/include/oxwald/rhi/types.hpp) | `BufferDesc`, `TextureDesc`, `SamplerDesc`, `MemoryUsage`, `DefaultSampler`, `TimelinePoint` |
| [`access.hpp`](../../engine/rhi/include/oxwald/rhi/access.hpp) | `Access`, `accessInfo()` |
| [`format.hpp`](../../engine/rhi/include/oxwald/rhi/format.hpp) | `formatInfo()`, `mipLevelSize()`, `estimateTextureSize()` |
| [`command_list.hpp`](../../engine/rhi/include/oxwald/rhi/command_list.hpp) | `CommandList`, `RenderingDesc`, `ColorAttachment`, `DepthAttachment` |
| [`pipeline.hpp`](../../engine/rhi/include/oxwald/rhi/pipeline.hpp) | `ShaderStageDesc`, `GraphicsPipelineDesc`, `ComputePipelineDesc`, `RayTracingPipelineDesc`, `BlendState`, `kMaxPushConstantSize` |
| [`shader_compiler.hpp`](../../engine/rhi/include/oxwald/rhi/shader_compiler.hpp) | `ShaderCompiler`, `ShaderCompileDesc/Result`, `ShaderReflection`, `validateAgainstBindlessLayout()` |
| [`render_graph.hpp`](../../engine/rhi/include/oxwald/rhi/render_graph.hpp) | `RenderGraph`, `PassBuilder`, `PassContext`, `RenderGraphPlan`, `RGCompileOptions`, `RGExecuteOptions` |
| [`swapchain.hpp`](../../engine/rhi/include/oxwald/rhi/swapchain.hpp) | `ISurfaceProvider`, `Swapchain`, `PresentMode`, `SwapchainColor` |
| [`glfw_surface.hpp`](../../engine/rhi/glfw/include/oxwald/rhi/glfw_surface.hpp) | `initGlfwVulkan()`, `GlfwSurfaceProvider` (таргет `Oxwald::rhi_glfw`) |
| [`acceleration_structure.hpp`](../../engine/rhi/include/oxwald/rhi/acceleration_structure.hpp) | `BlasDesc`, `BlasTriangles`, `TlasDesc`, `TlasInstance` |
| [`profiling.hpp`](../../engine/rhi/include/oxwald/rhi/profiling.hpp) | `GpuTiming`, `GpuMemoryStats`, `RenderDocCapture` |
| [`environment.hpp`](../../engine/rhi/include/oxwald/rhi/environment.hpp) | `configureVulkanEnvironment()`, `initializeVulkanLoader()` |
| [`bindless.glsl`](../../engine/shaders/common/bindless.glsl) | GLSL-сторона bindless-модели |

Заметки для разработчиков модуля (текущие ограничения, DeviceCaps на Apple M4 Pro, тесты): [`docs/dev/modules/rhi.md`](../dev/modules/rhi.md).

## Что дальше

- [16. Открытый мир](16-world.md) — CPU-часть ландшафта, неба, воды и стриминга, которую рендерер выводит через RHI.
- [01. Ядро](01-core.md) — логирование, профилирование CPU в Tracy, слежение за файлами.
- [04. CVar'ы и уровни качества](04-cvars-quality.md) — как настройки качества графики попадают в рендерер.
- Глава о рендерере (*скоро*) — GPU-сцена, материалы и пассы кадра поверх render graph.
- [Оглавление](README.md).
