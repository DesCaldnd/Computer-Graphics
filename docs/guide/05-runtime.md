# 05. Runtime и игровой цикл

## Зачем

Модуль `runtime` (таргет `Oxwald::runtime`) собирает движок в работающую игру. Класс `ox::Engine`:

- создаёт службы (services): job system, VFS, настройки, ввод, сохранения, консоль, а также встроенные модули (физика, звук, Lua, корутины), если они есть в сборке;
- крутит игровой цикл с фиксированным шагом, паузой, замедлением времени и ограничением FPS;
- отдаёт каждый кадр рендереру, который работает в своём потоке;
- загружает проект `.oxproj`, уровни (синхронно, асинхронно, additive) и пользовательские настройки.

Ваша игра подключается к движку через **модули** (`IEngineModule`): в них создаются игровые службы, регистрируются типы и системы. Готовое приложение для запуска проекта без редактора — **OxwaldPlayer** (`apps/player`).

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `Engine` | Владелец служб, миров, планировщика систем и пайплайна рендера |
| `EngineConfig` | Параметры запуска: проект, headless, потоки, частота фиксированного шага, user-каталог, cvar'ы |
| `IEngineModule` | Точка расширения: `registerTypes`, `init`, `registerSystems`, `preUpdate`, `onWorldUnloading`, `onWorldChanged`, `shutdown` |
| `Services` | DI-контейнер ([глава 01](01-core.md)): `engine.services().get<T>()` |
| `IRenderer` | Контракт рендерера: `extract` (игровой поток) и `render` (поток рендера). По умолчанию `NullRenderer` |
| `RenderPipeline` | Передача кадров из игрового потока в поток рендера через два слота снимков |
| `Project` / `ProjectSettings` | Файл `<Имя>.oxproj` (JSON): стартовая сцена, модули, ввод, физика, звук, качество |
| Уровень | Мир, загруженный из `.oxscene`. Активный мир — `engine.world()` |
| VFS-схемы | `project://` — каталог проекта, `user://` — настройки и сохранения пользователя, `engine://` — данные движка |

## Шаг 1. Минимальный запуск

```cpp
#include <oxwald/runtime/engine.hpp>

ox::Engine engine;
ox::EngineConfig config;
config.appName = "HelloOxwald";
config.headless = true;           // без окна: NullRenderer, звук офлайн
config.userDir = userDir;         // user://; по умолчанию — системный каталог данных приложения
auto status = engine.init(config);
if (!status) { /* status.error().message */ }

engine.run(10);    // 10 кадров; run() без аргумента — до requestQuit() или закрытия окна
engine.shutdown(); // также вызывается из ~Engine
```

Вместо `run()` можно крутить кадры самому:

- `engine.tick()` — кадр с измеренным временем и ограничением FPS;
- `engine.tick(dt)` — кадр с заданной дельтой, без сна (тесты, редактор, lockstep-сервер).

Оба варианта возвращают `false`, когда запрошен выход.

Основные поля `EngineConfig`:

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `appName` | `"Oxwald"` | Имя приложения (каталог user-данных без проекта) |
| `projectPath` | пусто | `.oxproj` или его каталог |
| `projectSettings` | нет | Проект в памяти (тесты, инструменты), если нет `projectPath` |
| `startupScene` | из проекта | `""` — сцена проекта, `"-"` — без сцены, иначе URI или путь |
| `headless` | `false` | Без окна. `NullRenderer`, если рендерер не задан |
| `dedicatedServer` | `false` | Headless без звука и рендера |
| `threadedRendering` | `true` | Отдельный поток рендера. `false` — extract и render на игровом потоке (отладка) |
| `workerThreads` | 0 (все ядра) | Потоки job system, включая главный |
| `fixedRate`, `maxFixedSteps` | из проекта (60 Гц, 8) | Частота `FixedUpdate` и защита от «спирали смерти» |
| `targetFps` | 0 | Ограничение FPS (0 — cvar `t.MaxFPS` / без ограничения) |
| `maxFrameDelta` | 0.25 с | Длинный кадр (брейкпоинт) обрезается до этого значения |
| `userDir`, `engineDir` | авто | Корни `user://` и `engine://` |
| `editor` | `false` | Запуск в режиме редактирования (Edit mode) |
| `loadUserSettings`, `saveUserSettingsOnShutdown` | `true` | Чтение и запись `user://settings.json` |
| `quality`, `cvars` | нет | Аналоги `--quality` и `--cvar name=value` |

