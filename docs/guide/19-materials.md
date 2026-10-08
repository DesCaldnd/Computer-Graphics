# 19. Материалы

> Данные материала — `ox::assets::MaterialAsset` (модуль `assets`, таргет `Oxwald::assets`, заголовок `<oxwald/assets/material.hpp>`). Файл материала — `.oxmat` (JSON). На GPU материал превращается в `ox::render::GpuMaterial` (модуль `render`, `<oxwald/render/gpu_types.hpp>`) и шейдится функциями из `engine/shaders/render/common/material.glsl` и `pbr.glsl`. Глава для технических художников и программистов: какие параметры есть, что они делают на экране, как писать `.oxmat` руками и как создавать материалы из кода.

## Зачем

Материал описывает, как поверхность реагирует на свет: цвет, металл это или диэлектрик, насколько она шероховата, светится ли сама, прозрачна ли, преломляет ли свет. В OxwaldEngine один тип материала покрывает все случаи. Режим смешивания выбирает путь отрисовки:

| Задача | Что использовать |
| --- | --- |
| Камень, дерево, пластик, металл | `Opaque`, PBR metallic-roughness |
| Листва, решётки, забор из одной плоскости | `AlphaTest` + `alphaCutoff`, часто `doubleSided` |
| Окна, голограммы, тонкий цветной пластик | `Transparent`: альфа-смешивание (OIT или сортировка) |
| Толстое стекло, лёд, жидкость в сосуде, драгоценные камни | `Refractive`: преломление, IOR, поглощение по толщине |
| Экраны, лампы, неон, лава | `emissive` (в любом режиме) или `Unlit` |

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `MaterialAsset` | Структура с рефлексией: все параметры материала. Сериализуется в `.oxmat` (JSON) и в бинарный OXB1 |
| `.oxmat` | JSON-файл в `Assets/`. Это и исходник, и формат времени выполнения. Отсутствующие поля получают значения по умолчанию |
| UUID материала | Ссылка на материал из `MeshRendererComponent::materials`, по одному на сабмеш (слот) меша |
| `ShadingModel` | `Lit` (PBR), `Unlit` (без освещения), `Subsurface`, `Foliage` |
| `BlendMode` | `Opaque`, `AlphaTest`, `Transparent`, `Refractive` |
| ORM-текстура | Упакованная текстура: R — occlusion, G — roughness, B — metallic (раскладка glTF) |
| `GpuMaterial` | 128-байтовая запись в таблице материалов GPU-сцены: bindless-индексы текстур, факторы, флаги |
| `GpuResourceCache` | `renderer->resources()`: UUID → материал на GPU. Принимает материалы из `AssetManager` или напрямую из кода |

## Шаг 1. PBR metallic-roughness

Освещение физически корректное: GGX-распределение, height-correlated Smith, Френель по Шлику. Есть компенсация энергии многократного рассеяния (`r.Shading.MultiScatter`, на Low выключена), без неё шероховатые металлы темнеют. IBL — префильтрованный кубмап плюс SH9-освещённость (подробно в главе [20](20-lighting-shadows.md)).

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `baseColor` | `(1, 1, 1, 1)` | Линейный RGBA. Для диэлектрика — диффузный цвет, для металла — цвет отражения. `a` — непрозрачность для `AlphaTest`/`Transparent` |
| `metallic` | 0 | 0 — диэлектрик (F0 ≈ 0.04), 1 — металл. Промежуточные значения — только для переходов (ржавчина, грязь на металле) |
| `roughness` | 0.5 | Perceptual roughness: 0 — зеркало, 1 — полностью матовая |
| `albedoTexture` | нет | sRGB-цвет + альфа. Умножается на `baseColor` |
| `ormTexture` | нет | R occlusion, G roughness, B metallic. Каналы G и B **умножаются** на `roughness` и `metallic`: при ORM-текстуре оставьте факторы равными 1 |
| `occlusionStrength` | 1 | `occlusion = mix(1, orm.r, occlusionStrength)`. Влияет только на непрямой свет |
| `normalTexture` | нет | Касательная normal map, используются каналы RG (BC5), Z восстанавливается |
| `normalStrength` | 1 | Множитель XY нормали: 0 — плоско, больше 1 — рельефнее |
| `uvTiling`, `uvOffset` | `(1, 1)`, `(0, 0)` | `uv × tiling + offset` для всех текстур материала |

