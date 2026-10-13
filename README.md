# OxwaldEngine

OxwaldEngine — игровой движок на C++20 с рендерером на Vulkan, ECS на EnTT, редактором на Qt 6 и скриптами на Lua.
Движок разбит на независимые модули (статические библиотеки `Oxwald::<module>`): `core`, `scene`, `rhi`, `assets`,
`render`, `physics`, `animation`, `spline`, `audio`, `ai`, `net`, `script`, `async`, `ui`, `world`, `gameplay`,
`runtime`. Поверх них собираются автономный рантайм **OxwaldPlayer** и редактор **OxwaldEditor**. Все сторонние
зависимости ставятся через vcpkg.

![Редактор OxwaldEditor](docs/guide/images/editor/main_window_dark.png)

Демо-проект [OxwaldShowcase](samples/OxwaldShowcase/README.md) — хаб и 14 станций, по одной на подсистему движка:

| Хаб | Волюметрика | Вода и частицы |
| --- | --- | --- |
| ![Хаб](docs/guide/images/showcase/hub.png) | ![Волюметрика](docs/guide/images/showcase/volumetrics.png) | ![Вода](docs/guide/images/showcase/water.png) |
| **Отражения** | **Мир** | **RTX и апскейлеры** |
| ![Отражения](docs/guide/images/showcase/reflections.png) | ![Мир](docs/guide/images/showcase/world.png) | ![RTX](docs/guide/images/showcase/rtx.png) |

Эталонные изображения рендерера из тестов (`engine/render/tests/data/golden/`):

| PBR-материалы | Каскадные тени | Вода | Объёмные облака |
| --- | --- | --- | --- |
| ![PBR](engine/render/tests/data/golden/pbr_spheres_grid.png) | ![CSM](engine/render/tests/data/golden/csm_shadows.png) | ![Вода](engine/render/tests/data/golden/translucency_water.png) | ![Облака](engine/render/tests/data/golden/volumetrics_clouds_sunset.png) |

## Возможности

**Ядро и сцена**
- `core`: логирование, assert, `Result<T>`, математика (glm, Y-up, reversed-Z), UUID, сервисы (DI), события, job system
  (enkiTS), рефлексия типов с атрибутами, VFS, слежение за файлами, debug draw, профилирование через Tracy.
- Сериализация через рефлексию: бинарный формат OXB1 (версионирование полей, CRC) и JSON без потерь в обе стороны,
  утилита `oxdump`.
- CVar'ы и группы масштабируемости Low/Medium/High/Ultra в духе UE, автоопределение качества по GPU-бенчмарку.
- `scene`: ECS на EnTT (`World`/`Entity`), иерархия и трансформы, системы по фазам, сохранение сцен, префабы с overrides.
- Сохранения (в `runtime`): слоты, метаданные, поля с атрибутом `SaveGame`, секции `ISaveable`, миграции версий,
  атомарная запись, автосейв, асинхронное сохранение и загрузка.

**Рендеринг** (`rhi` + `render`)
- RHI на Vulkan: bindless, render graph с автоматическими барьерами и алиасингом, компиляция GLSL через shaderc
  с hot reload, BLAS/TLAS.
- Clustered forward+ PBR, IBL, тени: CSM, spot-тени в атласе, точечные тени, PCF/PCSS.
- Отражения: пробы с box projection, SSR, плоские зеркала; GTAO; irradiance volumes.
- Froxel-туман, объёмный свет и объёмные облака.
- Прозрачность: OIT, преломление, вода, GPU-частицы.
- Ray tracing: RT-тени, отражения, AO, DDGI, преломление; ReSTIR DI, path tracer, денойзер в стиле SVGF.
- Сглаживание и апскейлеры: TAA, TAAU, FXAA, FSR 1, DLSS (NGX).
- Постобработка: автоэкспозиция, bloom, DoF, motion blur, цветокоррекция и LUT, тонмаппинг ACES/AgX/Neutral.
- GPU-driven: отсечение на GPU (frustum + HiZ occlusion), LOD, меш-шейдеры и meshlets, async compute, стриминг мипов.
- Мир: ландшафт, растительность, небо; скиннинг в compute-шейдере.

