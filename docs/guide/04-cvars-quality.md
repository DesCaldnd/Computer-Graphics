# 04. CVar'ы, консоль и качество графики

## Зачем

Игре нужны «ручки», которые можно крутить без перекомпиляции: дальность прорисовки, чувствительность мыши,
режим отладки, лимит FPS. В Oxwald это **консольные переменные** (CVar, console variable) — именованные значения
вида `r.Shadows.Resolution`, которые меняются из кода, консоли, командной строки (`--cvar`), файлов настроек и
меню. Поверх них построены **группы масштабируемости** (scalability groups) с уровнями Low/Medium/High/Ultra — как
пресеты качества в Unreal Engine — и **пользовательские настройки** игрока, которые применяются на лету, без
перезапуска.

## Ключевые понятия

| Понятие | Где | Коротко |
| --- | --- | --- |
| `CVar<T>` | `core/cvar.hpp` | переменная типа `bool`, `int`, `float` или `std::string` с описанием, флагами, диапазоном |
| `ConsoleCommand` | `core/cvar.hpp` | глобальная консольная команда |
| `CVarRegistry` | `core/cvar.hpp` | процесс-глобальный реестр: поиск, `execute("имя значение")`, сохранение |
| Группа масштабируемости | `core/scalability.hpp` | `ViewDistance, AntiAliasing, Shadows, GlobalIllumination, Reflections, PostProcess, Textures, Effects, Foliage, Shading, Volumetrics, RayTracing` |
| Уровень качества | `QualityLevel` | `Low=0, Medium=1, High=2, Ultra=3`, плюс `Custom` (что-то правили вручную) |
| `Console` | `runtime/console.hpp` | бэкенд внутриигровой консоли: история, автодополнение, команды, захват лога |
| `Settings` | `runtime/settings.hpp` | настройки игрока (`user://settings.json`) поверх настроек проекта |

Порядок применения значений при старте:
**значение по умолчанию в коде → настройки проекта (`.oxproj`) → настройки игрока (`settings.json`) →
командная строка (`--quality`, затем `--cvar`)**.

## Объявление CVar'ов

CVar — объект со статическим временем жизни в `.cpp`, который точно попадёт в программу: кладите его рядом с кодом,
который его читает. Конструктор сам регистрирует переменную, деструктор — снимает с регистрации.

```cpp
#include <oxwald/core/cvar.hpp>

ox::CVar<bool> cvGodMode("g.GodMode", false, "Player takes no damage", ox::CVarFlags::Cheat);
ox::CVar<float> cvMouseSensitivity("g.MouseSensitivity", 1.0f, "Mouse sensitivity", 0.1f, 10.0f,
                                   ox::CVarFlags::Persist);   // диапазон: значения зажимаются
ox::CVar<int> cvHudMode("g.HudMode", 0, "HUD layout", ox::CVarEnum{"Full", "Minimal", "Off"},
                        ox::CVarFlags::Persist);              // int как перечисление: принимает имена
ox::CVar<std::string> cvStartLevel("g.StartLevel", "project://levels/intro.oxscene", "Level to start in");
ox::CVar<int> cvAiThreads("g.AI.Threads", 2, "AI worker threads", ox::CVarFlags::RequiresRestart);
ox::CVar<int> cvBuildNumber("g.BuildNumber", 1234, "Build number", ox::CVarFlags::ReadOnly);
```

Использование:

```cpp
float sensitivity = cvMouseSensitivity;          // == get(); атомарное чтение из любого потока
if (cvGodMode) return;                           // bool-переменная в условии
cvMouseSensitivity.set(2.5f);                    // из кода — разрешено всегда (даже ReadOnly)
cvHudMode.setFromString("minimal", ox::CVarSource::Console);   // как из консоли: регистр не важен

const auto id = cvMouseSensitivity.onChanged([&](const float& now, const float& before) {
    camera.setSensitivity(now);                  // вызывается в потоке, который изменил значение
});
cvMouseSensitivity.removeCallback(id);
cvMouseSensitivity.reset();                      // к значению по умолчанию
```

