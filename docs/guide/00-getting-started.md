# 00. Начало работы

## Зачем

Эта глава доводит вас от чистого checkout'а до работающего движка: какие нужны инструменты, как поставить
зависимости, собрать движок и тесты, запустить OxwaldPlayer и написать первую программу, которая крутит
игровой цикл. Попутно — как устроен репозиторий и что такое «модуль» движка.

## Требования

| Что | Версия / примечание |
| --- | --- |
| ОС | macOS (Apple Silicon, триплет `arm64-osx`), Windows, Linux |
| Компилятор | C++20. На macOS — **AppleClang** из Xcode Command Line Tools (`xcode-select --install`); тот же компилятор, что собирает порты vcpkg |
| CMake | ≥ 3.25 |
| Генератор | Ninja (используется всеми пресетами) |
| vcpkg | любой свежий checkout; переменная окружения `VCPKG_ROOT` должна указывать на него |
| Vulkan | 1.2+ с набором возможностей 1.3 (dynamic rendering, synchronization2, descriptor indexing, buffer device address, timeline semaphores). Загрузчик, заголовки и слои валидации ставятся из vcpkg |
| macOS: Vulkan | **MoltenVK** — ставится из vcpkg автоматически (порт из `vcpkg-overlays/ports`), отдельно Vulkan SDK не нужен |
| macOS: для редактора (Qt) | `brew install autoconf autoconf-archive automake libtool pkg-config` — хост-инструменты autotools для некоторых портов (например, libb2, который тянет Qt) |

Все сторонние библиотеки (glm, EnTT, Jolt, Lua/sol2, miniaudio, Recast, ENet, Tracy, Qt, …) приходят **только из
vcpkg** (манифест `vcpkg.json`). FetchContent, git-сабмодули и библиотеки из Homebrew не используются.

> Ray tracing и DLSS требуют соответствующего GPU (DLSS — только NVIDIA RTX на Windows/Linux). Без них движок
> работает, просто эти опции будут недоступны. На MoltenVK/Apple GPU ray tracing выключен.

## Установка зависимостей

Зависимости ставятся один раз в `<repo>/vcpkg_installed` и общие для всех каталогов сборки:

```sh
export VCPKG_ROOT=~/vcpkg            # путь к вашему checkout'у vcpkg
tools/bootstrap.sh                   # всё: движок, плеер, тесты и Qt-редактор
tools/bootstrap.sh --no-editor       # без Qt — заметно быстрее, если редактор не нужен
```

Скрипт вызывает `vcpkg install --x-install-root=vcpkg_installed` в режиме манифеста. На macOS он дополнительно
выбирает триплет `arm64-osx`, убирает из окружения `CC/CXX/LDFLAGS` (чтобы Homebrew LLVM не попал в сборку портов)
и подкладывает `glibtoolize` под именем `libtoolize`. Первая установка с Qt занимает долго — это нормально.

## Сборка

Проект использует CMake-пресеты (`CMakePresets.json`):

| Пресет | Тип сборки | Назначение |
| --- | --- | --- |
| `debug` | Debug | слои валидации Vulkan, Tracy |
| `dev` | RelWithDebInfo | повседневная разработка (по умолчанию) |
| `release` | Release | без тестов и без Tracy |

```sh
cmake --preset dev               # конфигурация -> build/dev
cmake --build --preset dev       # собрать всё
ctest --preset cpu               # тесты, которым не нужен GPU
ctest --preset dev               # все тесты (GPU-тесты сами пропускаются, если устройства нет)
```

Полезные опции CMake: `OX_BUILD_TESTS` (ON), `OX_BUILD_EDITOR` (ON), `OX_ENABLE_TRACY` (ON),
`OX_ENABLE_DLSS` (ON), `OX_WARNINGS_AS_ERRORS` (OFF).

### Сборка только части движка

Опция `OX_MODULES` ограничивает набор конфигурируемых модулей и верхнеуровневых каталогов (`editor`, `apps`,
`tools`, `samples`). Это удобно, когда вы работаете над одной областью, а остальное в процессе правки
(или просто чтобы собирать быстрее). Берите отдельный каталог сборки:

```sh
cmake --preset dev -B build/my-ai -DVCPKG_MANIFEST_INSTALL=OFF -DOX_MODULES="core;scene;ai"
cmake --build build/my-ai --target ox_ai_tests
ctest --test-dir build/my-ai -L ai
```

