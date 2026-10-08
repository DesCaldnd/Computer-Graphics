# 31. Ассеты

> Модуль `assets` (таргет `Oxwald::assets`, пространство имён `ox::assets`, зонтичный заголовок `<oxwald/assets/assets.hpp>`). База ассетов проекта, импортёры, CPU-данные в GPU-готовых форматах, асинхронная загрузка с зависимостями и hot reload, упаковка в `.oxpak`. Утилиты `tools/oximport` и `tools/oxpack`. Загрузку на GPU делает рендерер ([глава 18](18-rendering-overview.md)): он получает `MeshData`, `TextureData` и `MaterialAsset` и подписывается на сигналы `AssetManager`.

## Зачем

Художник кладёт в папку `Assets/` файлы из DCC-пакетов: PNG, glTF, FBX, WAV, Lua. Игре нужны другие данные: сжатые текстуры с мипами, меши с LOD и меш-летами, бинарные сцены. Кроме того, ссылки между ассетами не должны ломаться, когда файл переименовали или перенесли. Модуль решает эти задачи:

| Задача | Что есть |
| --- | --- |
| Учёт ассетов | `AssetRegistry`: у каждого файла есть `.meta` с UUID и настройками импорта. Переносы, копии и удаления отслеживаются |
| Импорт | Текстуры (BC7/BC5/RGBA16F, мипы, кубмапы), модели (glTF через fastgltf, FBX/OBJ/DAE и др. через assimp), материалы, сцены, префабы, Lua, звук, шрифты, навмеши, карты высот. Свои импортёры подключаются через `IAssetImporter` |
| Кэш | Результаты импорта лежат в `.oxcache/`. Реимпорт выполняется только при изменении исходника, настроек или версии импортёра |
| Загрузка | `AssetManager`: асинхронно на `JobSystem`, с приоритетами, с зависимостями (материал ждёт свои текстуры), со счётчиком ссылок, LRU-кэшем и бюджетом памяти |
| Hot reload | Сохранили PNG — текстура переимпортирована, перезагружена и заменена в игре без перезапуска |
| Сборка игры | `cookProject` / `oxpack`: только ассеты, достижимые из стартовых сцен, упакованные в один `.oxpak`. Плеер запускается с `--pak` |

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| Исходник (source) | Файл в `<проект>/Assets/**`: `rock.png`, `hero.glb`, `door.lua` |
| `.meta` | JSON рядом с исходником: UUID, имя импортёра и его настройки. Коммитится в VCS вместе с файлом |
| UUID | Постоянный идентификатор ассета. Сцены, материалы и компоненты ссылаются на ассеты только по UUID |
| Путь ассета | Путь относительно `Assets/` с `/`: `"Textures/rock.png"`. Под-ассеты: `"Models/hero.glb#Mesh/0"` |
| Артефакт | Результат импорта в `.oxcache/artifacts/<uuid><ext>`: `.oxtex`, `.oxmesh`, `.oxmat`, `.oxscene`… |
| Под-ассет (sub-asset) | Один исходник даёт несколько артефактов: модель → префаб + меши + материалы. UUID под-ассета детерминирован |
| `AssetType` | `Mesh`, `Texture`, `Material`, `Scene`, `Prefab`, `Script`, `Audio`, `AnimationClip`, `Skeleton`, `Font`, `NavMesh`, `Heightmap`, `Raw`; свои — от `FirstCustom` (1000) |
| `IAssetSource` | Откуда менеджер берёт данные: `AssetRegistry` (редактор, разработка) или `PakAssetSource` (собранная игра) |
| `AssetHandle<T>` | Ссылка на загружаемый ассет со счётчиком ссылок: `get()`, `getOrDefault()`, `onLoaded()`, `future()` |
| `.oxpak` | Архив собранной игры: артефакты, каталог `catalog.oxcat` и файл проекта `.oxproj` |

Структура проекта:

```
MyGame/
  MyGame.oxproj
  pack.json                          (необязательно) что упаковывать — шаг 7
  Assets/**                          исходники + "<файл>.meta"
  .oxcache/artifacts/<uuid><ext>     кэш импорта (не коммитить)
  .oxcache/imports/<uuid>.json       записи импорта: хэши, артефакты, зависимости
```

В редакторе база ассетов отображается в Content Browser ([глава 30](30-editor.md)):

![Content Browser](images/editor/content_browser.png)

## Шаг 1. База ассетов, `.meta` и UUID

```cpp
#include <oxwald/assets/assets.hpp>
using namespace ox::assets;

AssetRegistry registry(projectDir);       // встроенные импортёры уже зарегистрированы
const ScanResult scan = registry.scan();  // создаёт недостающие .meta, строит UUID <-> путь
scan.found; scan.metasCreated; scan.moved; scan.removed; scan.duplicatesFixed;

ox::Uuid rock = *registry.uuidForPath("Textures/rock.png");
registry.info(rock);            // AssetInfo: тип, путь, импортёр, под-ассеты, зависимости, путь артефакта
registry.meta(rock);            // AssetMeta из .meta
registry.absolutePath(rock);    // абсолютный путь исходника
```