Полный пример: `samples/guide_examples/04-cvars-quality/cvars.cpp`.

### Флаги

| Флаг | Смысл |
| --- | --- |
| `None` | обычная переменная |
| `Persist` | сохраняется в настройках игрока (`saveOverrides`), если отличается от значения по умолчанию |
| `ReadOnly` | менять может только код; консоль, конфиги и уровни качества отклоняются |
| `RequiresRestart` | значение запоминается, но действует после перезапуска; `CVarRegistry::restartRequired()` станет `true` — покажите игроку «требуется перезапуск» |
| `Cheat` | из консоли — только после `CVarRegistry::setCheatsEnabled(true)`; конфиги и код могут всегда |

Флаги комбинируются через `|`. Кто последним менял значение — `lastSource()`: `Default`, `Code`, `Scalability`,
`Config`, `Console`.

### Имена

Принятые префиксы: `r.` — рендеринг, `sg.` — уровни групп масштабируемости, `t.` — время/кадры, `g.` — геймплей,
`a.` — аудио, `fx.` — эффекты. Поиск по имени **нечувствителен к регистру**. Своим играм удобно добавлять префикс
проекта (`mygame.`), чтобы не столкнуться с переменными движка.

### Консольные команды

```cpp
int g_gold = 0;
ox::ConsoleCommand cmdGiveGold("g.GiveGold", "Give gold: g.GiveGold <amount>",
                               [](std::span<const std::string> args) -> std::string {
                                   if (args.empty()) return "usage: g.GiveGold <amount>";
                                   g_gold += std::stoi(args[0]);
                                   return "gold = " + std::to_string(g_gold);   // напечатается в консоли
                               });
```

### Реестр и строки консоли

```cpp
auto& reg = ox::CVarRegistry::instance();
reg.execute("g.StartLevel \"project://levels/boss arena.oxscene\"");   // "имя значение" — установить
auto shown = reg.execute("g.StartLevel");                              // "имя" — показать
auto out = reg.execute("g.GiveGold 150");                              // иначе — команда; *out == "gold = 150"
reg.execute("g.GodMode 1");                                            // ошибка: Cheat, читы выключены
reg.complete("g.Mouse");                                               // {"g.MouseSensitivity"}
reg.findAs<float>("g.MouseSensitivity");                               // CVar<float>* или nullptr
```

`execute` возвращает `Result<std::string>`: ошибка — неизвестное имя, неразбираемое значение или нет прав.
Кавычки группируют аргументы, `\` экранирует.

### Сохранение значений

```cpp
nlohmann::json saved = reg.saveOverrides();     // {"g.MouseSensitivity": 3.0, "g.HudMode": 2} — только Persist и не default
reg.saveOverridesToFile(path.string());
reg.loadOverridesFromFile(path.string());       // значения применяются с CVarSource::Config
```

Если загрузить значение для переменной, которая ещё не зарегистрирована (например, модуль рендера загрузится
позже), оно **ждёт** и применится в момент регистрации. В игре этим занимается `Settings` (см. ниже) — вручную
вызывать `saveOverrides` обычно не нужно.

## Группы масштабируемости и уровни качества

Фича, у которой есть «цена» по производительности, объявляет свои cvar'ы **с таблицей значений по уровням** —
четыре значения для Low, Medium, High, Ultra — и привязывает их к группе:

```cpp
#include <oxwald/core/scalability.hpp>

// Фича «трава»:                                                                  Low    Medium  High   Ultra
ox::CVar<float> cvGrassDensity("fx.Grass.Density", 1.0f, "Grass instances per m2",
                               ox::Scalability::Foliage, {0.25f, 0.5f, 1.0f, 2.0f});
ox::CVar<float> cvGrassDistance("fx.Grass.Distance", 80.0f, "Grass draw distance (m)",
                                ox::Scalability::Foliage, {30.0f, 50.0f, 80.0f, 150.0f});