`-DVCPKG_MANIFEST_INSTALL=OFF` говорит CMake не запускать установку vcpkg заново: всё уже лежит в
`vcpkg_installed`. Тесты каждого модуля помечены меткой с его именем (`-L ai`), GPU-тесты — дополнительно `gpu`,
примеры из этого руководства — `guide`.

## Запуск OxwaldPlayer

`OxwaldPlayer` (`apps/player`) — автономный рантайм игры без редактора. Он собирается, когда сконфигурирован модуль
`runtime`, и лежит в `build/<preset>/bin/`.

```sh
build/dev/bin/OxwaldPlayer --project path/to/MyGame          # окно, проект
build/dev/bin/OxwaldPlayer --headless --frames 600           # без окна, 600 кадров и выход
build/dev/bin/OxwaldPlayer --server --fps 60                 # выделенный сервер: без рендера и звука
build/dev/bin/OxwaldPlayer --project MyGame --quality low --cvar r.VSync=false --cvar t.MaxFPS=144
```

По завершении плеер печатает строку вида `OxwaldPlayer: frames=600 fixedSteps=... fps=... renderer=Null`.

| Флаг | Значение |
| --- | --- |
| `--project <dir\|file.oxproj>` | проект: каталог с `.oxproj` или сам файл |
| `--pak <file>` | собранная игра `.oxpak` (см. [31. Ассеты](31-assets.md)); без `--project` настройки проекта берутся из пакета |
| `--scene <uri\|path>` | стартовая сцена вместо указанной в проекте |
| `--headless` | без окна; рендерер-заглушка `NullRenderer`, звук офлайн |
| `--server` | выделенный сервер (подразумевает `--headless`, без рендера и звука) |
| `--frames <n>` | выйти после n кадров (0 — работать до закрытия) |
| `--width <px> --height <px>` | размер окна |
| `--fullscreen \| --borderless \| --windowed` | режим окна |
| `--monitor <i>` | монитор для полноэкранного режима |
| `--quality low\|medium\|high\|ultra` | общий уровень качества (см. [04](04-cvars-quality.md)) |
| `--cvar name=value` | переопределить cvar (можно повторять) |
| `--fps <n>` | ограничение кадров в секунду |
| `--fixed-rate <hz>` | частота шага `FixedUpdate` |
| `--single-thread` | без отдельного потока рендера (отладка) |
| `--user-dir <dir>` | корень `user://` (настройки, сохранения) |
| `--help` | справка |

Плеер рисует через Vulkan-рендерер ([18. Рендеринг](18-rendering-overview.md)) с ImGui-оверлеем (F1) и игровым
UI на RmlUi ([29. UI](29-ui.md)). В режиме `--headless` кадр рендерится offscreen, а выделенный сервер (`--server`)
работает с `NullRenderer` без рендера.

### Запуск редактора

Если сборка шла с Qt (`OX_BUILD_EDITOR=ON`, по умолчанию), рядом с плеером лежит OxwaldEditor:

```sh
build/dev/bin/OxwaldEditor.app/Contents/MacOS/OxwaldEditor                      # окно выбора проекта
build/dev/bin/OxwaldEditor.app/Contents/MacOS/OxwaldEditor --project MyGame.oxproj
```

Вне macOS (без app bundle) исполняемый файл лежит прямо в `build/dev/bin/`. Тур по редактору — в главе [30. Редактор](30-editor.md).

Тот же разбор аргументов доступен вашей программе: `ox::parseLaunchOptions` возвращает `LaunchOptions`, а
`toEngineConfig()` превращает их в конфигурацию движка.

```cpp
#include <oxwald/runtime/launch.hpp>

const std::vector<std::string> args = {"--headless", "--frames", "5", "--quality", "low",
                                       "--cvar", "t.MaxFPS=0", "--user-dir", userDir.string()};
auto options = ox::parseLaunchOptions(args);       // или parseLaunchOptions(argc, argv)
if (!options) { /* options.error().message + ox::launchUsage() */ }

ox::EngineConfig config = options->toEngineConfig("MyGame");
ox::Engine engine;
engine.init(config);
engine.run(options->frames);
```

Полный пример: `samples/guide_examples/00-getting-started/launch_options.cpp`.

## Структура репозитория