`scan()` создаёт `.meta` для каждого файла, у которого есть импортёр. Файлы без импортёра (`notes.txt`, `.rml`) база не видит: такие файлы игра читает напрямую через VFS ([глава 01](01-core.md)). Вот `.meta` текстуры. В `settings` записаны настройки импортёра, перечисления хранятся по именам:

```json
{
  "formatVersion": 1,
  "uuid": "f2ee7da0-57ee-4728-b080-72bb904220d5",
  "importer": "texture",
  "importerVersion": 1,
  "settings": {
    "type": "Color", "srgb": true, "maxSize": 0, "compression": "Default", "generateMips": true,
    "mipFilter": "Kaiser", "preserveAlphaCoverage": false, "alphaCutoff": 0.5, "flipY": false,
    "wrapU": "Repeat", "wrapV": "Repeat", "filter": "Linear", "streamingPriority": 128,
    "cubemap": "None", "cubeFaceSize": 0, "compressionQuality": 1
  }
}
```

**Переносы и копии.** Перенесите файл вместе с `.meta`, и UUID сохранится: придёт сигнал `onMoved(uuid, oldPath, newPath)`, а ссылки в сценах не сломаются. Если внешний инструмент перенёс файл без `.meta`, база найдёт осиротевший `.meta` по хэшу содержимого из записи импорта. Копия файла вместе с `.meta` получает новый UUID (`duplicatesFixed`). Если исходник удалён, его `.meta` и кэш удаляются (`Options::deleteOrphanMetas`, по умолчанию `true`).

```cpp
ScopedConnection c = registry.onMoved.connect([](const ox::Uuid& id, const std::string& from, const std::string& to) {});
std::filesystem::rename(assets / "rock.png", assets / "Nature/rock.png");
std::filesystem::rename(assets / "rock.png.meta", assets / "Nature/rock.png.meta");
registry.scan();   // moved == 1, uuidForPath("Nature/rock.png") == тот же UUID
```

| `AssetRegistry::Options` | По умолчанию | Смысл |
| --- | --- | --- |
| `assetsDir` | `"Assets"` | Папка исходников, относительно проекта или абсолютная. Runtime передаёт сюда `ProjectSettings::assetDirs[0]` |
| `importOnDemand` | `true` | `record()` и `readArtifact()` сами импортируют устаревший ассет |
| `deleteOrphanMetas` | `true` | Удалять `.meta` и кэш исчезнувших файлов |
| `watchDebounce` | 100 мс | Пауза перед реимпортом после изменения файла (hot reload) |

Полный пример: `samples/guide_examples/31-assets/database.cpp` (`ScanCreatesMetasWithStableUuids`, `MovingFileWithMetaKeepsUuid`).

## Шаг 2. Импорт текстур: BC7, BC5, мипы

Импорт выполняется лениво при первом обращении (`record()`, `readArtifact()`, загрузка через `AssetManager`) или явно:

```cpp
const ImportStats stats = registry.importAll();   // imported / upToDate / failed
registry.import(rock, /*force*/ true);            // один ассет
registry.needsImport(rock);

// Настройки пишутся в .meta, ассет тут же реимпортируется.
registry.setSettings(rock, {{"compression", "Uncompressed"}, {"maxSize", 32}});
```

Тип текстуры по умолчанию определяется по имени файла: `*_normal.png` получает `Normal`, `*_orm`, `*_rough`, `*_ao` получают `Linear`, `.hdr` и `.exr` — `HDR`. Если модель ссылается на ещё не импортированную текстуру как на карту нормалей или ORM, её новый `.meta` сразу получает нужный тип.

| Тип (`type`) | Формат по умолчанию | С `compression: Uncompressed` |
| --- | --- | --- |
| `Color`, `UI` (sRGB) | BC7 sRGB | RGBA8 sRGB |
| `Linear` (ORM, маски) | BC7 unorm | RGBA8 unorm |
| `Normal` | BC5: RG = XY, Z восстанавливается в шейдере | RGBA8 unorm |
| `HDR` | RGBA16F (кодировщика BC6H нет) | RGBA16F |