Цвета указываются **в линейном пространстве**. Цвет из палитры редактора или Photoshop (sRGB) нужно перевести: sRGB 0.5 ≈ линейный 0.214.

Ориентиры для `baseColor` и `roughness`:

| Материал | `baseColor` (линейный) | `metallic` | `roughness` |
| --- | --- | --- | --- |
| Золото | (1.0, 0.78, 0.34) | 1 | 0.2–0.4 |
| Алюминий | (0.91, 0.92, 0.92) | 1 | 0.3–0.6 |
| Пластик, краска | любой цвет | 0 | 0.3–0.6 |
| Бетон, камень | 0.2–0.5 серый | 0 | 0.7–0.95 |
| Уголь, асфальт | 0.02–0.05 | 0 | 0.8–1.0 |

## Шаг 2. Формат `.oxmat`

`.oxmat` — обычный JSON. Ключ `"oxmat": 1` обозначает формат, остальные ключи совпадают с полями `MaterialAsset`. Перечисления записываются строками, векторы — массивами, текстуры — UUID ассетов строкой. Отсутствующие поля получают значения по умолчанию, неизвестные игнорируются. Поэтому минимальный материал занимает три строки:

```json
{
  "oxmat": 1,
  "baseColor": [0.8, 0.05, 0.04, 1.0],
  "roughness": 0.35
}
```

Металл:

```json
{
  "oxmat": 1,
  "shadingModel": "Lit",
  "blendMode": "Opaque",
  "baseColor": [1.0, 0.78, 0.34, 1.0],
  "metallic": 1.0,
  "roughness": 0.3
}
```

Файлы примеров: `samples/guide_examples/19-materials/materials/*.oxmat` (`red_plastic`, `brushed_gold`, `green_glass`, `tinted_window`, `neon_sign`, `leaves`). Тест `LoadHandWrittenOxmat` в `materials_cpu.cpp` загружает каждый из них и проверяет поля.

Все поля:

| Поле | Тип в JSON | По умолчанию |
| --- | --- | --- |
| `shadingModel` | `"Lit"`, `"Unlit"`, `"Subsurface"`, `"Foliage"` | `"Lit"` |
| `blendMode` | `"Opaque"`, `"AlphaTest"`, `"Transparent"`, `"Refractive"` | `"Opaque"` |
| `baseColor` | `[r, g, b, a]` | `[1, 1, 1, 1]` |
| `metallic`, `roughness` | число | 0, 0.5 |
| `emissive`, `emissiveStrength` | `[r, g, b]`, число | `[0, 0, 0]`, 1 |
| `normalStrength`, `occlusionStrength` | число | 1, 1 |
| `heightScale` | число (параллакс, 0 — выкл.) | 0 |
| `albedoTexture`, `normalTexture`, `ormTexture`, `emissiveTexture`, `heightTexture` | UUID строкой | нет |
| `alphaCutoff` | число | 0.5 |
| `doubleSided` | `true`/`false` | `false` |
| `ior`, `transmission`, `thickness` | число | 1.5, 0, 0 |
| `absorptionColor`, `absorptionDistance` | `[r, g, b]`, число (м; 0 — выкл.) | `[1, 1, 1]`, 0 |
| `clearcoat`, `clearcoatRoughness` | число | 0, 0 |
| `subsurface`, `subsurfaceColor` | число, `[r, g, b]` | 0, `[1, 0.3, 0.2]` |
| `uvTiling`, `uvOffset` | `[u, v]` | `[1, 1]`, `[0, 0]` |
| `renderQueueOffset` | целое | 0 |

**Текстуры** указываются UUID текстурного ассета. «Нет текстуры» — нулевой UUID `"00000000-0000-0000-0000-000000000000"` (так пишет `saveMaterial`) или отсутствие поля. UUID берётся из `.meta`-файла рядом с картинкой: `Assets/Textures/rock.png.meta`, поле `uuid`. Путь вместо UUID не сработает: поле останется пустым (с предупреждением в логе), и материал будет без текстуры. В проекте `.oxmat` лучше сохранять из редактора или кодом (шаг 3), а руками править числа.