```
engine/<module>/                 модули движка (по одной статической библиотеке)
    include/oxwald/<module>/     публичные заголовки -> #include <oxwald/<module>/foo.hpp>
    src/                         реализация
    tests/                       тесты GoogleTest -> ox_<module>_tests
engine/shaders/                  GLSL-шейдеры (#include от корня engine/shaders)
editor/                          редактор OxwaldEditor (Qt)
apps/player/                     OxwaldPlayer
tools/                           утилиты: oxdump, oximport, oxpack, ...
samples/                         пример проекта и компилируемые примеры этого руководства (guide_examples/)
docs/guide/                      это руководство
docs/dev/                        документация для разработчиков движка (ARCHITECTURE.md, modules/*.md)
vcpkg-overlays/                  собственные порты и триплеты vcpkg
```

## Как устроены модули

Каждый каталог `engine/<name>/` с `CMakeLists.txt` подхватывается автоматически. Модуль объявляется одной
функцией:

```cmake
ox_add_module(ai
    PUBLIC_DEPS  Oxwald::core RecastNavigation::Detour   # видны тем, кто линкуется с модулем
    PRIVATE_DEPS ...                                     # только внутри
    TEST_DEPS    ...                                     # дополнительно для тестов
    TEST_LABELS  gpu)                                    # метки тестов (gpu — нужен Vulkan)
```

Она создаёт статическую библиотеку `ox_<name>` с псевдонимом **`Oxwald::<name>`**, собирает исходники из `src/`,
делает публичными заголовки из `include/` и, если есть `tests/`, — исполняемый файл `ox_<name>_tests`.
В своём коде линкуйтесь с псевдонимом:

```cmake
add_executable(MyGame main.cpp)
target_link_libraries(MyGame PRIVATE Oxwald::runtime)   # runtime тянет core, scene и все доступные модули
```