Настройки импорта текстуры (`TextureImportSettings`, те же ключи в `.meta` и в инспекторе):

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `type` | `Color` | `Color`, `Normal`, `Linear`, `HDR`, `UI` |
| `srgb` | `true` | Действует только для `Color`/`UI`. Маски и ORM — тип `Linear` |
| `maxSize` | 0 | Ограничение по большей стороне (0 — без ограничения). Пропорции сохраняются, степень двойки не нужна |
| `compression` | `Default` | `Default` (BC7/BC5) или `Uncompressed` |
| `generateMips`, `mipFilter` | `true`, `Kaiser` | Мипы до 1×1; фильтр Kaiser (windowed sinc) или `Box` |
| `preserveAlphaCoverage`, `alphaCutoff` | `false`, 0.5 | Сохранять долю «непрозрачных» пикселей на всех мипах (листва с alpha test) |
| `flipY` | `false` | Перевернуть по вертикали |
| `wrapU`, `wrapV`, `filter` | `Repeat`, `Repeat`, `Linear` | Сэмплер: `Repeat`/`Clamp`/`Mirror`, `Linear`/`Nearest` |
| `streamingPriority` | 128 | 0..255, выше — стримится раньше |
| `cubemap`, `cubeFaceSize` | `None`, 0 | `FromEquirect` — кубмапа из equirect-панорамы; размер грани (0 — высота / 2) |
| `compressionQuality` | 1 | Уровень UASTC для BC7: 0 (быстро) … 4 (очень медленно) |

![Настройки импорта текстуры в инспекторе](images/editor/inspector_asset_texture.png)

Как это устроено:

- **BC7** получается так: исходник сжимается в Basis Universal UASTC и при импорте перекодируется в BC7. Это даёт нулевую цену при загрузке и мипы, которые можно читать байтовыми диапазонами. BC7 поддерживают все целевые десктопы, включая MoltenVK на Apple Silicon. Качество — PSNR больше 38 дБ (тесты модуля). Для устройств без BC есть `decompressToRGBA8(texture)` (в 4 раза больше памяти).
- **BC5** — собственный кодировщик: два блока BC4, в тестах PSNR больше 40 дБ.
- **Мипы.** sRGB-данные фильтруются в линейном пространстве. Нормали перенормируются на каждом уровне.
- **Кубмапы.** Из equirect (`cubemap: FromEquirect`) или из шести граней: файл `.oxcube` с `{"faces": [+X, -X, +Y, -Y, +Z, -Z]}`.
- **KTX2** (включая Basis ETC1S/UASTC) берётся как есть или перекодируется в BC7 (BC5 для нормалей).

```cpp
auto tex = deserializeTexture(*registry.readArtifact(rock));
tex->format;      // TextureFormat::BC7Srgb
tex->mipCount;    // 7 для 64×64: 64, 32, 16, 8, 4, 2, 1
```

Полный пример: `samples/guide_examples/31-assets/database.cpp` (`TextureImportFormatsAndMips`).

## Шаг 3. Импорт моделей: меши, LOD, меш-леты

Модели читаются через fastgltf (`.gltf`, `.glb` — рекомендуемый формат) или assimp (`obj fbx dae 3ds ply stl blend x lwo ms3d`). Модель импортируется в соглашениях движка: правая система координат, Y вверх, метры, CCW. Главный ассет модели — **префаб**: корневая сущность с именем файла, под ней узлы с `Transform` и `MeshRenderer`. Меши, материалы, встроенные текстуры, скелет и клипы становятся под-ассетами:

```cpp
const ox::Uuid model = *registry.uuidForPath("Models/hill.obj");
registry.import(model);
registry.info(model)->type;                                     // AssetType::Prefab
const ox::Uuid mesh = *registry.uuidForPath("Models/hill.obj#Mesh/0");
const ox::Uuid mat  = *registry.uuidForPath("Models/hill.obj#Material/0");
// UUID под-ассета = Uuid::fromName("<UUID модели>/<имя>"): стабилен между реимпортами.

auto data = deserializeMesh(*registry.readArtifact(mesh));
data->lodCount();            // LOD 0 + упрощённые
data->triangleCount(0);      // полная детализация
data->meshlets.size();       // ≤ 64 вершин и ≤ 124 треугольников в каждом
data->collision;             // выпуклая оболочка + упрощённый меш (если generateCollision)
```

Имена под-ассетов: `Mesh/<i>` (или `Mesh/merged` при `mergeMeshes`), `Material/<i>`, `Texture/<i>[_normal|_linear]` (встроенные картинки), `Skeleton`, `Clip/<i>`. Внешние текстуры из MTL или glTF становятся обычными ассетами-соседями, и материал зависит от них.

Настройки импорта модели (`ModelImportSettings`):

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `scale`, `unitToMeters` | 1, 0 | Доп. масштаб; перевод единиц (0 — авто: `UnitScaleFactor` из FBX, glTF/OBJ = 1) |
| `upAxis` | `Auto` | Ось «вверх» исходника: `Auto`, `Y`, `Z`, `X` |
| `mergeMeshes` | `false` | Запечь всю иерархию в один меш (по сабмешу на материал) |
| `createPrefab`, `importMaterials`, `importAnimations` | `true` | Префаб, материалы, скелет и клипы |
| `generateTangents`, `flipUVs`, `weldVertices` | `true`, `false`, `true` | Касательные, если их нет; переворот UV; сварка вершин |
| `optimize` | `true` | Оптимизация под кэш вершин (meshoptimizer) |
| `generateLods`, `lodRatios`, `lodMaxError` | `true`, `[0.5, 0.25, 0.125]`, 0.05 | LOD: доля индексов на уровень, допустимая ошибка относительно размера меша |
| `generateMeshlets` | `true` | Меш-леты для GPU-driven отрисовки ([глава 27](27-gpu-driven-performance.md)) |
| `generateCollision`, `collisionSimplifyRatio`, `maxHullVertices` | `false`, 0.25, 64 | Данные для физики: выпуклая оболочка и упрощённый треугольный меш |