## Шаг 2. Свой модуль

Игровой код подключается через модуль: он создаёт службы, регистрирует системы и подписывается на события движка.

```cpp
struct ScoreService {
    int score = 0;
    int fixedTicks = 0;
};

class ScoreSystem final : public ox::ISystem {
public:
    std::string_view name() const override { return "Game.Score"; }
    ox::SystemPhase phase() const override { return ox::SystemPhase::FixedUpdate; }
    void update(ox::SystemContext& ctx) override { ++ctx.services.get<ScoreService>().fixedTicks; }
};

class GameModule final : public ox::IEngineModule {
public:
    std::string_view name() const override { return "game"; }
    void registerTypes() override { ox::registerSceneTypes(); /* + OX_REFLECT_TYPE своих компонентов */ }
    ox::Status init(ox::Engine& engine, ox::Services& services) override {
        services.emplace<ScoreService>();
        m_levelConn = engine.levelLoaded.connect([this](const std::string&) { ++levelsLoaded; });
        return {}; // ошибка здесь прерывает Engine::init и корректно откатывает уже созданное
    }
    void registerSystems(ox::Engine&, ox::SystemScheduler& scheduler) override { scheduler.emplace<ScoreSystem>(); }
    void preUpdate(ox::Engine& engine, const ox::FrameTime& time) override {
        if (!time.paused) engine.services().get<ScoreService>().score += 1; // раз в кадр, перед системами
    }
    void onWorldUnloading(ox::Engine&, ox::World&) override { /* отпустить ссылки на сущности старого мира */ }
    void shutdown(ox::Engine&, ox::Services&) override { m_levelConn.disconnect(); }

    int levelsLoaded = 0;

private:
    ox::Connection m_levelConn;
};

ox::Engine engine;
engine.addModule(std::make_unique<GameModule>()); // строго до init()
engine.init(config);
engine.services().get<ScoreService>();
```

**Порядок инициализации:** JobSystem → EventBus → FileWatcher → Vfs → DebugDraw → Settings → InputSystem → SaveGameSystem → Console → встроенные модули (physics, audio, script, async, …) → ваши модули в порядке `addModule` → `SystemScheduler` (+ `TransformSystem` и системы модулей) → стартовая сцена → `IRenderer::init` → запуск потока рендера.

При `shutdown()` всё идёт в обратную сторону: поток рендера дорисовывает кадры и останавливается, ожидаются фоновые загрузки и сохранения, пишутся пользовательские настройки, вызывается `onWorldUnloading`, уничтожаются миры, затем вызывается `shutdown` модулей в обратном порядке и уничтожаются службы.

Встроенный модуль можно выключить в проекте: `"modules": {"physics": false}`. Найти модуль по имени можно через `engine.findModule("game")`.

Полный пример: `samples/guide_examples/05-runtime/engine_module.cpp`.

## Шаг 3. Игровой цикл: фиксированный шаг, пауза, время

Что делает каждый `Engine::tick`:

1. События платформы (окно, клавиатура) и задачи главного потока.
2. Опрос FileWatcher (только редактор), рассылка EventBus, загрузка уровня (если запрошена).
3. `InputSystem::update`.
4. Время кадра: `dt = realDt × timeScale`, на паузе `dt = 0`.
5. `IEngineModule::preUpdate` для всех модулей.
6. `SystemScheduler::tick`: `PreUpdate`, `FixedUpdate` × N, `Update`, `PostUpdate`, `Extract`, удаление помеченных сущностей.
7. `SaveGameSystem::update`: завершение асинхронных операций, учёт игрового времени, автосейв.
8. `RenderPipeline::submit`: extract в слот снимка и передача кадра потоку рендера.
9. Ограничение FPS (`targetFps` / `t.MaxFPS`).

```cpp
const double step = 1.0 / 64.0;   // config.fixedRate = 64
engine.tick(step / 2);            // FixedUpdate не вызывается, stats().alpha == 0.5
engine.tick(step * 1.5);          // накопилось 2 шага: stats().fixedSteps == 2

engine.setPaused(true);           // FixedUpdate стоит; Update идёт с dt = 0 (камера, меню)
engine.stepFrames(1);             // покадровая отладка: следующий кадр делает ровно 1 фиксированный шаг
engine.setPaused(false);
engine.setTimeScale(0.5);         // замедление (0 — стоп, отрицательные значения обрезаются до 0)
```

**Интерполяция.** Физика и логика живут в `FixedUpdate`, а кадры рисуются с произвольной частотой. Чтобы движение было плавным, рендерер интерполирует трансформы между `WorldTransformComponent::previous` (состояние до шагов этого кадра) и `matrix` с коэффициентом `alpha` из `FrameContext`. Тот же `alpha` есть в `SystemContext` и `engine.stats().alpha`.

Статистика кадра — `engine.stats()` (`EngineStats`): `frame` (счёт с нуля), `frameMs`, `gameMs`, `simulationMs`, `extractMs`, `renderMs`, `waitForRenderMs`, `fixedSteps`, `totalFixedSteps`, `alpha`, `droppedTime`, `fps`, `gameTime`, `realTime`. Сигнал `engine.frameEnded` срабатывает в конце каждого кадра.

## Шаг 4. Потоки игры и рендера

```
game  : |input sim N|extract N→slot0|input sim N+1|extract N+1→slot1|input sim N+2|wait slot0|extract N+2→slot0|
render:                             |render N (slot0)              |render N+1 (slot1)        |render N+2 ...
```

У рендерера два слота снимков (`kRenderSnapshotSlots = 2`). Пока поток рендера рисует кадр N из одного слота, игровой поток симулирует кадр N+1 и извлекает его в другой. Игровой поток опережает рендер не больше чем на кадр.

Контракт `IRenderer`:

| Метод | Поток | Правило |
| --- | --- | --- |
| `init(services, surface)` / `shutdown()` | главный | Поток рендера не запущен |
| `extract(world, ctx)` | игровой | Скопировать всё нужное в слот `ctx.slot`. Не хранить ссылки на `World` |
| `render(ctx)` | рендера | Читать только слот `ctx.slot` и своё GPU-состояние. Мир и игровые службы не трогать |
| `resize(size)`, `settingsChanged()` | рендера | Вызываются между `render()`, никогда одновременно с ним |

```cpp
class PositionsRenderer final : public ox::IRenderer {
public:
    std::string_view name() const override { return "Positions"; }
    ox::Status init(ox::Services&, const ox::RenderSurface&) override { return {}; }
    void shutdown() override {}
    void extract(const ox::World& world, const ox::FrameContext& ctx) override {
        auto& snapshot = m_slots[ctx.slot];
        snapshot.positions.clear();
        for (auto [e, wt] : world.registry().view<const ox::WorldTransformComponent>().each()) {
            const glm::vec3 prev = glm::vec3(wt.previous[3]);
            const glm::vec3 curr = glm::vec3(wt.matrix[3]);
            snapshot.positions.push_back(glm::mix(prev, curr, ctx.alpha)); // интерполяция
        }
    }
    void render(const ox::FrameContext& ctx) override {
        const auto& snapshot = m_slots[ctx.slot]; // только свой слот
        // ... отрисовка ...
    }
    void resize(glm::uvec2) override {}
    void settingsChanged() override {}

private:
    struct Snapshot { std::vector<glm::vec3> positions; };
    std::array<Snapshot, ox::kRenderSnapshotSlots> m_slots;
};

engine.setRenderer(std::make_unique<PositionsRenderer>()); // до init(); движок владеет рендерером
```