Основные модули: `core` (база: лог, математика, рефлексия, cvar'ы, задачи…), `scene` (ECS), `rhi` (Vulkan),
`assets`, `render`, `physics`, `animation`, `spline`, `audio`, `ai`, `net`, `script` (Lua), `async` (корутины),
`world`, `gameplay`, `runtime` (`Engine`, игровой цикл, настройки, сохранения). Модуль `runtime` подключает
необязательные модули, которые есть в сборке, и определяет для них макросы `OX_HAS_<MODULE>=1` — поэтому он
собирается с любым подмножеством движка.

Все типы движка живут в пространстве имён `ox` (подпространства: `ox::script`, `ox::serial`, `ox::reflect`, …).
Система координат — правая, **Y вверх**, «вперёд» — `-Z`.

## Первая программа: движок без окна

Минимальный headless-запуск: движок создаёт свои сервисы (задачи, файловую систему, настройки, ввод,
сохранения, консоль), пустой мир и прокручивает 10 кадров.

```cpp
#include <oxwald/runtime/engine.hpp>

ox::Engine engine;
ox::EngineConfig config;
config.appName = "HelloOxwald";
config.headless = true;                     // без окна: NullRenderer
config.userDir = userDir;                   // user:// — настройки и сохранения
config.saveUserSettingsOnShutdown = false;

auto status = engine.init(config);          // ox::Status: проверяйте!
if (!status) { std::puts(status.error().message.c_str()); return 1; }

engine.run(10);                             // 10 кадров и выход (0 — до requestQuit())
// engine.stats().frame == 9 — индекс последнего кадра, счёт с нуля
engine.shutdown();                          // также вызывается из ~Engine
```

Полный пример: `samples/guide_examples/00-getting-started/hello_engine.cpp`.

Вместо `run()` можно крутить цикл самостоятельно: `engine.tick()` — один кадр с реальным временем,
`engine.tick(dt)` — кадр с заданной длительностью (тесты, редактор, lockstep-сервер).

## Первый проект

Игра — это **проект**: каталог с файлом `<Name>.oxproj` (обычный JSON настроек `ProjectSettings`) и каталогами
ассетов. Движок монтирует каталог проекта как `project://`.

```cpp
#include <oxwald/runtime/project.hpp>

ox::Project project = ox::Project::create(root / "MyGame", "MyGame");   // файл ещё не записан
project.settings.version = "0.1.0";
project.settings.defaultQuality = "Medium";
project.settings.physics.fixedRate = 60.0f;
project.save();                                                          // -> MyGame/MyGame.oxproj

ox::EngineConfig config;
config.projectPath = root / "MyGame";       // каталог или сам .oxproj
config.headless = true;
config.userDir = root / "user";

ox::Engine engine;
engine.init(config);
auto text = engine.vfs().readText("project://config/hello.txt");          // файлы проекта
```

Полный пример: `samples/guide_examples/00-getting-started/first_project.cpp`.

Получившийся `.oxproj` можно править руками — отсутствующие поля получают значения по умолчанию:

```json
{
  "name": "MyGame",
  "version": "0.1.0",
  "startupScene": "project://levels/main.oxscene",
  "modules": { "net": false },
  "defaultQuality": "Medium",
  "physics": { "fixedRate": 60.0 }
}
```

## Сборка примеров руководства

Все нетривиальные фрагменты кода руководства — компилируемые тесты в `samples/guide_examples/<глава>/`.
Они собираются при `OX_BUILD_TESTS=ON` и имеют метку `guide`:

```sh
cmake --build build/dev --target ox_guide_examples
ctest --test-dir build/dev -L guide
ctest --test-dir build/dev -L guide -R GuideGettingStarted     # только эта глава
```

## Типичные ошибки и подводные камни

- **`VCPKG_ROOT` не задан** — пресеты берут toolchain из `$env{VCPKG_ROOT}`; без него конфигурация падает сразу.
- **Homebrew LLVM в `CC/CXX`** (macOS). Движок принудительно собирается AppleClang (`OX_USE_APPLE_CLANG=ON`), чтобы
  совпадать с портами vcpkg. Если вы отключили это, получите ошибки линковки libc++.
- **Нет autotools на macOS** — установка Qt падает на порте libb2. Поставьте пакеты из таблицы требований или
  используйте `tools/bootstrap.sh --no-editor`.
- **Повторная установка vcpkg при каждой конфигурации.** В своих каталогах сборки передавайте
  `-DVCPKG_MANIFEST_INSTALL=OFF`.
- **`OX_MODULES` без зависимостей.** Если вы перечислили `runtime`, но забыли `scene`, конфигурация упадёт:
  указывайте модуль вместе с тем, от чего он зависит (`core;scene;runtime`).
- **Результат `engine.init()` проигнорирован.** `init` возвращает `ox::Status`; при ошибке (битый проект, нет
  стартовой сцены) движок не готов, и следующие вызовы упадут на assert.
- **Настройки пишутся в домашний каталог.** Без `userDir` движок использует `paths::userDataDir(<имя проекта>)`
  (`~/Library/Application Support/<app>` на macOS). В тестах и инструментах задавайте временный `userDir` и
  `saveUserSettingsOnShutdown = false`.
- **Ожидание картинки в плеере.** Пока рендерер не подключён к плееру, окно остаётся пустым (см. выше).

## API

- [`engine.hpp`](../../engine/runtime/include/oxwald/runtime/engine.hpp) — `Engine`, `EngineConfig`, `EngineStats`, `IEngineModule`
- [`launch.hpp`](../../engine/runtime/include/oxwald/runtime/launch.hpp) — `LaunchOptions`, `parseLaunchOptions`, `launchUsage`
- [`project.hpp`](../../engine/runtime/include/oxwald/runtime/project.hpp) — `Project`, `ProjectSettings`
- [`runtime.hpp`](../../engine/runtime/include/oxwald/runtime/runtime.hpp) — зонтичный заголовок рантайма
- [`apps/player/main.cpp`](../../apps/player/main.cpp) — исходник OxwaldPlayer: хороший образец своего `main`
- [`cmake/OxwaldHelpers.cmake`](../../cmake/OxwaldHelpers.cmake) — `ox_add_module`
- [`tools/bootstrap.sh`](../../tools/bootstrap.sh), [`CMakePresets.json`](../../CMakePresets.json), [`vcpkg.json`](../../vcpkg.json)

## Что дальше

- [01. Ядро (core)](01-core.md) — лог, `Result`, математика, сервисы, события, задачи, файлы.
- [02. Рефлексия и сериализация](02-reflection-serialization.md) — как описывать свои типы и сохранять их.
- [04. CVar'ы и качество графики](04-cvars-quality.md) — консоль, `--cvar`, `--quality`, настройки игрока.
- [05. Рантайм](05-runtime.md) — подробно об `Engine`, модулях, игровом цикле и потоке рендера.
- [Оглавление](README.md)