**LOD.** Все LOD используют общие вершинные потоки, а различаются диапазонами индексов. `MeshLod::error` — ошибка упрощения в единицах меша; она нужна для выбора LOD по экранному размеру. Если упрощение «застряло», у сабмеша может оказаться меньше LOD, чем у соседей, поэтому ограничивайте индекс LOD для каждого сабмеша отдельно.

**Меш-леты.** У каждого меш-лета есть ограничивающая сфера и конус нормалей для отсечения. Раскладка потоков (позиции 12 байт, атрибуты 48 байт, меш-лет 64 байта) описана в [`docs/dev/modules/assets.md`](../dev/modules/assets.md).

Полные примеры: `samples/guide_examples/31-assets/database.cpp` (`ModelImportLodsMeshletsAndSubAssets`, `ModelImportSettingsInMeta`).

## Шаг 4. Материалы

Материал `.oxmat` — это обычный JSON. Он одновременно служит исходником и рантайм-форматом. Отсутствующие поля получают значения по умолчанию, а текстуры задаются UUID:

```json
{
  "oxmat": 1,
  "shadingModel": "Lit",
  "baseColor": [0.8, 0.8, 0.8, 1.0],
  "roughness": 0.85,
  "albedoTexture": "f2ee7da0-57ee-4728-b080-72bb904220d5"
}
```

```cpp
MaterialAsset m;
m.albedoTexture = rock;
m.roughness = 0.8f;
saveMaterial(m, assets / "Materials/rock.oxmat");   // .oxmat/.json — JSON, иначе бинарный OXB1
registry.dependencies(matId);                       // {rock}: текстуры загрузятся раньше материала
```

Материалы из glTF, FBX и MTL импортируются как под-ассеты модели. Их нельзя редактировать, пока не реализовано извлечение в `.oxmat`. Если нужен редактируемый материал, создайте `.oxmat` и назначьте его в `MeshRenderer.materials`. Все поля, режимы смешивания, шейдерные варианты и инстансы описаны в [главе 19](19-materials.md).

Полный пример: `samples/guide_examples/31-assets/database.cpp` (`MaterialFileInProject`).

## Шаг 5. Асинхронная загрузка: `AssetManager`

```cpp
#include <oxwald/assets/asset_manager.hpp>

ox::JobSystem jobs(2);
AssetManager assets(registry /* или PakAssetSource */, &jobs, {.memoryBudget = 512u << 20});

AssetHandle<MaterialAsset> mat = assets.load<MaterialAsset>("Materials/rock.oxmat", kPriorityHigh);
mat.onLoaded([](bool ok) { /* главный поток, внутри update(); текстуры к этому моменту загружены */ });
const MaterialAsset* m = mat.getOrDefault();   // пока грузится — встроенный материал по умолчанию

AssetHandle<MeshData> mesh = assets.loadSync<MeshData>("Models/hill.obj#Mesh/0");   // блокирующая
std::shared_future<bool> f = mat.future();     // точка стыковки с корутинами (глава 08)

assets.onReloaded.connect([&](const ox::Uuid& id, AssetType type) { gpuCache.reupload(id); });
// каждый кадр, главный поток:
assets.update();   // колбэки, onLoaded/onFailed/onReloaded/onUnloaded, подмена при hot reload, выгрузка по LRU
```

В `Engine` всё это уже настроено: модуль `assets` регистрирует `AssetRegistry` (или `PakAssetSource`), `IAssetSource` и `AssetManager` как сервисы и вызывает `update()` каждый кадр на игровом потоке. Получить менеджер в игровом коде можно так: `services.get<ox::assets::AssetManager>()`.

Как работает загрузка:

- **Порядок.** Запросы выполняются на `JobSystem` в порядке приоритета: `kPriorityLow` (−100), `kPriorityNormal` (0), `kPriorityHigh` (100), `kPriorityCritical` (1000). Без `JobSystem` и через `loadSync` загрузка синхронная.
- **Зависимости** грузятся первыми: материал переходит в `Loaded` только когда готовы его текстуры. Сцена тянет все ассеты, на которые ссылается. Упавшая зависимость не роняет родителя — используйте `getOrDefault()`.
- **Состояния** (`AssetState`): `Unloaded`, `Queued`, `Loading`, `WaitingForDependencies`, `Loaded`, `Failed`.
- **Плейсхолдеры** для `getOrDefault()`: шахматная текстура 64×64, куб, материал по умолчанию. Встроенные ассеты (`builtin::checkerTexture()`, `whiteTexture()`, `flatNormalTexture()`, `cubeMesh()`, `defaultMaterial()`) всегда в памяти.
- **Ошибки.** Неизвестный UUID и несовпадение типа (`load<MeshData>` для текстуры: `"type mismatch"`) дают `Failed` и `error()`.
- **Время жизни.** `AssetHandle` считает ссылки. Ассет без ссылок остаётся в LRU-кэше на `keepUnreferenced` штук или пока не превышен `memoryBudget`. `unloadUnused()` выгружает все такие ассеты сразу. Указатель из `get()` действителен до следующего `update()` после перезагрузки или выгрузки, для долгого хранения используйте `share()`.