```json
{
  "oxmat": 1,
  "albedoTexture": "<uuid из Textures/rock.png.meta>",
  "normalTexture": "<uuid из Textures/rock_normal.png.meta>",
  "ormTexture": "<uuid из Textures/rock_orm.png.meta>",
  "roughness": 1.0,
  "metallic": 1.0,
  "uvTiling": [4.0, 4.0]
}
```

Текстуры — зависимости материала (`textureDependencies()`). `AssetManager` загружает их раньше, и материал становится `Loaded`, когда готовы и они.

Импортированные модели (glTF, FBX, OBJ+MTL) получают материалы как суб-ассеты модели. Импортируются факторы, режим альфы и расширения `KHR_materials_transmission`, `volume`, `ior`, `clearcoat`, `emissive_strength`, `unlit` и `KHR_texture_transform`. Если в ORM-текстуре нет упакованной окклюзии, `occlusionStrength` ставится в 0. Подробнее — в главе [31](31-assets.md).

## Шаг 3. Материалы в коде

`MaterialAsset` — обычная структура. Её можно создать, сохранить в `.oxmat` и загрузить обратно:

```cpp
#include <oxwald/assets/material.hpp>
using namespace ox::assets;

MaterialAsset m;
m.blendMode = BlendMode::Refractive;
m.roughness = 0.05f;
m.ior = 1.33f;                           // вода
m.absorptionColor = {0.3f, 0.7f, 0.9f};  // цвет после absorptionDistance метров пути
m.absorptionDistance = 2.0f;
m.albedoTexture = textureUuid;           // UUID текстурного ассета

saveMaterial(m, "Assets/Materials/water.oxmat");   // .oxmat/.json → JSON, иначе бинарный OXB1
std::string json = materialToJson(m);              // тот же JSON строкой
Result<MaterialAsset> back = loadMaterialFile("Assets/Materials/water.oxmat");   // JSON или OXB1
if (!back) OX_LOG_ERROR("material: {}", back.error().message);
```

Пример: `CreateInCodeSaveAndLoad` в `materials_cpu.cpp`. Он печатает полный JSON с полями по умолчанию.

**Назначение на меш.** `MeshRendererComponent::materials` хранит UUID материалов, по одному на сабмеш. Если слотов меньше, чем сабмешей, или UUID не найден, используется материал по умолчанию (белый, `Lit`, roughness 0.5).

```cpp
AssetRegistry registry(projectDir);   // <project>/Assets
registry.scan();                       // создаёт .meta с UUID
Uuid wood = *registry.uuidForPath("Materials/red_plastic.oxmat");

auto& mr = crate.add<MeshRendererComponent>();
mr.mesh = builtin::cubeMesh();         // или UUID меша из модели
mr.materials = {wood};                 // слот 0
```

Пример: `OxmatInProjectAssignedToMesh` в `materials_cpu.cpp`.

**Материал без файла** (процедурный, вариант для конкретного объекта, превью) регистрируется прямо в кэше рендерера под любым UUID:

```cpp
ox::render::Renderer& r = *ox::render::rendererOf(engine.renderer());   // или свой Renderer

assets::MaterialAsset paint;
paint.baseColor = {0.9f, 0.05f, 0.05f, 1.0f};
const Uuid id = Uuid::fromName("Materials/runtime_paint.oxmat");
r.resources().addMaterial(id, paint);   // поток рендера

mesh.get<MeshRendererComponent>().materials = {id};

paint.baseColor = {0.05f, 0.05f, 0.9f, 1.0f};
r.resources().addMaterial(id, paint);   // тот же UUID: запись заменяется, меши не трогаем
```

Пример: тесты `ReplaceMaterialAtRuntime` и `OxmatFilesRender` в `materials_gpu.cpp`. В тестах кэш используется из того же потока, что и рендер. В игре с потоком рендера `addMaterial` нужно вызывать из потока рендера: кэш не потокобезопасен. Обычный путь для игры — `.oxmat` в проекте через `AssetManager` (шаг 8).