`FrameContext` содержит `frameIndex`, `slot`, `time`, `realTime`, `dt`, `realDt`, `alpha`, `timeScale`, `paused`, `editMode`, `loading` (рисовать экран загрузки) и `viewportSize`. `engine.pipeline().flush()` блокирует игровой поток, пока не будут отрисованы все отправленные кадры. `config.threadedRendering = false` выполняет extract и render прямо в игровом потоке, так проще отлаживать рендерер.

Полный пример: `samples/guide_examples/05-runtime/renderer_launch.cpp`.

## Шаг 5. Проект `.oxproj`

Проект — это каталог с файлом `<Имя>.oxproj` (обычный JSON) и папками ассетов. Движок монтирует каталог как `project://`.

```cpp
#include <oxwald/runtime/project.hpp>

ox::Project project = ox::Project::create(dir / "MyGame", "MyGame"); // файл не пишется до save()
project.settings.version = "1.0.0";
project.settings.startupScene = "project://levels/main.oxscene";
project.settings.physics.fixedRate = 50.0f;
project.settings.modules["audio"] = false;
project.save();

auto loaded = ox::Project::load(dir / "MyGame"); // каталог или путь к .oxproj
loaded->settings.moduleEnabled("physics");      // true: отсутствие в списке = включён
```

Файл можно писать и руками. Отсутствующие поля получают значения по умолчанию, неизвестные игнорируются:

```json
{
  "name": "MyGame",
  "version": "1.0.0",
  "startupScene": "project://levels/main.oxscene",
  "assetDirs": ["assets"],
  "modules": { "audio": false },
  "saveVersion": 1,
  "physics": { "gravity": [0, -9.81, 0], "fixedRate": 50, "maxSubsteps": 8 },
  "defaultQuality": "High",
  "input": { "actions": [], "contexts": [], "activeContexts": [] }
}
```

| Секция | Содержимое |
| --- | --- |
| `name`, `version`, `company` | Идентификация. `version` пишется в заголовки сохранений |
| `startupScene` | Сцена, которую `Engine::init` загружает первой |
| `assetDirs` | Каталоги ассетов относительно корня проекта |
| `modules` | Выключатели встроенных модулей (`physics`, `audio`, `script`, `async`, …) |
| `saveVersion` | Текущая версия данных сохранений ([глава 07](07-savegames.md)) |
| `input` | Раскладка ввода по умолчанию ([глава 06](06-input.md)) |
| `physics` | `gravity`, `fixedRate`, `maxSubsteps`, `maxBodies` |
| `audio` | `sampleRate`, `maxVoices`, `busVolumes` |
| `rendering` | `cvars` по умолчанию, `rayTracingIfSupported`, `upscaler` |
| `defaultQuality`, `scalability` | Уровни качества ([глава 04](04-cvars-quality.md)) |
| `packaging` | Параметры упаковки |

Полный пример: `samples/guide_examples/05-runtime/project_levels.cpp`.

## Шаг 6. Уровни