**Игровые подсистемы**
- `physics`: Jolt — тела, формы, слои коллизий, запросы, соединения, контроллер персонажа.
- `animation`: скелеты, клипы, blend spaces, state machine, root motion, IK, скиннинг (CPU и GPU).
- `spline`: Безье, Catmull-Rom, B-сплайны/NURBS, параметризация по длине дуги, движение по пути, экструзия мешей.
- `audio`: miniaudio — 3D-звук, шины и микшер, эффекты, окклюзия.
- `ai`: Recast/Detour (навмеш, толпа, динамические препятствия), behavior trees, utility AI, восприятие.
- `net`: ENet, репликация с дельта-сжатием, интерполяция, предсказание на клиенте, выделенный сервер.
- `script`: Lua 5.4 + sol2, песочница, скрипты сущностей, hot reload.
- `async`: корутины C++20 (`Task`/`Future`, ожидание кадров и времени, переходы между потоками, `whenAll`/`whenAny`,
  отмена), `await` из Lua.
- `ui`: отладочный оверлей на Dear ImGui (статистика, консоль, cvar'ы, инспектор, render graph, корутины) и игровой
  UI на RmlUi (модели данных, hot reload, Lua).
- `world`: ландшафт с эрозией и CDLOD, растительность, солнце и луна, смена дня и ночи, ветер и погода, вода
  Герстнера с плавучестью, стриминг чанков.
- `gameplay`: ECS-компоненты и системы, связывающие все модули с миром.

**Приложения**
- `runtime` + OxwaldPlayer: `Engine`, игровой цикл с отдельным потоком рендера, ввод в стиле Enhanced Input, проекты
  `.oxproj`, пользовательские настройки, консоль, headless-режим и выделенный сервер.
- OxwaldEditor (Qt 6): вьюпорт на Vulkan, outliner, инспектор на основе рефлексии, браузер контента с миниатюрами,
  undo/redo, play-in-editor, инструменты сплайнов и ландшафта, настройки проекта и масштабируемости, интерфейс RU/EN.
- Инструменты: `oximport` (импорт ассетов), `oxpack` (упаковка проекта в `.oxpak`), `oxdump` (бинарные файлы ↔ JSON).

## Требования

- CMake ≥ 3.25, Ninja.
- [vcpkg](https://github.com/microsoft/vcpkg), переменная окружения `VCPKG_ROOT` указывает на его checkout.
- Компилятор с поддержкой C++20. На macOS используется AppleClang (`/usr/bin/clang++`), тот же, что собирает порты
  vcpkg; также нужны Xcode Command Line Tools и `brew install autoconf autoconf-archive automake libtool pkg-config`
  (инструменты сборки для некоторых портов). На Windows — Visual Studio 2022 (MSVC v143) с компонентом «Desktop
  development with C++»; подробности — в разделе [Сборка на Windows](#сборка-на-windows).
- GPU с Vulkan 1.2+ и набором возможностей 1.3 (dynamic rendering, synchronization2, descriptor indexing,
  buffer device address, timeline semaphores). На macOS MoltenVK собирается из overlay-порта vcpkg. На Windows
  загрузчик Vulkan и слои валидации приходят из vcpkg, драйвер — от производителя GPU.
- Git LFS на Windows и Linux: overlay-порт `nvidia-dlss` забирает бинарные файлы DLSS SDK через LFS.
- Для редактора — Qt 6 из vcpkg. Первая сборка Qt занимает много времени; без редактора можно обойтись
  (`--no-editor`).

## Быстрый старт

```sh
tools/bootstrap.sh                 # все зависимости через vcpkg в ./vcpkg_installed (включая Qt для редактора)
tools/bootstrap.sh --no-editor     # без Qt: движок, плеер и тесты

cmake --preset dev                 # конфигурация в build/dev (RelWithDebInfo); есть также debug и release
cmake --build --preset dev         # сборка всего
ctest --preset dev                 # все тесты
ctest --preset cpu                 # только тесты, которым не нужен GPU
```

Запуск плеера:

```sh
build/dev/bin/OxwaldPlayer --project path/to/MyGame          # окно, проект (папка или файл .oxproj)
build/dev/bin/OxwaldPlayer --headless --frames 600           # без окна, 600 кадров и выход
build/dev/bin/OxwaldPlayer --server --fps 60                 # выделенный сервер
build/dev/bin/OxwaldPlayer --project MyGame --quality low --cvar r.VSync=false
build/dev/bin/OxwaldPlayer --help                            # все опции
```

Другие опции: `--pak`, `--patch-pak` (патч-паки поверх `--pak`, см. `oxpack --patch`), `--scene`, `--width`/`--height`, `--fullscreen`/`--borderless`/`--windowed`, `--monitor`,
`--user-dir`, `--fixed-rate`, `--single-thread`; аргументы после `--` передаются коду игры.

Запуск редактора (на macOS это бандл):

```sh
build/dev/bin/OxwaldEditor.app/Contents/MacOS/OxwaldEditor                        # окно выбора проекта
build/dev/bin/OxwaldEditor.app/Contents/MacOS/OxwaldEditor --project MyGame.oxproj
```

На Windows исполняемые файлы лежат прямо в `build\devin\` (`OxwaldPlayer.exe`, `OxwaldEditor.exe`).

### Сборка на Windows

Проверено на Windows 11 с NVIDIA RTX 3080 (драйвер 617.42): Visual Studio 2022 17.14 (MSVC 14.44), CMake 3.30,
Ninja 1.12.1. Собирается всё: движок, плеер, инструменты, тесты, примеры руководства, демо-проект и редактор.

Что поставить:
- Visual Studio 2022 с компонентом «Desktop development with C++»;
- CMake ≥ 3.25 и Ninja;
- Git и Git LFS (`git lfs install`) — без LFS не установится порт `nvidia-dlss` (он скачивает около 700 МБ);
- Python 3;
- свежий драйвер GPU;
- vcpkg: `git clone https://github.com/microsoft/vcpkg C:\devcpkg`, затем `bootstrap-vcpkg.bat` и переменная
  окружения `VCPKG_ROOT`.

Репозиторий клонируйте в короткий путь без пробелов. Переводы строк фиксирует `.gitattributes` (LF), настраивать
`core.autocrlf` не нужно. Все команды запускайте из **«x64 Native Tools Command Prompt for VS 2022»**, из корня
репозитория: Ninja должен видеть `cl.exe`.

```bat
%VCPKG_ROOT%cpkg install --x-install-root=vcpkg_installed --triplet x64-windows

cmake --preset dev -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build --preset dev
ctest --preset dev -j8 --output-on-failure
```

Установка зависимостей вместе с Qt заняла около 44 минут на i9-9900K (без бинарного кэша vcpkg). Пресет `dev`
собирается под MSVC с предупреждениями как ошибками (`/W4 /WX`).

Подводные камни:
- **Не запускайте тесты от имени администратора.** В процессах с повышенными правами загрузчик Vulkan игнорирует
  `VK_ADD_LAYER_PATH`, и слой валидации из каталога сборки не находится.
- **Запрос брандмауэра Windows** про сетевой доступ для тестовых exe — это клиент Tracy, который слушает порт.
  На результат тестов ответ не влияет.
- **Пропуск `DeviceLifecycle.SurfaceDeviceAfterHeadlessDevice`** — норма: драйвер NVIDIA на Windows не предоставляет
  `VK_EXT_headless_surface`.
- **Пути с не-ASCII символами** вне системной ANSI-кодировки пока не поддерживаются (см.
  [BACKLOG](docs/dev/BACKLOG.md)).

### Сборка на Linux

**На железе не проверялась.** Код и CMake для Linux есть (включая xcb-surface во вьюпорте редактора и рантайм DLSS
в overlay-порте), но ни сборка, ни тесты на Linux-машине не запускались. Ожидаемый порядок тот же, что на macOS:
`tools/bootstrap.sh` (триплет vcpkg по умолчанию), затем `cmake --preset dev` и `ctest --preset dev`. Нужны Git LFS
и dev-пакеты X11/xcb/Wayland для Qt и GLFW. Wayland-surface во вьюпорте редактора не реализован: там редактор
переключается на программный вьюпорт.

Полезные опции CMake:
- `-DOX_MODULES="core;scene;physics"` — сконфигурировать только перечисленные модули и каталоги верхнего уровня
  (`editor`, `apps`, `tools`, `samples`); удобно для частичной сборки.
- `-DOX_WARNINGS_AS_ERRORS=ON|OFF` — предупреждения как ошибки (включено в пресетах `dev` и `debug`; код в
  `samples/` получает предупреждения, но не ошибки).
- `-DOX_BUILD_TESTS=OFF`, `-DOX_ENABLE_TRACY=OFF`, `-DOX_ENABLE_DLSS=OFF`. Редактор собирается, если найден Qt 6;
  чтобы его пропустить, не включайте `editor` в `OX_MODULES` (или ставьте зависимости с `--no-editor`).

## Структура репозитория

```
engine/<module>/        модули движка: include/oxwald/<module>/ (публичные заголовки), src/, tests/
engine/shaders/         GLSL-шейдеры (корень для #include)
editor/                 редактор OxwaldEditor (Qt 6)
apps/player/            OxwaldPlayer — автономный рантайм игры
tools/                  bootstrap.sh, oxdump, oximport, oxpack, rhi_window_smoke
samples/                OxwaldShowcase (демо-проект), guide_examples (компилируемые примеры из руководства)
docs/guide/             руководство пользователя (на русском)
docs/dev/               документация для разработчиков движка
vcpkg-overlays/         overlay-порты (MoltenVK, NVIDIA DLSS, Vulkan) и триплеты vcpkg
```

## Документация

- [Руководство пользователя](docs/guide/README.md) — главы по каждой подсистеме с проверенными примерами кода.
- [Архитектура и соглашения для контрибьюторов](docs/dev/ARCHITECTURE.md).
- [Заметки по модулям](docs/dev/modules/) — API, ограничения, детали реализации.
- [История изменений](CHANGELOG.md).

## Платформы

| Платформа | Состояние |
| --- | --- |
| macOS arm64 (Apple GPU, MoltenVK) | Основная платформа разработки: полная сборка, все тесты |
| Windows 11 x64 (MSVC, NVIDIA RTX 3080) | Проверена: полная сборка с редактором, `ctest` (см. ниже) |
| Linux x64 | Код и CMake есть, на железе не проверялась |

На **macOS** нет аппаратного ray tracing, DLSS и меш-шейдеров: соответствующие эффекты отключены, переключатели в UI
неактивны и показывают причину, а GPU-тесты таких функций пропускаются (`GTEST_SKIP`), а не падают.

На **Windows с NVIDIA RTX** эти пути исполняются. На RTX 3080 проверены DLSS (все режимы качества и DLAA),
меш-шейдеры, `drawIndirectCount`, async compute и ray tracing: GPU-тесты всех RT-эффектов и path tracer'а в обоих
режимах проходят, станции демо-проекта отрисованы с `r.RayTracing` без ошибок валидации (что осталось непроверенным —
в [главе 24](docs/guide/24-ray-tracing.md)). Полный `ctest`: 1249 тестов, ни один не падает, 3 пропускаются по
делу: нет `VK_EXT_headless_surface`; тест неактивного переключателя RT на машине с RT; оконный скриншот в
headless-запуске. Редактор открывает демо-проект с Vulkan-вьюпортом и корректно завершается. Изменения после 0.1.0,
включая найденные портом ошибки, описаны в [истории изменений](CHANGELOG.md).

DLSS (NGX) доступен только на Windows и Linux с NVIDIA RTX.

## Лицензия

Лицензия проекта пока не выбрана. Сторонние зависимости, устанавливаемые через vcpkg, распространяются под своими
лицензиями.
