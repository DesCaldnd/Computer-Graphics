# 18. Устройство рендерера

> Модуль `render` (таргет `Oxwald::render`, пространство имён `ox::render`, зонтичный заголовок `<oxwald/render/render.hpp>`). Это кластерный forward+ рендерер поверх [RHI](17-rhi-vulkan.md): GPU-сцена, extract кадра, API расширения через render features, PBR и IBL, тени, пикинг, оверлеи редактора, отладочные режимы, статистика. Шейдеры лежат в `engine/shaders/render/`. Глава нужна всем, кто пишет свои эффекты или встраивает рендерер в инструмент. Если вы просто делаете игру, хватит шагов 1, 2 и 7.

## Зачем

Рендерер решает три задачи. Каждой посвящена своя часть главы:

| Задача | Как решена |
| --- | --- |
| Отделить игру от GPU | Игровой поток копирует нужное из мира в `RenderSnapshot` (extract), а поток рендера работает только со снимком. Мир рендерер не трогает |
| Собрать кадр | Каждый кадр для каждого вида заново объявляется [render graph](17-rhi-vulkan.md): встроенные проходы плюс проходы фич. Граф сам расставляет барьеры, отбрасывает неиспользуемые проходы и переиспользует память |
| Расширяться без правки движка | Эффект оформляется как `IRenderFeature`. Он встраивается в одну из 12 точек кадра, читает и публикует ресурсы по именам и получает cvar-переключатель `r.Feature.<Name>` |

**Почему forward+, а не deferred.** Прозрачность использует те же кластеры света, что и непрозрачная геометрия. Модель материала не ограничена размером G-buffer. MSAA тоже остаётся возможным. Тонкий G-buffer после depth prepass всё равно есть: `Normals` и `Velocity` нужны экранным эффектам (SSAO, SSR, TAA) ещё до освещения.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `Renderer` | Владеет GPU-сценой, кэшем ресурсов, реестром фич и видами. Создаётся на существующем `rhi::Device` |
| `RenderSnapshot` | Копия мира на один кадр: камеры, меши с текущей и прошлой матрицей, свет, окружение, линии `DebugDraw`, палитры скиннинга, расширения фич |
| `RenderView` / `ViewId` | Постоянное состояние вьюпорта: разрешение рендера и вывода, jitter, прошлые матрицы, history-текстуры, состояние фич, свой граф |
| `CameraParams` | Камера кадра: матрица мира, FOV в радианах, near/far, EV100. Строится `fromComponent(...)` или `lookAt(...)` |
| `RenderSettings` | Снимок всех `r.*` cvar'ов, который берётся один раз за кадр. Проходы читают снимок, а не cvar'ы |
| `IRenderFeature` | Эффект, встроенный в кадр: точки внедрения, порядок, группа взаимоисключения, `setup()` объявляет проходы |
| `FeatureContext` | Всё, что доступно фиче в `setup()`: граф, ресурсы кадра, вид, настройки, GPU-сцена, списки отрисовки |
| `FrameResources` | «Доска объявлений» кадра: ресурсы графа по именам (`res::kDepth`, `res::kAO`…) |
| `GpuScene` / `GpuResourceCache` | Геометрия, материалы и инстансы на GPU. Кэш отображает UUID ассета в меш, текстуру или материал |
| `RenderStats` | Статистика последнего завершённого кадра: время каждого прохода на GPU, draw calls, треугольники, VRAM |

## Шаг 1. Рендер кадра: Renderer, вид, снимок

Минимальный цикл. Устройством и кадрами устройства владеет вызывающий код: поток рендера runtime, вьюпорт редактора или тест.

```cpp
#include <oxwald/render/render.hpp>
using namespace ox::render;

std::unique_ptr<Renderer> renderer = Renderer::create(*device);   // + RendererDesc{.jobs = &jobs}
ViewId view = renderer->createView({.name = "Main"});

// Игровой поток (фаза Extract): копия мира
world.updateTransforms();
world.snapshotPreviousTransforms();
RenderSnapshot snapshot;
extract(world, snapshot);

// Поток рендера
device->beginFrame();
renderer->beginFrame(snapshot);                        // снимок настроек, загрузки, инстансы
ViewRenderRequest req;
req.view = view;
req.camera = CameraParams::lookAt({0, 2, 3.5f}, {0, 0.3f, 0}, 50.0f, 0.1f, 200.0f);
req.camera.ev100 = 11.0f;
req.target.texture = target;                           // офскрин-цель (или .swapchain = swapchain.get())
req.target.finalAccess = rhi::Access::TransferRead;    // состояние цели после кадра
renderer->renderView(req);                             // можно несколько видов за кадр
renderer->endFrame();                                  // RenderStats
device->endFrame();
```

Полный пример: `samples/guide_examples/18-rendering-overview/guide_render_fixture.hpp` (метод `render`) и тест `FrameRunsCorePasses` в `custom_feature.cpp`.

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `RendererDesc::jobs` | `nullptr` | `JobSystem` для загрузки ассетов на рабочих потоках. Без него загрузка идёт в `update()` на потоке рендера |
| `RendererDesc::builtinFeatures` | `true` | Встроенные фичи: тени, небо, IBL, прозрачность, пост-обработка, оверлеи и т. д. |
| `RendererDesc::instantiateFactories` | `true` | Создать все фичи, зарегистрированные через `registerFeatureFactory` |
| `ViewDesc::flags.editor` | `false` | Буфер `EntityID` (пикинг), обводка выделения, сетка |
| `ViewRenderRequest::settingsOverride` | — | Свои `RenderSettings` для одного вида (например, debug view только в одном вьюпорте) |
| `ViewRenderRequest::recordInto` | `nullptr` | Записать вид в чужой command list вместо собственной отправки (редактор) |
| `RenderTarget::finalAccess` | `SampledFragment` | В каком состоянии оставить офскрин-цель |