## Шаг 4. Режимы смешивания

| `blendMode` | Где рисуется | Тени | Что учитывается |
| --- | --- | --- | --- |
| `Opaque` | Depth prepass + ForwardOpaque | отбрасывает | PBR, текстуры, emissive |
| `AlphaTest` | То же, пиксели с `alpha < alphaCutoff` отбрасываются | отбрасывает (с вырезами) | + `alphaCutoff` |
| `Transparent` | Фича `Translucency`, после непрозрачного | не отбрасывает | `baseColor.a` × альфа текстуры, полное освещение и туман |
| `Refractive` | Фича `Translucency`, от дальнего к ближнему | не отбрасывает | `ior`, `transmission`, `thickness`, поглощение, `roughness` → размытие |

**AlphaTest.** Альфа берётся как `baseColor.a × альфа albedoTexture × альфа цвета вершины`. Пиксель отбрасывается, если она меньше `alphaCutoff`. На дальних мипах альфа «тает», и листва редеет. Против этого есть hashed alpha: `r.AlphaTest.Dither` (`Off`/`On`/`Auto`, по умолчанию `Auto` — включён вместе с TAA). Он даёт стохастический порог на пиксель и кадр, а TAA сглаживает шум. Alpha-to-coverage нет: у рендерера нет MSAA-пути.

```json
{ "oxmat": 1, "shadingModel": "Foliage", "blendMode": "AlphaTest",
  "baseColor": [0.25, 0.5, 0.15, 1.0], "alphaCutoff": 0.4, "doubleSided": true }
```

**Transparent.** Метод выбирает cvar `r.Translucency.Method`:

| Значение | Что делает |
| --- | --- |
| `Auto` (по умолчанию) | Сортировка, если видно не больше `r.Translucency.SortedMaxInstances` (4) прозрачных инстансов, иначе OIT |
| `OIT` | Weighted Blended OIT: порядок не важен, но перекрывающиеся цвета усредняются |
| `Sorted` | Сортировка от дальнего к ближнему (premultiplied alpha). Правильно для непересекающихся объектов |

`renderQueueOffset ≠ 0` заставляет материал всегда идти сортированным путём. Это нужно для слоёв, где важен порядок: стекло с наклейкой, HUD-голограммы.

```json
{ "oxmat": 1, "blendMode": "Transparent", "baseColor": [0.2, 0.5, 1.0, 0.35], "roughness": 0.05, "doubleSided": true }
```

Пример: `TransparentBlendsWithBackground` в `materials_gpu.cpp`. Глава [23](23-transparency-water-particles.md) подробно разбирает OIT, воду и частицы.

## Шаг 5. Преломление: IOR, толщина, поглощение

`Refractive` — для объёмных прозрачных тел. Луч преломляется по `refract(−V, N, 1/ior)`, проходит через объект на `thickness` и дальше до фона (не дальше `r.Refraction.MaxDistance`, 4 м). Цвет фона берётся из размытой мип-цепочки кадра, и чем выше `roughness`, тем дальше мип (матовое стекло). Отражение — Шлик с `F0 = ((ior − 1)/(ior + 1))²`. При полном внутреннем отражении остаётся только отражение окружения. `metallic` для преломляющих материалов игнорируется.

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `ior` | 1.5 | Показатель преломления: вода 1.33, лёд 1.31, стекло 1.5, алмаз 2.42 |
| `transmission` | 0 | Доля пропущенного света. **Для `Refractive` значение 0 считается 1**. Значения меньше 1 возвращают диффузную часть (мутное стекло, молочный пластик) |
| `thickness` | 0 | Толщина в метрах, если её нельзя измерить. Для замкнутых мешей толщина измеряется по задним граням (`r.Refraction.BackfaceDepth`, вкл. со Shading Medium) |
| `absorptionColor` | `(1, 1, 1)` | Цвет, который остаётся после прохождения `absorptionDistance` метров. Закон Бера — Ламберта: `pow(absorptionColor, толщина / absorptionDistance)` |
| `absorptionDistance` | 0 | 0 — поглощения нет |
| `baseColor.rgb` | `(1, 1, 1)` | Дополнительно тонирует преломлённый свет (тонкая цветная плёнка) |
| `roughness` | 0.5 | Размытие преломления и отражения. Для чистого стекла ставьте 0.02–0.05 |