| `AssetManager::Options` | По умолчанию | Смысл |
| --- | --- | --- |
| `memoryBudget` | 0 (без лимита) | При превышении выгружаются ассеты без ссылок, старые первыми. В `Engine` — `EngineConfig::assetMemoryBudget` |
| `keepUnreferenced` | 64 | Размер LRU ассетов без ссылок |
| `callbacksOnMainThread` | `true` | `false` — колбэки `onLoaded` выполняются на потоке, закончившем загрузку |

`assets.stats()` возвращает число загруженных, загружающихся, упавших и непривязанных ассетов, байты по типам и бюджет. Это удобно для отладочного оверлея ([глава 29](29-ui.md)).

Полный пример: `samples/guide_examples/31-assets/loading.cpp` (`AsyncLoadWithDependencies`, `SyncLoadSubAssetsAndFuture`, `PlaceholdersAndErrors`, `RefCountingAndLru`).

## Шаг 6. Hot reload

```cpp
registry.startWatching();     // опрашивающий FileWatcher по Assets/
// каждый кадр:
registry.poll();              // реимпорт изменённых исходников и .meta → onReimported (+ зависимые)
assets.update();              // фоновая перезагрузка закончена → данные подменены → onReloaded
tex.generation();             // меняется при каждой перезагрузке: GPU-кэш сравнивает и перезаливает
```

В редакторе (и при `EngineConfig::fileWatching = true`) `Engine` сам вызывает `startWatching()` и `poll()`. Цепочка такая: сохранили PNG → реимпорт → `AssetManager` перезагружает текстуру в фоне → `update()` подменяет данные (тот же handle, новые байты) → `onReloaded` → рендерер перезаливает текстуру. Модуль gameplay превращает `onReloaded` в перезагрузку работающих скриптов, деревьев поведения, экземпляров префабов, аниматоров и мешей коллайдеров ([глава 32](32-gameplay-components.md)). Ассет, упавший при импорте, повторяется только после следующего изменения исходника.

Полный пример: `samples/guide_examples/31-assets/loading.cpp` (`HotReload`).

## Шаг 7. Сборка игры в `.oxpak`

В релизную сборку попадает только то, что нужно игре: стартовые сцены и всё, что достижимо из них по зависимостям (префабы, меши, материалы, текстуры), плюс список «всегда включать». Список задаётся в `pack.json` в корне проекта:

```json
{"startupScenes": ["Levels/start.oxscene"], "alwaysInclude": ["Scripts/", "Prefabs/"]}
```

Элементы `alwaysInclude` — пути ассетов, UUID или префиксы папок (`"Prefabs/"`). Это нужно для того, что грузится по имени и в зависимостях не видно: Lua-скрипты, префабы для `scene.spawn`, сетевые типы. Элемент, который ничего не нашёл, попадает в `report.errors`. Если ни сцен, ни включений нет (и нет `pack.json`), пакуется всё.

В пак попадают только **ассеты** (файлы, у которых есть импортёр) и `.oxproj`. Файлы без импортёра — разметка RmlUi (`.rml`, `.rcss`), произвольный JSON с данными — сейчас не пакуются, и собранная игра их не увидит.

```cpp
#include <oxwald/assets/cook.hpp>

CookOptions options;                                // pack.json дополняет эти списки
options.startupScenes = {"Levels/start.oxscene"};
auto report = cookProject(registry, "Build/Game.oxpak", options);
for (const auto& item : report->items) { /* item.path, item.type, item.size, item.storedSize */ }
report->errors; report->pakSize;
```

| `CookOptions` | По умолчанию | Смысл |
| --- | --- | --- |
| `startupScenes` | — | Пути сцен относительно `Assets/` или UUID |
| `alwaysInclude` | — | Пути, UUID или префиксы папок |
| `includeAll` | `false` | Всё из проекта (то же самое, если оба списка пусты) |
| `compress`, `compressionLevel` | `true`, 6 | zstd на каждую запись, если это экономит больше 1/16. Текстуры не сжимаются: их мипы читаются диапазонами |
| `alignment` | 16 | Выравнивание записей (удобно для mmap) |