**Снимок.** Функция `extract(world, out, options)` вызывается на игровом потоке. Она читает `Camera`, `MeshRenderer`, `Light`, `Environment` и `WorldTransform` (текущий и прошлый) и пропускает неактивные сущности. Поток рендера читает только снимок. Двойной буфер `SnapshotBuffer` позволяет игровому потоку писать кадр N+1, пока рендер рисует кадр N:

```cpp
SnapshotBuffer buffers;
RenderSnapshot& out = buffers.writeSlot();
extract(world, out, {.time = 1.0, .deltaTime = 1.0f / 60.0f, .frame = 60});
buffers.publish();
const RenderSnapshot& s = buffers.readSlot();   // поток рендера
```

| В снимке | Откуда |
| --- | --- |
| `cameras` | `CameraComponent` + мировая матрица (текущая и прошлая). `primaryCamera()` — первая с `primary`, иначе 0, или −1, если камер нет |
| `meshes`, `materials` | `MeshRendererComponent`. На каждый сабмеш — UUID материала в общем массиве `materials` (`materialOffset/Count`) |
| `lights` | `LightComponent`: позиция, направление (−Z сущности), флаг `moved` |
| `environment` | `EnvironmentComponent`, индекс солнца в `lights` |
| `debugLines`, `debugLinesOverlay` | `DebugDraw` (вызовите `flush()` до extract) |
| `extensions` | Данные фич (частицы, вода, пробы…) из extract-хуков (`addExtractHook`) |

Полный пример: `samples/guide_examples/18-rendering-overview/frame_cpu.cpp` (`ExtractWorldIntoSnapshot`).

## Шаг 2. Рендерер в игре и в редакторе

Сами вызывать `beginFrame`/`renderView` нужно только в инструментах и тестах. Игра и редактор используют готовые точки входа:

| Точка входа | Заголовок | Для чего |
| --- | --- | --- |
| `createRenderer(options)` | `runtime_renderer.hpp` | `ox::IRenderer` для движка (глава [05](05-runtime.md)): устройство, swapchain (или офскрин-цель в headless-режиме), extract в слот снимка, рендер из основной камеры, связь с `AssetManager` и hot reload ассетов. Его ставит OxwaldPlayer |
| `rendererOf(IRenderer&)` | `runtime_renderer.hpp` | Доступ к `Renderer` за `IRenderer`: фичи, статистика, кэш ресурсов |
| `EditorViewportRenderer` | `editor_viewport.hpp` | Вьюпорт редактора без Qt: запись в command list редактора, синхронный пикинг по UUID, `renderToImage` для миниатюр |
| `makeEditorViewportRenderer(device)` | `editor_viewport_adapter.hpp` | Обёртка в `editor::IViewportRenderer` (подключайте из кода редактора) |

```cpp
#include <oxwald/render/runtime_renderer.hpp>

ox::Engine engine;
engine.setRenderer(ox::render::createRenderer({.headlessWidth = 320, .headlessHeight = 180})); // до init()
ox::EngineConfig config;
config.headless = true;           // без окна: рендер в офскрин-текстуру
engine.init(config);
// ... сцена: CameraComponent{.primary = true}, MeshRendererComponent ...
engine.run(3);
ox::render::Renderer* r = ox::render::rendererOf(engine.renderer());
OX_LOG_INFO("draw calls: {}", r->stats().drawCalls);
```

Полный пример: `samples/guide_examples/18-rendering-overview/runtime_renderer.cpp`.

| `RuntimeRendererOptions` | По умолчанию | Смысл |
| --- | --- | --- |
| `autoDetectQuality` | `false` | Прогнать GPU-бенчмарк в `init()` и выставить уровни качества (глава 26) |
| `headlessWidth`, `headlessHeight` | 1280, 720 | Размер офскрин-цели без окна |

Редактор:

```cpp
EditorViewportRenderer evr(*device);
EditorViewportFrame f;
f.world = &world;
f.camera = CameraParams::lookAt({0, 0, 4}, {0, 0, 0});
f.debugView = DebugView::None;   // у вьюпорта свой режим просмотра, cvar не нужен
std::vector<u8> rgba = evr.renderToImage(f, 64, 64);              // миниатюра
std::optional<Uuid> hit = evr.pick(f, {64, 64}, {32, 32});        // UUID сущности под курсором
```

Пример: тест `EditorViewportRendersToImage` в `custom_feature.cpp`. Сам редактор описан в главе [30](30-editor.md).

## Шаг 3. Как устроен кадр

Порядок проходов одного вида. В квадратных скобках указаны точки внедрения (`InjectionPoint`) и фичи, которые туда встраиваются:

```
 снимок ──► SceneUpload (инстансы / материалы / таблица мешей: копии из staging)
   [PreDepth]      GPU-каллинг, окклюзия по HiZ прошлого кадра
 DepthPrepass ──► Depth (D32, reversed-Z) · Normals (RGBA16F) · Velocity (RG16F) · EntityID (редактор)
 HiZ             (отбрасывается, если никто не читает)
   [AfterDepth]    SSAO, трассировка SSR, контактные тени, декали
 LightCulling    (compute, кластеры 16×9×24) ──► LightClusters
   [Shadows]       ShadowsRaster | ShadowsRT ──► каскады, атлас, точечные тени, ShadowMask
   [Lighting]      входы освещения: AO, ReflectionsSpecular, IndirectDiffuse, VolumetricFog
 ForwardOpaque   (depth EQUAL, кластерный PBR + IBL) ──► SceneColorHDR (RGBA16F, pre-exposed)
   [AfterOpaque]   небо (встроено, order −1000), композит SSR, копия для преломления
   [Translucency]  прозрачное, OIT, вода, частицы
   [BeforePostProcess] TAA
   [PostProcess]   автоэкспозиция, bloom, DOF… (по order)
   [Upscale]       один слот: FSR1 | DLSS; иначе bilinear Resample, если разрешения различаются
   [AfterUpscale]  HDR-эффекты в разрешении вывода
 Tonemap         (ACES / AgX / Neutral, экспозиция, sRGB, dither) ──► SceneColorLDR (RGBA8)
   [Overlay]       сетка и обводка редактора, DebugLines, UI, гизмо
   [Debug]         режимы отладки
 Final           (копия в цель) ──► Output (swapchain или офскрин)
```

| Точка | Что уже готово | Типичные фичи |
| --- | --- | --- |
| `PreDepth` | Загруженная сцена | GPU-каллинг |
| `AfterDepth` | `Depth`, `Normals`, `Velocity`, `HiZ` | SSAO, SSR, контактные тени, декали |
| `Shadows` | `LightClusters` | Растровые или RT-тени (одна группа) |
| `Lighting` | Всё выше | Публикация `AO`, `ReflectionsSpecular`, `IndirectDiffuse`, `VolumetricFog` |
| `AfterOpaque` | `SceneColorHDR` с непрозрачным и небом | Композит SSR, источник преломления |
| `Translucency` | То же + кластеры | Стекло, вода, частицы |
| `BeforePostProcess` | Полный HDR-кадр | TAA |
| `PostProcess` | — | Цепочка HDR-эффектов по `order()` |
| `Upscale` | — | Единственный слот, побеждает фича с наибольшим `priority()` |
| `AfterUpscale` | `SceneColorHDR` в разрешении вывода | Эффекты в полном разрешении |
| `Overlay` | `SceneColorLDR` (после тонмаппинга) | UI, гизмо |
| `Debug` | — | Визуализации |

**Граф объявляется заново каждый кадр.** `rhi::RenderGraph` кэширует скомпилированный план по хэшу топологии. Поэтому изменение любого cvar'а или результата `isEnabled()` фичи перестраивает граф на следующем кадре без перезапуска. Проходы, результат которых никто не читает, отбрасываются автоматически.

## Шаг 4. Ресурсы кадра: имена и форматы

Фичи общаются через `FrameResources` по именам. Имена и форматы — константы из `render_types.hpp`. Используйте `res::k*` и `formats::k*`, а не литералы: тогда смена формата — правка в одной строке.

| `res::` | Формат (`formats::`) | Размер | Кто пишет | Кто читает; что, если ресурса нет |
| --- | --- | --- | --- | --- |
| `kSceneColorHDR` | `kSceneColor` RGBA16F | рендер до Upscale, вывод после | ForwardOpaque, небо, AfterOpaque…PostProcess, апскейлер | Tonemap. **Pre-exposed**: радиантность × `VIEW.preExposure` |
| `kDepth` | `kDepth` D32, reversed-Z (near 1, far 0) | рендер | DepthPrepass | все; линеаризация `oxLinearDepth` |
| `kNormals` | `kNormals` RGBA16F: xyz мировая нормаль (с normal map), w perceptual roughness | рендер | DepthPrepass | SSR, SSAO, ShadowMask |
| `kVelocity` | `kVelocity` RG16F: `uvТекущий − uvПрошлый` без jitter | рендер | DepthPrepass | TAA, motion blur, апскейлеры |
| `kEntityId` | `kEntityId` R32_UINT, 0 = нет | рендер | DepthPrepass (виды редактора) | пикинг, обводка |
| `kHiZ` | `kHiZ` R32F, полная цепочка мипов, min | рендер | HiZ | окклюзия, SSR |
| `kLightClusters` | буфер | 16×9×24 × (1 + maxLights) u32 | LightCulling | ForwardOpaque, прозрачность, волюметрика |
| `kShadowMask` | `kShadowMask` R8, видимость солнца | рендер | ShadowsRaster / ShadowsRT | ForwardOpaque; без него сэмплируются каскады |
| `kAO` | `kAO` R8 | рендер | фича на `Lighting` | ForwardOpaque умножает непрямой диффуз и specular occlusion; по умолчанию 1 |
| `kReflectionsSpecular` | `kReflections` RGBA16F: rgb радиантность (не pre-exposed), a — вес | рендер | SSR, пробы, RT-отражения | `mix(ibl, rgb, a)`; по умолчанию IBL |
| `kIndirectDiffuse` | `kIndirectDiffuse` RGBA16F: irradiance/π белой поверхности | рендер | GI-фичи | заменяет SH-освещённость; по умолчанию SH9 |
| `kVolumetricFog` | `kVolumetricFog` RGBA16F 3D (froxels) | сетка `VIEW.volumetricFogGrid` | волюметрика | композит, прозрачность |
| `kSceneColorLDR` | `kSceneColorLDR` RGBA8, sRGB-кривая | вывод | Tonemap | Overlay, Debug, Final |
| `kOutput` | формат цели | вывод | Final | импортированная цель вида |

Правила:

- свои выходы публикуйте через `resources().setTexture(name, rg)`;
- каждое чтение объявляйте (`.read(t, Access::...)`), чтобы граф поставил барьер;
- `SceneColorHDR` меняйте на месте через `.color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD)` или `.write(hdr, Access::StorageWriteCompute)`;
- входы освещения (`AO`, `ReflectionsSpecular`, …) должны существовать до объявления `ForwardOpaque`, то есть публикуются на `Lighting` или раньше.

Константы проверяются в тесте `InjectionPointsAndResourceContracts` (`frame_cpu.cpp`).

## Шаг 5. Своя render feature

Полный рабочий пример: простой экранный AO. Compute-проход читает `Depth` и `Normals`, пишет `AO`, а встроенный `ForwardOpaque` сам умножает на него непрямое освещение.

```cpp
#include <oxwald/render/render.hpp>
#include <oxwald/rhi/device.hpp>
using namespace ox;
using namespace ox::render;

CVar<bool> cvGuideAo("r.GuideAO", true, "Guide: screen-space ambient occlusion");
CVar<float> cvGuideAoRadius("r.GuideAO.Radius", 0.5f, "Guide AO radius (m)", Scalability::Effects,
                            {0.3f, 0.5f, 0.5f, 0.8f});
CVar<float> cvGuideAoIntensity("r.GuideAO.Intensity", 1.5f, "Guide AO strength");

class GuideAoFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "GuideAO"; }          // → r.Feature.GuideAO
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    std::string_view exclusiveGroup() const override { return "AO"; }      // как встроенная AmbientOcclusion и RT AO
    i32 priority() const override { return 10; }                            // выше встроенной (0) — заменяет её
    std::vector<std::string_view> provides() const override { return {res::kAO}; }
    std::vector<std::string> cvarNames() const override { return {"r.GuideAO", "r.GuideAO.Radius", "r.GuideAO.Intensity"}; }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvGuideAo; }

    bool initialize(FeatureInitContext& ctx) override {
        m_pipeline = createComputePipeline(ctx.device, "guide.ao", "guide/simple_ao.comp");
        return bool(m_pipeline);                 // false — фича выключается навсегда
    }
    void shutdown(rhi::Device& device) override { device.destroy(m_pipeline); }

    void setup(FeatureContext& ctx) override {
        FrameResources& R = ctx.resources();
        const rhi::RGTexture depth = R.texture(res::kDepth);
        const rhi::RGTexture normals = R.texture(res::kNormals);
        const Extent2D e = ctx.renderExtent();   // разрешение рендера, не вывода

        rhi::TextureDesc td;
        td.format = formats::kAO;
        td.width = e.width;
        td.height = e.height;
        td.usage = rhi::TextureUsage::None;      // граф выведет Storage | Sampled сам
        td.name = "GuideAO";
        const rhi::RGTexture ao = ctx.graph().createTexture(td);

        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        const f32 radius = cvGuideAoRadius, intensity = cvGuideAoIntensity;
        ctx.graph()
            .addPass("GuideAO", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .overwrite(ao, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 depth, normals, outAo;
                    f32 radius, intensity;
                } pc{view, scene, p.sampledIndex(depth), p.sampledIndex(normals), p.storageIndex(ao), radius, intensity};
                p.cmd.bindPipeline(m_pipeline);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(e.width, e.height);
            });
        R.setTexture(res::kAO, ao);              // ForwardOpaque прочитает AO
    }

private:
    rhi::PipelineHandle m_pipeline;
};
```

Шейдер `shaders/guide/simple_ao.comp` (сокращённо):

```glsl
#version 460
#include <render/common/math.glsl>
#include <render/common/scene.glsl>     // OX_RENDER_PUSH, VIEW; сам подключает view.glsl
layout(local_size_x = 8, local_size_y = 8) in;
OX_RENDER_PUSH(uint depth; uint normals; uint outAo; float radius; float intensity;);

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = ivec2(VIEW.renderSize.xy);
    if (any(greaterThanEqual(p, size))) return;
    float d = OX_FETCH_2D(pc.depth, p, 0).r;
    if (d <= 0.0) { OX_IMAGE_STORE_2D(r8, pc.outAo, p, vec4(1.0)); return; }   // небо: reversed-Z, far = 0
    vec2 uv = (vec2(p) + 0.5) * VIEW.renderSize.zw;
    vec3 P = oxWorldPositionFromDepth(pc.view, uv, d);
    vec3 N = normalize(OX_FETCH_2D(pc.normals, p, 0).xyz);
    float radiusPx = clamp(pc.radius * abs(VIEW.proj[1][1]) * 0.5 * VIEW.renderSize.y
                           / oxLinearDepth(pc.view, d), 2.0, 48.0);
    float occlusion = 0.0;
    // ... 12 выборок глубины в диске radiusPx: max(dot(N, normalize(Q - P)) - 0.1, 0) × спад по расстоянию ...
    float ao = clamp(1.0 - pc.intensity * occlusion / 12.0, 0.0, 1.0);
    OX_IMAGE_STORE_2D(r8, pc.outAo, p, vec4(ao));
}
```

Полный пример: `samples/guide_examples/18-rendering-overview/custom_feature.cpp` и `shaders/guide/simple_ao.comp`. Тест `CustomAoFeatureDarkensCreases` проверяет, что щель у основания куба темнеет, а открытый пол остаётся прежним.

В движке уже есть фича `AmbientOcclusion` (GTAO, на Low — SSAO; `r.AO.Method`: 0 выкл., 1 SSAO, 2 GTAO) в той же группе `"AO"` с приоритетом 0. Поэтому у нашей фичи `priority() = 10`: пока `r.GuideAO` включён, работает она, а встроенная отключается. С тем же приоритетом победила бы та, что зарегистрирована раньше, то есть встроенная, и наш проход не запустился бы.