Зелёное бутылочное стекло:

```json
{
  "oxmat": 1,
  "blendMode": "Refractive",
  "roughness": 0.02,
  "ior": 1.5,
  "transmission": 1.0,
  "absorptionColor": [0.45, 0.9, 0.55],
  "absorptionDistance": 1.0,
  "thickness": 0.05
}
```

Толстые части получаются темнее и насыщеннее тонких: толщина измеряется по задним граням. Для **незамкнутых** мешей (плоское окно из одной плоскости) задних граней нет, и работает `thickness` из материала. Если там 0, поглощения не будет. Пример: `RefractiveGlassAbsorbs` в `materials_gpu.cpp`.

| CVar | По умолчанию | Low / Medium / High / Ultra |
| --- | --- | --- |
| `r.Refraction` | вкл. | — |
| `r.Refraction.Strength` | 1 | — |
| `r.Refraction.MaxDistance` | 4 м | — |
| `r.Refraction.Mips` | 6 | Reflections: 3, 5, 6, 7 |
| `r.Refraction.BackfaceDepth` | вкл. | Shading: выкл., вкл., вкл., вкл. |

Ориентир (M4 Pro, 1080p): источник преломления 0,31 мс + копия глубины 0,12 мс (один раз за кадр, если есть хоть один преломляющий объект), задние грани 0,02 мс, сами объекты 0,14 мс.

## Шаг 6. Излучение (emissive) и Unlit

`emissive × emissiveStrength` — цвет свечения. Оно **относительно экспозиции**: 1 означает «белый при текущей экспозиции камеры». Неон с `emissive = 1` выглядит одинаково ярким днём (EV100 15) и в тёмном интерьере (EV100 6). Тест `EmissiveIsExposureRelative` это проверяет. Значения больше 1 уходят в пересвет и подхватываются bloom (глава [25](25-upscalers-postprocess.md)). `emissiveTexture` умножается на цвет.

```json
{ "oxmat": 1, "baseColor": [0.05, 0.05, 0.05, 1.0], "emissive": [1.0, 0.1, 0.6], "emissiveStrength": 4.0 }
```

Emissive **не освещает соседей**: это не источник света. Чтобы лампа светила, поставьте рядом `LightComponent` (глава [20](20-lighting-shadows.md)).

`shadingModel: "Unlit"` выводит `baseColor` (× текстура) без освещения, тоже относительно экспозиции. Подходит для UI в мире, мониторов, отладочной геометрии. `emissive` у Unlit не добавляется: цвет задаётся через `baseColor`.

## Шаг 7. Двусторонние материалы

По умолчанию задние грани отсекаются (back-face culling): плоскость, на которую смотрят сзади, невидима. `doubleSided: true` отключает отсечение для этого материала, а на обратной стороне нормаль разворачивается к зрителю, так что освещение остаётся правильным. Нужно для листвы, ткани, бумаги, окон из одной плоскости.

```cpp
assets::MaterialAsset leaf;
leaf.blendMode = assets::BlendMode::AlphaTest;
leaf.alphaCutoff = 0.5f;
leaf.doubleSided = true;
```

Пример: `AlphaTestAndDoubleSided` в `materials_gpu.cpp`. На GPU двусторонность и альфа-тест — варианты пайплайна (биты `DrawVariant`). Материалы с разными вариантами не попадают в один батч, поэтому не включайте `doubleSided` без нужды.

## Шаг 8. Материалы в проекте и hot reload

В игре и в редакторе материалы грузятся через `AssetManager`. `createRenderer()` сам подключает провайдер и hot reload, если в сервисах движка есть `AssetManager` (глава [31](31-assets.md)). Вручную, например в своём инструменте:

```cpp
#include <oxwald/render/asset_provider.hpp>

assets::AssetRegistry registry(projectDir);
registry.scan();
assets::AssetManager assets(registry, &jobs);
renderer->resources().setProvider(render::makeAssetManagerProvider(assets), &jobs);
ScopedConnection hotReload = render::connectAssetHotReload(assets, renderer->resources());   // держите живым

const Uuid paint = *registry.uuidForPath("Materials/paint.oxmat");
mr.materials = {paint};          // рендер загрузит материал и его текстуры сам

// ... художник сохранил paint.oxmat ...
assets.reload(paint);            // в редакторе это делает слежение за файлами
assets.update();                 // каждый кадр: подмена + onReloaded → кэш рендерера обновит GpuMaterial
```

Пример: `ProjectMaterialsThroughAssetManagerWithHotReload` в `materials_gpu.cpp`. Пока материал грузится, объект рисуется материалом по умолчанию, пока грузятся текстуры — только факторами. Если текстура не найдена, вместо albedo видна шахматка.

## Шаг 9. Что происходит на GPU

`GpuResourceCache` переводит `MaterialAsset` в `GpuMaterial`:

| `GpuMaterial` | Из `MaterialAsset` |
| --- | --- |
| `baseColor`, `metallic`, `roughness`, `normalStrength`, `occlusionStrength`, `alphaCutoff` | Как есть |
| `emissive` | `emissive × emissiveStrength` |
| `albedoTexture`, `normalTexture`, `ormTexture`, `emissiveTexture` | Bindless-индексы; `kInvalidIndex`, если текстуры нет или она ещё грузится |
| `flags` | Биты 0–2 — `BlendMode`; `kMaterialDoubleSided`, `kMaterialUnlit`, `kMaterialSorted` (`renderQueueOffset ≠ 0`) |
| `ior`, `transmission`, `thickness`, `absorptionColor`, `absorptionDistance`, `clearcoat`, `clearcoatRoughness`, `subsurface`, `uvTiling`, `uvOffset` | Как есть |
| `sampler` | Общий анизотропный repeat-сэмплер (`r.Textures.Anisotropy`, `r.Textures.MipBias`) |

В шейдере своей фичи материал инстанса читается так:

```glsl
#include <render/common/material.glsl>
Material m = SCENE.materials.m[SCENE.instances.i[instanceIndex].materialIndex];
OxMaterialSample ms = oxSampleMaterial(m, uv0, vertexColor, N, T, gl_FrontFacing, VIEW.mipBias);
// ms.baseColor, ms.normal, ms.metallic, ms.perceptualRoughness, ms.occlusion, ms.emissive
```

Качество текстур материалов регулируют cvar'ы группы Textures: `r.Textures.Anisotropy` (2/4/8/16), `r.Textures.MipBias` (1/0.5/0/0) и `r.Textures.MaxSize` (1024/2048/4096/8192 — большие мипы не загружаются). На устройствах без BC-сжатия BC-текстуры распаковываются на CPU.

## Типичные ошибки и подводные камни