Что лежит в паке: `assets/<uuid><ext>` для каждого артефакта, `catalog.oxcat` (JSON: UUID → тип, путь исходника, запись, зависимости) и файл(ы) `<Name>.oxproj` в корне. У каждой записи есть CRC32, у оглавления — свой CRC. Читатель на POSIX отображает файл в память (`PakReader::view` даёт span без копирования).

Собранная игра работает без импортёров, только с паками:

```cpp
PakAssetSource source;
source.addPak("Game.oxpak");
source.addPak("Patch1.oxpak");                     // последующие паки перекрывают предыдущие — патчи
AssetManager assets(source, &jobs);                // пути и UUID те же, что в редакторе
auto scene = assets.load<SceneAsset>("Levels/start.oxscene");
vfs.mount("game", std::make_unique<PakMountSource>(*PakReader::open("Game.oxpak")));   // сырые файлы пака
```

Мипы текстур читаются диапазонами прямо из пака: `readArtifactRange` → `readTextureInfo` → `readMipRange`. На этом построен стриминг мипов ([глава 27](27-gpu-driven-performance.md)).

Полные примеры: `samples/guide_examples/31-assets/pak.cpp` (`CookOnlyWhatTheGameUses`, `CookedGameLoadsFromPakOnly`).

## Шаг 8. Утилиты `oximport` и `oxpack`

Обе утилиты собираются в `build/<пресет>/tools/`.

**`oximport`** импортирует один файл теми же импортёрами, что и редактор, и печатает результат. Удобно, чтобы проверить модель до того, как класть её в проект. `.meta` при этом не создаётся.

```sh
oximport Assets/Models/hero.glb
oximport Assets/Models/hero.glb --settings '{"generateCollision": true, "lodRatios": [0.5, 0.2]}' --json
oximport Assets/Textures/rock.png --settings '{"type": "Linear"}' --out /tmp/rock
```

| Флаг | Смысл |
| --- | --- |
| `<file>` | Исходник (обязательный) |
| `--settings '<json>'` | Настройки импортёра поверх значений по умолчанию, ключи как в `.meta` |
| `--out <dir>` | Записать каждый артефакт как `<dir>/<имя или main><ext>` (`/` в имени заменяется на `_`) |
| `--json` | Машиночитаемая сводка: импортёр, время, артефакты (вершины, треугольники по LOD, меш-леты, границы, формат и мипы текстуры) |
| `-h`, `--help` | Справка |

Коды выхода: 0 — успех, 1 — нет импортёра или ошибка импорта, 2 — ошибка в аргументах.

**`oxpack`** собирает пак из проекта (это `cookProject`):

```sh
oxpack MyGame -o Build/Game.oxpak                                     # по pack.json или всё
oxpack MyGame -o Build/Game.oxpak --scene Levels/start.oxscene --include Scripts/ --verify
oxpack MyGame -o Build/Game.oxpak --all --no-compress --quiet
```

| Флаг | Смысл |
| --- | --- |
| `<project dir>` | Папка проекта с `Assets/` (обязательный) |
| `-o`, `--output <file>` | Выходной `.oxpak` (обязательный) |
| `--scene <path>` | Стартовая сцена (повторяемый) |
| `--include <path\|dir/\|uuid>` | Включить всегда (повторяемый) |
| `--all` | Всё из проекта |
| `--no-compress`, `--level <n>` | Без zstd; уровень zstd (по умолчанию 6) |
| `--align <bytes>` | Выравнивание записей (по умолчанию 16) |
| `--verify` | Перечитать пак, проверить CRC и каталог |
| `--quiet` | Без таблицы ассетов и info-логов |

`oxpack` печатает таблицу «тип, размер, сохранено, ассет» (крупные сверху) и итог: `N assets, X -> Y stored, pak Z`. Код выхода 1, если были ошибки ассетов или проверка не прошла.

## Шаг 9. Запуск плеера с `--pak`

```sh
oxpack MyGame -o Build/Game.oxpak --verify
OxwaldPlayer --pak Build/Game.oxpak                     # папка проекта не нужна: .oxproj лежит в паке
OxwaldPlayer --pak Build/Game.oxpak --scene project://Assets/Levels/test.oxscene --windowed --width 1280 --height 720
OxwaldPlayer --pak Build/Game.oxpak --headless --frames 600 --user-dir /tmp/smoke   # смоук-тест в CI
```