ox::CVar<bool> cvGrassShadows("fx.Grass.Shadows", false, "Grass casts shadows",
                              ox::Scalability::Foliage, {false, false, false, true});
ox::CVar<int> cvDecalLimit("fx.Decals.Max", 256, "Max decals", ox::Scalability::Effects, {64, 128, 256, 512});
```

| Переменная | Группа | Low | Medium | High | Ultra |
| --- | --- | --- | --- | --- | --- |
| `fx.Grass.Density` | Foliage | 0.25 | 0.5 | 1.0 | 2.0 |
| `fx.Grass.Distance` | Foliage | 30 | 50 | 80 | 150 |
| `fx.Grass.Shadows` | Foliage | false | false | false | true |
| `fx.Decals.Max` | Effects | 64 | 128 | 256 | 512 |

Дальше фича просто читает свои cvar'ы (и, если нужно, реагирует на изменения через `onChanged`) — какой уровень
выбран, ей знать не нужно.

```cpp
namespace sc = ox::scalability;

sc::setOverall(ox::QualityLevel::Low);                          // все группы
sc::setGroup(ox::Scalability::Foliage, ox::QualityLevel::Ultra); // одна группа
sc::currentLevel(ox::Scalability::Foliage);                     // Ultra
sc::overallLevel();                                             // Custom: группы на разных уровнях

ox::CVarRegistry::instance().execute("fx.Grass.Distance 100");  // ручная правка...
sc::currentLevel(ox::Scalability::Foliage);                     // ...делает группу Custom

ox::CVarRegistry::instance().execute("sg.Foliage Medium");      // уровень группы — тоже cvar (Persist)

nlohmann::json preset = sc::savePreset();   // {"groups": {"Foliage": "Medium", ...}, "overrides": {"fx.Decals.Max": 100}}
sc::loadPreset(preset);

cvGrassDensity.levelValue(ox::QualityLevel::Ultra);             // 2.0 — для подсказок в меню
sc::cvars(ox::Scalability::Foliage);                            // все переменные группы
```

Полный пример: `samples/guide_examples/04-cvars-quality/scalability.cpp`.

Как это устроено: уровень каждой группы хранится в сохраняемой int-переменной `sg.<Группа>` (`sg.Shadows`,
`sg.Foliage`, …). Её изменение (из кода, консоли или файла) применяет значения уровня ко всем переменным группы
с источником `CVarSource::Scalability`. Группа сообщает `Custom`, если хотя бы одна её переменная отличается от
значения своего уровня; `savePreset` сохраняет такие правки в `overrides`. Переменная, зарегистрированная **после**
выбора уровня (поздно загруженный модуль), сразу получает значение текущего уровня. Переменные с флагом
`ReadOnly` уровнями не меняются. Имена групп и уровней для UI — `groupName`, `levelName`, `groupFromName`,
`levelFromName`.

> **Рендерер.** Конкретные cvar'ы рендерера (`r.Shadows.*`, `r.Upscaler`, `r.Upscaler.Quality`, `r.RayTracing.*`,
> …) и автоматический подбор уровней по короткому GPU-бенчмарку будут описаны в главе о рендеринге (скоро).

## Внутриигровая консоль

`ox::Console` (модуль runtime) — бэкенд консоли: окно рисует UI-слой, а вся логика здесь. Строка консоли — это
присваивание/запрос cvar'а, глобальная `ConsoleCommand` или команда, зарегистрированная на этом экземпляре
консоли. В движке консоль — сервис `engine.console()`.

```cpp
#include <oxwald/runtime/console.hpp>

ox::Console console;
console.addCommand("spawn", "spawn <prefab> [count]", [&](std::span<const std::string> args) -> std::string {
    if (args.empty()) return "usage: spawn <prefab> [count]";
    const int count = args.size() > 1 ? std::stoi(args[1]) : 1;
    spawnPrefab(args[0], count);
    return "spawned " + std::to_string(count) + " x " + args[0];
});