**Где лежат шейдеры фичи.** Путь в `createComputePipeline` и `createFullscreenPipeline` ищется в корнях шейдеров по порядку: сначала `DeviceDesc::shaderOptions.includeRoots`, затем `engine/shaders` (в дистрибутиве — `<exe>/shaders`). Держите шейдеры игры в своём каталоге и добавьте его в `includeRoots`. Внутри шейдера `#include <render/common/...>` найдёт движковые файлы:

```cpp
rhi::DeviceDesc desc;
desc.shaderOptions.includeRoots = {projectDir / "shaders"};   // shaders/guide/simple_ao.comp
```

**Регистрация.**

| Способ | Когда |
| --- | --- |
| `renderer->features().emplace<MyFeature>(args...)` | Одному конкретному `Renderer` во время работы |
| `registerFeatureFactory("MyFeature", [] { return std::make_unique<MyFeature>(); })` | Из явной `registerXxx()` вашего модуля, до создания рендерера. Фабрики инстанцируются во всех `Renderer`, созданных после регистрации |
| `features().remove("MyFeature", device)` | Убрать фичу на ходу |

**Методы `IRenderFeature`.**

| Метод | По умолчанию | Смысл |
| --- | --- | --- |
| `name()` | — | Имя фичи и cvar `r.Feature.<Name>` (bool, по умолчанию `true`) |
| `injectionPoints()` | — | `maskOf(...)` из одной или нескольких точек. `setup()` вызывается по разу для каждой |
| `order()` | 0 | Порядок внутри точки по возрастанию (в PostProcess: bloom 100, DOF 200…) |
| `exclusiveGroup()`, `priority()` | пусто, 0 | В группе работает одна включённая фича с наибольшим приоритетом |
| `provides()`, `cvarNames()` | пусто | Документация, UI настроек, диагностика конфликтов |
| `isEnabled(settings, caps)` | `true` | Проверяется каждый кадр. Изменение результата перестраивает граф |
| `initialize(ctx)` | `true` | Лениво при первом включении: пайплайны, постоянные текстуры |
| `prepareView(setup)` | — | До графа: `screenPercentage`, `jitterPhases`, `mipBias` (апскейлеры, TAA) |
| `setup(ctx)` | — | Объявить проходы для `ctx.point()` |

**Растровый и RT-варианты одного эффекта** кладите в одну `exclusiveGroup`. У RT-варианта приоритет выше, а `isEnabled` возвращает `s.rayTracing && caps.rayTracingSupported()`. На устройствах без ray query `RenderSettings::rayTracing` и так принудительно `false`. Оба варианта должны публиковать одни и те же ресурсы. Разрешение конфликтов можно проверить без GPU (`ExclusiveGroupsOrderAndUpscaleSlot` в `frame_cpu.cpp`).

**Что ещё даёт `FeatureContext`.**

| Метод | Зачем |
| --- | --- |
| `viewConstants()` | Изменяемые `GpuViewConstants`, загружаются после всех `setup()` |
| `viewAddress()`, `sceneAddress()` | Адреса для первых 16 байт push constants (`ViewBuffer view; SceneBuffer scene;`) |
| `allocate(size)`, `upload(span)` | Память кадра, видимая хостом → адрес на GPU. Живёт, пока кадр не завершится |
| `drawLists()` | Списки отрисовки камеры: `Opaque`, `Masked`, `Transparent` (сортировка от дальнего к ближнему), `Refractive` |
| `buildDrawList(filter)`, `cullDrawList(filter)` | Свой список (фрустум, сфера, флаги, корзины) для теневых видов, захватов, плоских отражений. `cullDrawList` каллит на GPU, если включён `r.GpuDriven` |
| `drawBatches(cmd, list, pipelines[4], pc, size)` | Один instanced `drawIndexed` на батч; `pipelines[variant]`: бит 0 — alpha test, бит 1 — двусторонний |
| `history(name, desc)` | Постоянная пара `current`/`previous` (TAA, временное накопление). Сбрасывается при resize и смене камеры |
| `viewState<T>()` | Постоянное состояние на пару (фича, вид); `T` наследует `IFeatureViewState` |
| `defaults()` | Белая, чёрная, плоская нормаль, шахматка, чёрный куб + их bindless-индексы |

Помощники: `fullscreenVertexShader()`, `createFullscreenPipeline(device, name, fragPath, formats, blend, defines)`, `createComputePipeline(device, name, path, defines)`, `drawFullscreen(cmd, pipeline, pc, size)`. Пример растрового прохода на `AfterOpaque`, рисующего поверх `SceneColorHDR` с `LOAD`, есть в `engine/render/tests/gpu/renderer_tests.cpp` (`MarkerFeature`).

**Соглашения шейдеров** (`engine/shaders/render/common/`):

| Include | Что внутри |
| --- | --- |
| `view.glsl` | `ViewConstants`, `ViewBuffer`, `oxLinearDepth`, `oxWorldPositionFromDepth`, `oxViewRay`, `oxProjectToUv`, алиасы storage-изображений `r8/rg16f/r16f`, id режимов отладки |
| `scene.glsl` | `Instance`, `MeshInfo`, `Material`, `Light`, `OX_RENDER_PUSH(...)`, `OX_RENDER_DRAW_PUSH(...)`, макросы `VIEW`/`SCENE`, `oxFetchVertex` |
| `material.glsl` | `oxSampleMaterial`, `oxMaterialAlpha` |
| `pbr.glsl`, `lighting.glsl` | BRDF, `oxEvaluateLighting(...)` (прямой свет, кластеры, тени, IBL), `oxApplyHeightFog` |
| `shadows.glsl`, `clusters.glsl`, `color.glsl`, `sky.glsl`, `math.glsl` | Тени, кластеры, тонмапперы и sRGB, небо, шум и последовательности |
| `fullscreen.vert` | Полноэкранный треугольник, `uv` с началом в левом верхнем углу |