С `--pak` движок читает настройки проекта из `.oxproj` внутри пака, регистрирует `PakAssetSource` вместо `AssetRegistry` (импортёров нет) и монтирует пак в `project://`. Поэтому стартовая сцена `project://Assets/Levels/start.oxscene` и асинхронная смена уровней работают так же, как с папкой проекта. Остальные флаги плеера (окно, качество, cvar'ы, `--user-dir`) описаны в [главе 05](05-runtime.md).

```cpp
auto options = ox::parseLaunchOptions(args);   // {"--pak", "Game.oxpak", "--headless"}
ox::EngineConfig config = options->toEngineConfig("OxwaldPlayer");   // config.pakPath
ox::Engine engine;
engine.init(config);
engine.projectSettings().name;     // "MyGame" — прочитано из пака
engine.currentLevel();             // "project://Assets/Levels/start.oxscene"
```

Паки-патчи задаются в `EngineConfig::patchPaks`: они монтируются после основного и перекрывают его. Отдельного флага командной строки для них нет.

Полный пример: `samples/guide_examples/31-assets/pak_player.cpp` (таргет `ox_guide_assets_player`).

## Шаг 10. Свой импортёр

```cpp
class DialogImporter final : public IAssetImporter {
public:
    std::string_view name() const override { return "dialog"; }
    u32 version() const override { return 1; }   // увеличьте — и все .dialog переимпортируются
    std::vector<std::string> extensions() const override { return {".dialog"}; }
    AssetType mainType() const override { return AssetType::Raw; }
    nlohmann::ordered_json defaultSettings() const override { return {{"trim", true}}; }

    Status import(ImportContext& ctx) override {
        auto bytes = ctx.readSource();
        if (!bytes) return bytes.error();
        const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        const auto lines = std::count(text.begin(), text.end(), '\n');
        if (lines == 0) ctx.warn("пустой диалог");
        const nlohmann::ordered_json info = {{"lines", lines}, {"trim", ctx.meta().settings.value("trim", true)}};
        ctx.setMain(AssetType::Raw, serializeBlob(AssetType::Raw, info, *bytes));
        return {};
    }
};

registry.importers().add(std::make_unique<DialogImporter>());   // до scan(); поздняя регистрация побеждает
auto dialog = assets.loadSync<RawAsset>("Dialogs/intro.dialog");
dialog->info["lines"];
```

В `import` доступны:

- `ctx.settings<T>()` — отражённая структура настроек из `.meta`;
- `ctx.addSubAsset(name, type, bytes)` — под-ассеты;
- `artifact.dependencies` — зависимости;
- `ctx.resolveAsset(path, settingsHint)` — UUID соседнего исходника (`.meta` создаётся при необходимости);
- `ctx.addSourceDependency(path)` — реимпорт при изменении доп. файла;
- `ctx.warn(...)` — предупреждение.

Для собственного `AssetType` (от `FirstCustom`) зарегистрируйте загрузчик: `assets.registerLoader(type, loader)`. Модуль gameplay так добавляет импортёры `.oxbt` (деревья поведения) и `.oxanimctrl` (контроллеры аниматора): `gameplay::registerGameplayImporters(registry.importers())`.

Полный пример: `samples/guide_examples/31-assets/database.cpp` (`CustomImporter`, `StandaloneImportLikeOximport`).

## Типичные ошибки и подводные камни

- **`.meta` не в VCS.** У коллеги UUID сгенерируются заново, и все ссылки в сценах и материалах сломаются. Коммитьте `.meta` вместе с исходником, а `.oxcache/` добавьте в игнор.
- **Перенос файла без `.meta` из проводника.** Обычно база находит `.meta` по хэшу содержимого. Но если файл перенесли и одновременно изменили, связь потеряется. Переносите в Content Browser или вместе с `.meta`.
- **Копия `.meta` вручную.** Копия получит новый UUID (`duplicatesFixed`), и это правильно. Ссылаться на копию нужно по её новому UUID.
- **Маски как `Color`.** ORM, roughness и маски в sRGB искажаются. Называйте такие файлы `*_orm.png` или выставляйте `type: Linear`.
- **`get()` хранится между кадрами.** После hot reload или выгрузки указатель становится недействителен на следующем `update()`. Храните handle или `share()`.
- **Забытый `assets.update()`.** Без него не придут колбэки `onLoaded`, не применится hot reload и не сработает выгрузка. `Engine` вызывает его сам, свой цикл — нет.
- **Циклы зависимостей** (материал A → текстура → … → A) не обнаруживаются, и загрузчик зависнет.
- **Скрипты и префабы, загружаемые по имени, не попали в пак.** `cookProject` видит только зависимости сцен. Добавьте `"Scripts/"` и папку префабов в `alwaysInclude`.
- **Интерфейс и данные без импортёра не попали в пак.** `.rml`, `.rcss` и свои `.json` не являются ассетами, поэтому `oxpack` их не пакует. Пока такой контент нужно поставлять рядом с игрой или оформлять своим импортёром (шаг 10).
- **`project://`-пути в паке.** Стартовая сцена должна лежать в `Assets/`: пак отдаёт ассеты по пути `project://<assetDir>/<путь>`. Файлы вне `Assets/` (кроме `.oxproj`) в пак не попадают.
- **Импорт блокирует базу.** Операции `AssetRegistry` сериализуются одним мьютексом: пока импортируется большая модель, другие запросы ждут. Импортируйте заранее (`importAll()` при открытии проекта) или через `oximport`.
- **HDR и BC6H.** Кодировщика BC6H нет, HDR хранится в RGBA16F (8 байт на тексель). Ограничивайте `maxSize` у HDRI.
- **Что не импортируется.** Морфы (morph targets), камеры и источники света из glTF, сжатие KHR_draco/meshopt. Импорт скиннинга (кости сопоставляются со скелетом модуля animation по именам) пока не проверен тестами на реальном skinned-файле.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`assets.hpp`](../../engine/assets/include/oxwald/assets/assets.hpp) | Зонтичный заголовок |
| [`asset_types.hpp`](../../engine/assets/include/oxwald/assets/asset_types.hpp) | `AssetType`, `builtin::*`, `registerAssetTypes` |
| [`asset_registry.hpp`](../../engine/assets/include/oxwald/assets/asset_registry.hpp) | `AssetRegistry`, `AssetInfo`, `ScanResult`, `ImportStats` |
| [`asset_meta.hpp`](../../engine/assets/include/oxwald/assets/asset_meta.hpp) | `AssetMeta`, `readMeta`/`writeMeta`, `toSettingsJson`/`fromSettingsJson` |
| [`importer.hpp`](../../engine/assets/include/oxwald/assets/importer.hpp) | `IAssetImporter`, `ImportContext`, `ImporterRegistry`, `importStandalone` |
| [`asset_manager.hpp`](../../engine/assets/include/oxwald/assets/asset_manager.hpp) | `AssetManager`, приоритеты `kPriority*`, `AssetStats` |
| [`asset_handle.hpp`](../../engine/assets/include/oxwald/assets/asset_handle.hpp) | `AssetHandle<T>`, `UntypedAssetHandle`, `AssetState` |
| [`asset_source.hpp`](../../engine/assets/include/oxwald/assets/asset_source.hpp) | `IAssetSource`, `AssetRecord` |
| [`texture_import.hpp`](../../engine/assets/include/oxwald/assets/texture_import.hpp) | `TextureImportSettings`, `importTexture*`, BC7/BC5, `decompressToRGBA8` |
| [`texture.hpp`](../../engine/assets/include/oxwald/assets/texture.hpp) | `TextureData`, `TextureFormat`, `.oxtex`, `readTextureInfo`, `readMipRange` |
| [`image.hpp`](../../engine/assets/include/oxwald/assets/image.hpp) | `Image`, загрузка картинок, мипы |
| [`model_import.hpp`](../../engine/assets/include/oxwald/assets/model_import.hpp) | `ModelImportSettings`, `importModel`, `importMeshFile` |
| [`mesh.hpp`](../../engine/assets/include/oxwald/assets/mesh.hpp) | `MeshData`, `Submesh`, `MeshLod`, `Meshlet`, `.oxmesh` |
| [`mesh_processing.hpp`](../../engine/assets/include/oxwald/assets/mesh_processing.hpp) | `MeshProcessSettings`, касательные, LOD, меш-леты, коллизия |
| [`material.hpp`](../../engine/assets/include/oxwald/assets/material.hpp) | `MaterialAsset`, `saveMaterial`, `loadMaterial` |
| [`asset_data.hpp`](../../engine/assets/include/oxwald/assets/asset_data.hpp) | `SceneAsset`, `PrefabAsset`, `ScriptAsset`, `BlobAsset` и наследники, `serializeBlob` |
| [`pak.hpp`](../../engine/assets/include/oxwald/assets/pak.hpp) | `PakWriter`, `PakReader`, `PakMountSource`, `PakAssetSource` |
| [`cook.hpp`](../../engine/assets/include/oxwald/assets/cook.hpp) | `CookOptions`, `CookReport`, `cookProject` |