```cpp
ox::EngineConfig config;
config.projectPath = root;            // стартовая сцена проекта загрузится в init()
engine.init(config);
engine.currentLevel();                // "project://levels/main.oxscene"

// Синхронная замена уровня (блокирует кадр).
engine.loadScene("project://levels/main.oxscene");

// Additive: догрузить сцену в текущий мир (UUID сохраняются) и выгрузить обратно.
auto added = engine.loadSceneAdditive("project://levels/props.oxscene"); // -> корневые сущности
engine.unloadAdditive("project://levels/props.oxscene");

// Асинхронная смена уровня: файл читается и декодируется в фоне, мир подменяется в начале одного из следующих кадров.
engine.setLoadingScreenHooks({
    .begin = [&](const std::string& level) { /* показать экран загрузки */ },
    .end = [&](const std::string& level, bool ok) { /* спрятать; ok == false — уровень не загрузился */ },
});
engine.requestLevelChange("project://levels/boss.oxscene");
// engine.loading() == true, FrameContext::loading == true, игра продолжает тикать
```

- Сигналы движка: `levelUnloading` (старый мир ещё жив), `levelLoaded` (новый мир активен), `modeChanged`, `frameEnded`.
- При асинхронной смене уровня автоматически делается автосейв покидаемого уровня (если включён `autosaveOnLevelChange`). При ошибке загрузки остаётся старый уровень.
- При выгрузке мира отменяются все корутины ([глава 08](08-coroutines.md)), а модули получают `onWorldUnloading`.
- `engine.setWorld(std::make_unique<World>(), "uri")` устанавливает мир, собранный в коде (новая сцена в редакторе, тесты).

**Редактор.** При `config.editor = true` движок стартует в `EngineMode::Edit`: системы с `playModeOnly()` и скрипты не работают. `enterPlayMode()` клонирует редактируемый мир и симулирует копию, `exitPlayMode()` выбрасывает её. `engine.world()` всегда возвращает активный мир, `engine.editWorld()` — редактируемый.

**Консоль.** Встроенные команды: `quit`, `pause`, `step`, `timescale`, `level`, `save`, `load`, `stats`, `help`, `find`, `history`, `clear`. Выполнить команду из кода: `engine.console().execute("timescale 0.25")`.

## Шаг 7. OxwaldPlayer

`OxwaldPlayer` запускает проект без редактора. Если сборка без GLFW или указан `--headless`, окно не открывается.

```sh
OxwaldPlayer --project samples/OxwaldShowcase
OxwaldPlayer MyGame --scene project://levels/test.oxscene --windowed --width 1280 --height 720
OxwaldPlayer MyGame --headless --frames 600 --cvar r.Bloom=0      # смоук-тест в CI
OxwaldPlayer MyGame --server                                       # выделенный сервер
```

| Флаг | Смысл |
| --- | --- |
| `--project <dir\|file.oxproj>` (или первый позиционный аргумент) | Проект |
| `--scene <uri\|path>` | Переопределить стартовую сцену |
| `--headless` / `--server` | Без окна / выделенный сервер (без звука и рендера) |
| `--frames N` | Выйти после N кадров |
| `--width W --height H`, `--fullscreen \| --borderless \| --windowed`, `--monitor I` | Окно |
| `--quality low\|medium\|high\|ultra`, `--cvar name=value` (повторяемый) | Качество и cvar'ы |
| `--fps N`, `--fixed-rate HZ` | Ограничение FPS, частота `FixedUpdate` |
| `--single-thread` | Без потока рендера |
| `--user-dir DIR` | Корень `user://` |
| `--pak <file>` | Упакованный проект (пока не поддерживается) |

Если вы пишете свой лаунчер, используйте тот же разбор командной строки:

```cpp
#include <oxwald/runtime/launch.hpp>

auto options = ox::parseLaunchOptions(argc, argv);  // или std::span<const std::string>
if (!options) { std::fprintf(stderr, "%s\n%s", options.error().message.c_str(), ox::launchUsage().c_str()); return 2; }
ox::EngineConfig config = options->toEngineConfig("MyGame");
ox::Engine engine;
engine.init(config);
engine.run(options->frames);
```

Полный пример: `samples/guide_examples/05-runtime/renderer_launch.cpp`.

## Типичные ошибки и подводные камни