console.execute("spawn \"orc warrior\" 3");            // "spawned 3 x orc warrior"
console.execute("r.VSync false; t.MaxFPS 120");        // несколько команд через ;
console.complete("r.WindowMode F");                     // {"r.WindowMode Fullscreen"} — значения enum/bool тоже
console.completeCommonPrefix("spa");                    // "spawn" — то, что вставляет Tab
console.historyPrev();                                  // стрелка вверх
console.captureLog(ox::log::Level::Warn);               // дублировать OX_LOG_* в вывод консоли
for (const auto& line : console.output()) { /* line.kind, line.level, line.text */ }
```

Полный пример: `samples/guide_examples/04-cvars-quality/console.cpp`.

Встроенные команды консоли: `help [префикс]`, `find <текст>`, `history`, `clear`. Консоль движка добавляет:

| Команда | Действие |
| --- | --- |
| `quit` | выйти из игры |
| `pause` | переключить паузу |
| `step [n]` | на паузе — продвинуть n кадров (по одному фиксированному шагу) |
| `timescale <x>` | масштаб игрового времени |
| `level <uri>` | асинхронно загрузить уровень |
| `save <slot>` / `load <slot>` | сохранить / загрузить игру |
| `stats` | статистика кадра |

Отличие `addCommand` от глобальной `ConsoleCommand`: команда консоли живёт, пока жива консоль, и может
захватывать ссылки на игровые объекты; `ConsoleCommand` видна везде (`CVarRegistry::execute`, все консоли).
`saveHistory()`/`loadHistory()` сохраняют историю между запусками. Сигнал `Console::cvarChanged` сообщает имя
переменной, изменённой из консоли.

## Пользовательские настройки

`ox::Settings` (сервис `engine.settings()`) хранит настройки игрока `UserSettings` в `user://settings.json` поверх
настроек проекта и применяет их **через те же cvar'ы и группы масштабируемости** — поэтому консоль, меню опций,
файл настроек и командная строка всегда согласованы.

```cpp
ox::Settings& settings = engine.settings();
ox::ScopedConnection c = settings.changed.connect([&](ox::SettingsCategory cat) {
    if (ox::hasCategory(cat, ox::SettingsCategory::Graphics)) refreshOptionsMenu();
});

ox::GraphicsSettings g = settings.user().graphics;   // текущие значения для меню
g.quality = "Low";                                    // общий уровень
g.groups = {{"Shadows", "Ultra"}};                    // ...но тени на максимум
g.vsync = false;
g.maxFps = 144;
g.fov = 100.0f;
settings.setGraphics(g);      // -> cvar'ы + scalability + сигнал changed -> окно и рендерер

ox::AudioSettings a = settings.user().audio;
a.masterVolume = 0.5f;
a.busVolumes["Music"] = 0.3f;
settings.setAudio(a);
```

Полный пример: `samples/guide_examples/04-cvars-quality/settings.cpp`.

**Графика применяется без перезапуска.** Когда `Settings::changed` приходит с категорией `Graphics` или из
консоли движка меняется `r.*`/`sg.*`, `Engine` вызывает `applyGraphicsSettings()`: платформа применяет параметры
окна (режим, разрешение, монитор), а рендерер получает `IRenderer::settingsChanged()` на своём потоке прямо перед
следующим кадром и перестраивает граф рендера. Изменения из консоли движка (`engine.console()`) автоматически
попадают в `settings.user()` (`captureFromCVars()`), а при завершении движок сохраняет `settings.json`
(отключается `EngineConfig::saveUserSettingsOnShutdown = false`). Сохранить раньше — `settings.save()`.

### Поля настроек