Утилиты: [`tools/oximport/main.cpp`](../../tools/oximport/main.cpp), [`tools/oxpack/main.cpp`](../../tools/oxpack/main.cpp). Плеер: [`apps/player/main.cpp`](../../apps/player/main.cpp), разбор флагов — [`runtime/launch.hpp`](../../engine/runtime/include/oxwald/runtime/launch.hpp). Форматы файлов (`.oxmesh`, `.oxtex`, `.oxpak`) и заметки для разработчиков: [`docs/dev/modules/assets.md`](../dev/modules/assets.md).

## Что дальше

- [19. Материалы](19-materials.md) — все поля материала, режимы смешивания, текстуры и инстансы.
- [18. Обзор рендеринга](18-rendering-overview.md) — как рендерер заливает меши и текстуры на GPU и реагирует на `onReloaded`.
- [27. GPU-driven и производительность](27-gpu-driven-performance.md) — меш-леты, LOD и стриминг мипов.
- [32. Компоненты ECS](32-gameplay-components.md) — `MeshRenderer`, `Script`, `AudioSource` и другие компоненты, которые ссылаются на ассеты.
- [30. Редактор](30-editor.md) — Content Browser, миниатюры, настройки импорта в инспекторе.
- [05. Runtime](05-runtime.md) — проект `.oxproj`, уровни и остальные флаги OxwaldPlayer.
- [Оглавление](README.md).