- Push constants: не больше 128 байт, scalar layout. Первые 16 байт — `view` и `scene`. В списке полей макроса **не должно быть запятых**: одна декларация на `;`.
- Помощники принимают 8-байтовые `ViewBuffer`/`SceneBuffer`, а не `ViewConstants` по значению: иначе на каждый вызов грузилось бы около 1,7 КБ.
- Матрицы `VIEW` содержат Y-flip Vulkan (NDC y вниз, `uv = ndc.xy * 0.5 + 0.5`) и reversed-Z.
- В `SceneColorHDR` пишите `radiance * VIEW.preExposure`.
- **Правило Metal:** depth-текстуры, которые сэмплируются comparison-сэмплером, читайте только через `OX_SAMPLE_SHADOW`/`OX_SAMPLE_SHADOW_ARRAY`.
- Всё, что проходит depth test EQUAL против препасса, рисуйте общим `passes/mesh.vert` с `invariant gl_Position`.

## Шаг 6. Разрешение рендера и RenderView

У вида два размера: **разрешение вывода** (цель, окно) и **разрешение рендера**, в котором идут все проходы до `Upscale`. Их соотношение задаёт `r.ScreenPercentage` (25–200, по умолчанию 100). Апскейлеры переопределяют его в `prepareView()` через `ViewSetup::screenPercentage`. Если разрешения различаются и апскейлер не включён, встроенный проход Resample растягивает картинку билинейно.

```cpp
CVarRegistry::instance().set("r.ScreenPercentage", "50", CVarSource::Code);
// вывод 256×256 → ctx.renderExtent() == {128, 128}, ctx.outputExtent() == {256, 256}
```

Тест: `CustomAoFeatureDarkensCreases` (последний блок). В фичах всегда берите размеры из `ctx.renderExtent()`/`ctx.outputExtent()` и `VIEW.renderSize` в шейдере, а не из размера цели.

Что ещё хранит `RenderView`:

| Метод | Смысл |
| --- | --- |
| `renderExtent()`, `outputExtent()` | Размеры текущего кадра |
| `jitterPixels()`, `jitterNdc()` | Субпиксельный сдвиг Halton(2,3), если фича запросила `jitterPhases` |
| `viewProj()`, `unjitteredViewProj()`, `prevUnjitteredViewProj()` | Матрицы кадра; без jitter — для motion vectors |
| `cameraCut()`, `requestCameraCut()` | Смена плана: history-текстуры невалидны |
| `frameIndex()` | Кадров, отрисованных этим видом |

## Шаг 7. Отладочные режимы просмотра и статистика

Режимы переключаются cvar'ом `r.DebugView` (консоль, ImGui-оверлей, меню вьюпорта редактора) или полем `EditorViewportFrame::debugView` для одного вьюпорта:

| `r.DebugView` | Что видно |
| --- | --- |
| `None` | Обычный кадр |
| `Albedo`, `Normals`, `Roughness`, `Metallic`, `Emissive` | Каналы материала без освещения и тонмаппинга (`Normals` — `n * 0.5 + 0.5`) |
| `AO` | Окклюзия материала × `AO` кадра |
| `LightComplexity` | Тепловая карта числа локальных источников в кластере (шкала до 32) |
| `Overdraw` | Перерисовка |
| `ShadowCascades` | Раскраска по каскадам CSM |
| `Wireframe` | Каркас (нужен `DeviceCaps::fillModeNonSolid`) |
| `Velocity`, `Depth`, `ShadowMask` | Буферы кадра |

```cpp
CVarRegistry::instance().set("r.DebugView", "Normals", CVarSource::Code);
// плоскость с нормалью (0, 1, 0) → пиксель ≈ (128, 255, 128)
```

Пример: тест `DebugViews` в `custom_feature.cpp`. Дополнительно есть `r.Wireframe` (каркас поверх кадра), `r.GpuTimings` (тайминги проходов, по умолчанию вкл.) и `r.FrustumCulling`.

**Статистика.** `renderer->stats()` описывает последний завершённый кадр. GPU-тайминги отстают на число кадров в полёте.

| Поле `RenderStats` | Смысл |
| --- | --- |
| `passes` | `PassTiming{name = "<вид>/<проход>", gpuMs, startMs, asyncCompute}` |
| `gpuFrameMs`, `cpuRenderMs` | Сумма GPU-времени проходов; время рендерера на CPU |
| `drawCalls`, `triangles`, `instances`, `visibleInstances` | Нагрузка кадра |
| `lights`, `shadowedLights`, `shadowMapsRendered`, `shadowMapsCached` | Свет и кэш теней |
| `renderGraphPasses`, `renderGraphCompiles` | Проходов в графе; перекомпиляций плана в этом кадре |
| `vramUsageBytes`, `vram` | Память GPU, в том числе по категориям |

Ориентир (Apple M4 Pro, 1080p, 400 инстансов / 1,08 млн треугольников, солнце с 4 каскадами, 64 точечных источника, IBL): **GPU 3,4 мс**, из них ForwardOpaque 2,63 мс, DepthPrepass 0,14 мс, Tonemap 0,06 мс. CPU рендерера — около 1,2 мс.