| `GraphicsSettings` | cvar | По умолчанию |
| --- | --- | --- |
| `resolution` | `r.ResolutionX`, `r.ResolutionY` | `{0, 0}` — размер окна по умолчанию / разрешение рабочего стола |
| `windowMode` | `r.WindowMode` (`Windowed`, `Borderless`, `Fullscreen`) | `Windowed` |
| `monitor` | `r.Monitor` | 0 |
| `vsync` | `r.VSync` | true |
| `maxFps` | `t.MaxFPS` (0 — без ограничения) | 0 |
| `quality` | `sg.*` (`Low`/`Medium`/`High`/`Ultra`, `Custom` — только `groups`) | `High` |
| `groups` | `sg.<Группа>` | пусто |
| `rayTracing` | `r.RayTracing` (рендерер) | false |
| `upscaler` | `r.Upscaler` (`Off`, `FSR1`, `DLSS`) | `Off` |
| `upscalerQuality` | `r.Upscaler.Quality` | `Quality` |
| `fov` | `g.FOV` (30–150°) | 90 |

| `UserSettings` / `AudioSettings` | Смысл |
| --- | --- |
| `audio.masterVolume` | `a.MasterVolume`, 0–1 |
| `audio.busVolumes` | громкость шин `Music`, `SFX`, `Voice`, `UI`, `Ambience` (`settings.busVolume("Music")`) |
| `inputRebinds` | переназначения клавиш (см. [06-input](06-input.md)) |
| `mouseSensitivity`, `invertY`, `language` | общие параметры игры |
| `cvars` | прочие сохраняемые cvar'ы, в том числе ручные правки групп `Custom` |

Переменные `r.VSync`, `r.WindowMode`, `r.ResolutionX/Y`, `r.Monitor`, `t.MaxFPS`, `g.FOV`, `a.MasterVolume`
объявлены рантаймом (`ox::cvars::vsync()` и т. д.); `r.RayTracing`, `r.Upscaler`, `r.Upscaler.Quality` объявляет
рендерер, а `Settings` задаёт их по имени — значения ждут регистрации.

Пример `settings.json`:

```json
{
  "graphics": {
    "resolution": [2560, 1440],
    "windowMode": "Borderless",
    "monitor": 0,
    "vsync": false,
    "maxFps": 144,
    "quality": "Custom",
    "groups": { "Shadows": "Ultra", "Textures": "Low" },
    "rayTracing": false,
    "upscaler": "FSR1",
    "upscalerQuality": "Balanced",
    "fov": 100.0
  },
  "audio": { "masterVolume": 0.5, "busVolumes": { "Music": 0.3 } },
  "inputRebinds": {},
  "mouseSensitivity": 1.0,
  "invertY": false,
  "language": "ru",
  "cvars": { "g.MouseSensitivity": "3" }
}
```

### Значения по умолчанию в проекте

В `.oxproj` (см. [00](00-getting-started.md)) задаются стартовые значения для новых игроков:

```json
{
  "defaultQuality": "Medium",
  "scalability": { "Shadows": "Low" },
  "rendering": { "cvars": { "r.Bloom": "0" }, "upscaler": "FSR1" }
}
```

И с командной строки (поверх всего): `OxwaldPlayer --quality ultra --cvar fx.Grass.Density=0.5`.

## CVar'ы из Lua

Встроенного Lua-API для cvar'ов пока нет, но его легко добавить своим модулем API через `ScriptVM::bindApi`
(один раз при создании VM, до создания песочниц):

```cpp
vm.bindApi("cvar", [](sol::state_view, sol::table& api) {
    api["get"] = [](const std::string& name) -> std::optional<std::string> {
        const ox::ICVar* cv = ox::CVarRegistry::instance().find(name);
        return cv ? std::optional<std::string>(cv->toString()) : std::nullopt;   // nil, если нет
    };
    api["set"] = [](const std::string& name, const std::string& value) {
        return ox::CVarRegistry::instance().set(name, value, ox::CVarSource::Console);   // права как у консоли
    };
    api["exec"] = [](const std::string& line) {
        auto r = ox::CVarRegistry::instance().execute(line);
        return r ? *r : r.error().message;
    };
});
```

```lua
densityBefore = cvar.get("fx.Grass.Density")     -- "1": значения приходят строками
cvar.set("ui.ShowFps", "true")
if tonumber(densityBefore) > 0.5 then
    cvar.exec("fx.Grass.Density 0.5")            -- та же строка, что и в консоли
end
log.info("grass density is now", cvar.get("fx.Grass.Density"))
```