- **`addModule` и `setRenderer` после `init()`** не работают. Всё регистрируется до инициализации.
- **Указатель на рендерер после `shutdown()`.** Движок владеет рендерером и уничтожает его при остановке. Статистику снимайте до `shutdown()` (и после `pipeline().flush()`).
- **Доступ к `World` из `render()`.** Это гонка данных: в это время игровой поток уже симулирует следующий кадр. Всё нужное копируйте в слот снимка в `extract()`.
- **Логика в `Update` вместо `FixedUpdate`.** Физика и детерминированная логика должны жить в фиксированном шаге, иначе их поведение зависит от FPS. В `Update` оставьте камеру, UI и визуальные эффекты.
- **Пауза не останавливает `Update`.** Системы фазы `Update` продолжают работать с `dt = 0`. Если система что-то накапливает покадрово, проверяйте `engine.paused()` или `FrameTime::paused`.
- **Хэндлы `Entity` после смены уровня** указывают на уничтоженный мир. Очищайте их в `onWorldUnloading` или по сигналу `levelUnloading`.
- **`stats().frame` считается с нуля.** После `run(10)` он равен 9.
- **Ошибка `init` модуля** прерывает весь `Engine::init`: уже созданные службы корректно уничтожаются, и `engine.initialized()` возвращает false.
- **`--pak` пока не поддерживается** плеером. Запускайте проект из распакованного каталога.

## API

| Заголовок | Что внутри |
| --- | --- |
| [`runtime.hpp`](../../engine/runtime/include/oxwald/runtime/runtime.hpp) | Общий заголовок модуля |
| [`engine.hpp`](../../engine/runtime/include/oxwald/runtime/engine.hpp) | `Engine`, `EngineConfig`, `EngineStats`, `FrameTime`, `IEngineModule`, `LoadingScreenHooks` |
| [`renderer.hpp`](../../engine/runtime/include/oxwald/runtime/renderer.hpp) | `IRenderer`, `NullRenderer`, `FrameContext`, `RenderSurface`, `kRenderSnapshotSlots` |
| [`render_pipeline.hpp`](../../engine/runtime/include/oxwald/runtime/render_pipeline.hpp) | `RenderPipeline` (потоки игры и рендера) |
| [`project.hpp`](../../engine/runtime/include/oxwald/runtime/project.hpp) | `Project`, `ProjectSettings` и вложенные секции |
| [`launch.hpp`](../../engine/runtime/include/oxwald/runtime/launch.hpp) | `LaunchOptions`, `parseLaunchOptions`, `launchUsage` |
| [`platform.hpp`](../../engine/runtime/include/oxwald/runtime/platform.hpp) | `IPlatform`, `GlfwPlatform`, `WindowDesc` |
| [`console.hpp`](../../engine/runtime/include/oxwald/runtime/console.hpp) | `Console` |
| [`settings.hpp`](../../engine/runtime/include/oxwald/runtime/settings.hpp), [`json_io.hpp`](../../engine/runtime/include/oxwald/runtime/json_io.hpp) | Пользовательские настройки, plain JSON для `.oxproj` |
| [`apps/player/main.cpp`](../../apps/player/main.cpp) | Исходник OxwaldPlayer: образец своего лаунчера |

Заметки для разработчиков модуля: [`docs/dev/modules/runtime.md`](../dev/modules/runtime.md).

## Что дальше

- [03. ECS и сцены](03-ecs-scene.md): системы, фазы и сцены, которые запускает движок.
- [04. CVar'ы и уровни качества](04-cvars-quality.md): `--cvar`, `--quality`, пользовательские настройки.
- [06. Ввод](06-input.md): `engine.input()` и раскладка из `.oxproj`.
- [07. Сохранения](07-savegames.md): `engine.saves()`, автосейв при смене уровня.
- [08. Корутины](08-coroutines.md): корутины, которые движок тикает в `PreUpdate` и отменяет при выгрузке мира.
- [Оглавление](README.md).