## Шаг 8. Hot reload шейдеров

Пайплайн, созданный из файла (`createComputePipeline`, `createFullscreenPipeline`, `ShaderStageDesc::file`), перезагружается сам. `Device::beginFrame()` раз в `DeviceDesc::hotReloadPollMs` (200 мс) проверяет время изменения главного файла и всех его include. Затронутые пайплайны перекомпилируются, а `PipelineHandle` фичи остаётся прежним. Если правка не компилируется, продолжает работать старый пайплайн, а ошибка уходит в лог.

```cpp
// В игре — автоматически. В тестах и инструментах — явно:
u32 reloaded = device->reloadChangedShaders(/*force*/ true);
```

Пример: `HotReloadOfFeatureShader` в `custom_feature.cpp`. Тест правит копию `simple_ao.comp` и проверяет, что картинка изменилась. Так же перезагружаются шейдеры самого рендерера из `engine/shaders/render/`. Hot reload ассетов (меши, текстуры, материалы) идёт отдельно через `AssetManager` (см. главу [19](19-materials.md)).

## Шаг 9. GPU-сцена

Фиче, которая рисует геометрию, полезно знать, как устроена сцена на GPU (`gpu_scene.hpp`, `gpu_types.hpp`, `common/scene.glsl`):

| Данные | Устройство |
| --- | --- |
| Геометрия | Общие растущие арены: `positions` (vec3, 12 Б), `attributes` (48 Б: нормаль, тангенс+знак, uv0, uv1, RGBA8), `indices` u32, `skin`, `meshlets`. Индексы относительны мешу, база передаётся как `vertexOffset` |
| `GpuMeshInfo` | На сабмеш: аргументы отрисовки LOD0, границы, смещения скиннинга и мешлетов |
| `GpuMaterial` (128 Б) | Bindless-индексы текстур, PBR-факторы, режим смешивания, IOR, пропускание, толщина, поглощение, UV-трансформ (глава [19](19-materials.md)) |
| `GpuInstance` | На пару (сущность, сабмеш): `world`, `prevWorld`, мировая сфера, индексы меша и материала, флаги (тени, сдвинут, скиннинг, выделен), id сущности |
| На вид и кадр | `GpuLight` (сначала направленный; люмены → кандела), `GpuShadow`, палитры, `GpuSceneHeader` со всеми адресами |

Слоты инстансов стабильны, а обновления инкрементальные: проход `SceneUpload` копирует из staging только изменившиеся элементы.

`GpuResourceCache` (`renderer->resources()`) отображает UUID ассета в GPU-ресурс. Он грузит данные через `AssetProvider` на `JobSystem` и заливает их через transfer-очередь. Пока ассет грузится, меши пропускаются, вместо материала используется материал по умолчанию, вместо текстур — только факторы. Если albedo-текстура отсутствует, видна шахматка. Процедурные примитивы (куб, сфера, плоскость, цилиндр, капсула, конус, тор) всегда в кэше под `primitiveUuid(Primitive::...)`.

| Метод кэша | Смысл |
| --- | --- |
| `setProvider(makeAssetManagerProvider(am), &jobs)` | Загрузка через `AssetManager` (`asset_provider.hpp`) |
| `addMesh`, `addTexture`, `addMaterial` | Регистрация данных из кода. Повторный вызов с тем же UUID заменяет запись |
| `invalidate(id)`, `queueInvalidate(id)` | Перезагрузить ассет (второй вариант потокобезопасен) |
| `flush()` | Дождаться всех загрузок (экраны загрузки, тесты) |
| `state(id)` | `Unknown`, `Loading`, `Ready`, `Missing` |

## Типичные ошибки и подводные камни

- **Фича ничего не делает.** Её проход отброшен графом: результат никто не читает. Публикуйте выход через `setTexture()` под известным именем, которое читает встроенный проход (`res::kAO`…), или пометьте ресурс `markOutput()`, а пасс — `sideEffect()`. Проверить, что проход выполнялся, можно по `renderer->stats().passes`.
- **Вход освещения опубликован слишком поздно.** `AO`, `ReflectionsSpecular` и `IndirectDiffuse` из точки `AfterOpaque` до `ForwardOpaque` не доходят. Публикуйте их на `Lighting` или раньше.
- **`OX_RENDER_PUSH` не определён.** Макрос живёт в `render/common/scene.glsl`, а не в `view.glsl`. Подключайте `scene.glsl`: он сам включает `view.glsl`.
- **Запятые в `OX_RENDER_PUSH(uint a, b;)`.** Препроцессор примет запятую как разделитель аргументов. Пишите `uint a; uint b;`.
- **C++-структура push constants не совпадает с GLSL.** Порядок и типы должны совпадать один в один, scalar layout, первые два поля — `u64 view, scene`.
- **Размеры из цели вместо `renderExtent()`.** При `r.ScreenPercentage` ≠ 100 или с апскейлером фича будет писать мимо. До `Upscale` всё работает в разрешении рендера.
- **`SceneColorHDR` без pre-exposure.** Значения, записанные без `* VIEW.preExposure`, будут «скакать» при автоэкспозиции.
- **Чтение мира из потока рендера.** Всё, что нужно фиче, должно попасть в снимок: через extract-хук (`addExtractHook`) и `RenderSnapshot::extension<T>()`.
- **Фича в занятой группе не запускается.** В `exclusiveGroup` работает одна фича: с наибольшим `priority()`, а при равенстве — зарегистрированная раньше (встроенные регистрируются первыми). Чтобы заменить встроенный эффект, дайте своей фиче приоритет выше. Чтобы работать рядом с ним, не указывайте группу.
- **Первые кадры отличаются от остальных.** IBL окружения строится в первых кадрах, история TAA и временных эффектов пуста. Для сравнения картинок в тестах сделайте пару кадров «прогрева». Статистика `stats()` отстаёт на число кадров в полёте, поэтому проход, добавленный только что, появится в `passes` через 2–3 кадра.
- **Фабрика зарегистрирована после создания рендерера.** `registerFeatureFactory` влияет только на `Renderer`, созданные позже. Уже работающему добавляйте фичу через `features().emplace<T>()`.
- **Шейдер фичи не находится.** Путь относительный и ищется в `includeRoots`, затем в `engine/shaders`. В дистрибутиве шейдеры игры нужно скопировать рядом с исполняемым файлом и добавить их каталог в `includeRoots`.
- **Hot reload «не срабатывает».** Пайплайн создан из inline-GLSL (`ShaderStageDesc::glsl`). Перезагружаются только пайплайны из файлов.
- **Debug view и FXAA.** FXAA и film grain применяются и к отладочным режимам. Для точного чтения пикселей выключите AA.
- **Wireframe не работает.** Нужен `fillModeNonSolid`. На устройствах без него режим недоступен.