- **Цвет в sRGB вместо линейного.** `baseColor: [0.5, 0.5, 0.5]` — это не «средне-серый» из пипетки, а заметно светлее. Переводите цвета пипетки в линейные.
- **ORM-текстура и факторы.** Каналы ORM умножаются на `roughness` и `metallic`. Если оставить `roughness: 0.5` по умолчанию, текстура станет вдвое глаже, а `metallic: 0` обнулит металл из текстуры. При ORM ставьте оба фактора в 1.
- **Путь вместо UUID в поле текстуры.** `"albedoTexture": "Textures/rock.png"` не сработает: поле останется пустым, нужен UUID из `.meta`.
- **Опечатка в значении или имени поля.** `"blendMode": "Glass"` не ошибка загрузки: поле остаётся по умолчанию (`Opaque`), а в лог уходит предупреждение `Material.blendMode: cannot convert string value, keeping default`. Ключ с опечаткой (`"roughnes"`) молча игнорируется. Ошибку возвращает только невалидный JSON. Если материал выглядит «по умолчанию», смотрите лог (тест `TyposKeepDefaults`).
- **`Refractive` у плоского окна без `thickness`.** Задних граней нет, толщина 0, поглощения нет. Для тонкого стекла часто лучше `Transparent`.
- **`transmission: 0` у `Refractive`** — это не «непрозрачное стекло»: 0 трактуется как 1. Для мутного стекла задайте, например, 0.6.
- **Прозрачный объект без тени.** В растровые тени попадают только `Opaque` и `AlphaTest`: `Transparent` и `Refractive` теней не отбрасывают. Это ожидаемое поведение, а не ошибка импорта.
- **Emissive как лампа.** Свечение не освещает сцену: добавьте `LightComponent`.
- **`Unlit` с `emissive`.** У Unlit выводится только `baseColor`, `emissive` игнорируется.
- **Поля без эффекта на обычных мешах.** `clearcoat`, `clearcoatRoughness`, `heightScale`/`heightTexture` (параллакс) и модели `Subsurface`/`Foliage` сохраняются и импортируются, но стандартные проходы их пока не шейдят: на мешах они выглядят как `Lit`. `subsurface` учитывается только шейдером растительности (глава [28](28-world-rendering.md)).
- **`addMaterial` из игрового потока при включённом потоке рендера.** Кэш рендерера не потокобезопасен. Вызывайте его на потоке рендера или используйте `.oxmat` через `AssetManager`.
- **Слишком много `doubleSided` и `AlphaTest`.** Это отдельные варианты пайплайна: больше батчей и draw calls. Для непрозрачных мешей, которые видны только снаружи, оставляйте `Opaque` и одностороннюю отрисовку.

## API

| Заголовок | Что внутри |
| --- | --- |
| [`material.hpp`](../../engine/assets/include/oxwald/assets/material.hpp) | `MaterialAsset`, `ShadingModel`, `BlendMode`, `saveMaterial`, `materialToJson`, `materialToBinary`, `loadMaterial`, `loadMaterialFile` |
| [`asset_types.hpp`](../../engine/assets/include/oxwald/assets/asset_types.hpp) | `AssetType::Material`, `builtin::defaultMaterial()`, `builtin::cubeMesh()` |
| [`asset_registry.hpp`](../../engine/assets/include/oxwald/assets/asset_registry.hpp), [`asset_manager.hpp`](../../engine/assets/include/oxwald/assets/asset_manager.hpp) | Проект, UUID по пути, загрузка, hot reload |
| [`gpu_resource_cache.hpp`](../../engine/render/include/oxwald/render/gpu_resource_cache.hpp) | `GpuResourceCache::addMaterial`, `setProvider`, `invalidate` |
| [`asset_provider.hpp`](../../engine/render/include/oxwald/render/asset_provider.hpp) | `makeAssetManagerProvider`, `connectAssetHotReload` |
| [`gpu_types.hpp`](../../engine/render/include/oxwald/render/gpu_types.hpp) | `GpuMaterial`, `GpuMaterialFlags` |
| [`components.hpp`](../../engine/scene/include/oxwald/scene/components.hpp) | `MeshRendererComponent` |
| [`material.glsl`](../../engine/shaders/render/common/material.glsl), [`pbr.glsl`](../../engine/shaders/render/common/pbr.glsl) | `oxSampleMaterial`, `oxMaterialAlpha`, BRDF |
| [`translucent.frag`](../../engine/shaders/render/translucency/translucent.frag) | Шейдинг `Transparent` и `Refractive` |

Заметки для разработчиков: [`docs/dev/modules/assets.md`](../dev/modules/assets.md) (материалы, импорт), [`docs/dev/modules/render.md`](../dev/modules/render.md) §4–5 и §12.

## Что дальше

- [18. Устройство рендерера](18-rendering-overview.md) — кадр, GPU-сцена, свои фичи.
- [20. Освещение и тени](20-lighting-shadows.md) — источники света, единицы, IBL.
- [23. Прозрачность, вода и частицы](23-transparency-water-particles.md) — OIT, вода, частицы.
- [31. Ассеты](31-assets.md) — импорт моделей и текстур, `.meta`, кэш, паки.
- [Оглавление](README.md).