Полный пример: `samples/guide_examples/04-cvars-quality/lua_cvars.cpp` + `cvars_example.lua`. Используя источник
`Console`, вы сохраняете защиту `Cheat`/`ReadOnly` от скриптов модов.

## Типичные ошибки и подводные камни

- **CVar в «неиспользуемом» .cpp статической библиотеки.** Линкер выбрасывает объектный файл — переменная не
  зарегистрируется, `--cvar` и консоль её не увидят. Объявляйте cvar'ы рядом с кодом, который их читает.
- **CVar как локальная переменная.** Она снимается с регистрации в деструкторе; объявляйте со статическим временем
  жизни.
- **Две переменные с одним именем** — assert при регистрации (как и совпадение имени cvar'а и `ConsoleCommand`).
  Имена, отличающиеся только регистром, assert не ловит, но поиск нечувствителен к регистру — найдётся
  только одна из них; избегайте таких пар. Не объявляйте заново переменные рантайма (`r.VSync`, `t.MaxFPS`, …) —
  берите `ox::cvars::vsync()`.
- **Неверное число значений уровней.** Таблица должна содержать ровно 4 значения (Low, Medium, High, Ultra), иначе
  assert.
- **Тяжёлая работа в `onChanged`.** Колбэк выполняется в потоке, который изменил значение (часто — главный поток
  или поток консоли). Для рендерера правильный путь — `IRenderer::settingsChanged()`, который движок вызывает на
  потоке рендера.
- **`CVarRegistry::execute` вместо консоли движка.** Применение графики без перезапуска срабатывает на
  `Console::cvarChanged` и `Settings::changed`. Если вы меняете `r.*` в обход — через реестр или `set()` из кода —
  вызовите `engine.settings().captureFromCVars()` и `engine.applyGraphicsSettings()` сами (или меняйте через
  `Settings::setGraphics`).
- **Правка переменной группы «ломает» уровень.** После ручного изменения группа становится `Custom` — это
  ожидаемо; повторный выбор уровня (`setGroup`) вернёт табличные значения.
- **`RequiresRestart` без сообщения игроку.** Проверяйте `CVarRegistry::restartRequired()` после применения
  настроек.
- **`ReadOnly` и уровни.** Такие переменные игнорируют группы масштабируемости и значения из файлов — это способ
  «закрепить» значение кодом.

## API

- [`cvar.hpp`](../../engine/core/include/oxwald/core/cvar.hpp) — `CVar<T>`, `CVarFlags`, `CVarEnum`, `ConsoleCommand`, `CVarRegistry`, `QualityLevel`, `Scalability`
- [`scalability.hpp`](../../engine/core/include/oxwald/core/scalability.hpp) — `setOverall`, `setGroup`, `currentLevel`, `overallLevel`, `savePreset`/`loadPreset`
- [`console.hpp`](../../engine/runtime/include/oxwald/runtime/console.hpp) — `Console`
- [`settings.hpp`](../../engine/runtime/include/oxwald/runtime/settings.hpp) — `Settings`, `UserSettings`, `GraphicsSettings`, `AudioSettings`, `ox::cvars::*`
- [`project.hpp`](../../engine/runtime/include/oxwald/runtime/project.hpp) — `ProjectSettings::defaultQuality`, `scalability`, `rendering`
- [`launch.hpp`](../../engine/runtime/include/oxwald/runtime/launch.hpp) — `--quality`, `--cvar`
- [`script_vm.hpp`](../../engine/script/include/oxwald/script/script_vm.hpp) — `ScriptVM::bindApi`

## Что дальше

- [01. Ядро (core)](01-core.md)
- [05. Рантайм](05-runtime.md) — `Engine`, модули, игровой цикл и поток рендера.
- [06. Ввод](06-input.md) — переназначение клавиш в настройках.
- [15. Lua](15-scripting-lua.md) — свои API-модули для скриптов.
- [17. RHI и Vulkan](17-rhi-vulkan.md)
- [Оглавление](README.md)