## API

| Заголовок | Что внутри |
| --- | --- |
| [`render.hpp`](../../engine/render/include/oxwald/render/render.hpp) | Зонтичный заголовок |
| [`renderer.hpp`](../../engine/render/include/oxwald/render/renderer.hpp) | `Renderer`, `RendererDesc`, `ViewRenderRequest`, `RenderTarget`, `PickResult`, `registerBuiltinFeatures` |
| [`render_feature.hpp`](../../engine/render/include/oxwald/render/render_feature.hpp) | `IRenderFeature`, `InjectionPoint`, `maskOf`, `FeatureContext`, `ViewSetup`, `FeatureRegistry`, `registerFeatureFactory`, `DrawList`, `DrawFilter`, помощники пайплайнов |
| [`render_types.hpp`](../../engine/render/include/oxwald/render/render_types.hpp) | `res::k*`, `formats::k*`, `Extent2D`, `encodeEntityId` |
| [`frame_resources.hpp`](../../engine/render/include/oxwald/render/frame_resources.hpp) | `FrameResources` |
| [`render_view.hpp`](../../engine/render/include/oxwald/render/render_view.hpp) | `RenderView`, `CameraParams`, `ViewDesc`, `ViewFlags`, `HistoryTexture`, `IFeatureViewState`, `haltonJitter` |
| [`render_settings.hpp`](../../engine/render/include/oxwald/render/render_settings.hpp) | `RenderSettings`, `DebugView`, `Tonemapper`, `registerRenderCVars` |
| [`render_stats.hpp`](../../engine/render/include/oxwald/render/render_stats.hpp) | `RenderStats`, `PassTiming` |
| [`snapshot.hpp`](../../engine/render/include/oxwald/render/snapshot.hpp) | `RenderSnapshot`, `extract`, `ExtractOptions`, `addExtractHook`, `SnapshotBuffer`, `ISnapshotExtension` |
| [`gpu_scene.hpp`](../../engine/render/include/oxwald/render/gpu_scene.hpp), [`gpu_types.hpp`](../../engine/render/include/oxwald/render/gpu_types.hpp) | `GpuScene`, `GpuMaterial`, `GpuInstance`, `GpuLight`, `GpuViewConstants` |
| [`gpu_resource_cache.hpp`](../../engine/render/include/oxwald/render/gpu_resource_cache.hpp) | `GpuResourceCache`, `AssetProvider`, `ResourceState` |
| [`asset_provider.hpp`](../../engine/render/include/oxwald/render/asset_provider.hpp) | `makeAssetManagerProvider`, `connectAssetHotReload` |
| [`mesh_primitives.hpp`](../../engine/render/include/oxwald/render/mesh_primitives.hpp) | `Primitive`, `makePrimitive`, `primitiveUuid` |
| [`runtime_renderer.hpp`](../../engine/render/include/oxwald/render/runtime_renderer.hpp) | `createRenderer`, `RuntimeRendererOptions`, `rendererOf` |
| [`editor_viewport.hpp`](../../engine/render/include/oxwald/render/editor_viewport.hpp) | `EditorViewportRenderer`, `EditorViewportFrame` |
| [`common/`](../../engine/shaders/render/common/) | GLSL: `view.glsl`, `scene.glsl`, `material.glsl`, `lighting.glsl`… |

Заметки для разработчиков модуля (golden-тесты, полный список cvar'ов, ограничения): [`docs/dev/modules/render.md`](../dev/modules/render.md).

## Что дальше

- [19. Материалы](19-materials.md) — PBR-параметры, `.oxmat`, режимы смешивания, стекло.
- [20. Освещение и тени](20-lighting-shadows.md) — источники, единицы, каскады, атлас, PCSS, IBL.
- [25. Апскейлеры и пост-обработка](25-upscalers-postprocess.md) — фичи в `Upscale` и `PostProcess`, jitter.
- [27. GPU-driven рендеринг и производительность](27-gpu-driven-performance.md) — `cullDrawList`, мешлеты, стриминг мипов.
- [17. RHI и Vulkan](17-rhi-vulkan.md) — render graph, bindless, пайплайны.
- [Оглавление](README.md).
