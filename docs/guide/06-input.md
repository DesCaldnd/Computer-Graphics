# 06. Ввод

## Зачем

Если код игры спрашивает «нажата ли клавиша Space», управление намертво привязано к клавиатуре. Геймпад, переназначение клавиш и режим «в машине», где пробел — тормоз, а не прыжок, приходится доделывать вручную по всему коду. OxwaldEngine устроен по модели Enhanced Input из Unreal Engine. Игра спрашивает о **действиях** (actions): «Jump сработал?», «куда направлен Move?». Как физический ввод превращается в действия, описывает **раскладка** (mapping) — данные, которые лежат в `.oxproj` и правятся без перекомпиляции.

Подсистема ввода — служба `ox::InputSystem` в модуле `runtime`. `Engine` создаёт её сам (`engine.input()`), загружает раскладку из проекта, применяет пользовательские переназначения и раз в кадр вызывает `update()`.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| Действие (`InputActionDesc`) | Имя и тип значения: `Bool`, `Axis1D`, `Axis2D`, `Axis3D` |
| Источник (source) | Строка с физическим вводом: `"Key.W"`, `"Mouse.XY"`, `"Gamepad.LeftStick"` |
| Привязка (`InputBinding`) | Источник → действие, плюс модификаторы и триггеры |
| Модификатор (`InputModifier`) | Преобразует значение: мёртвая зона, инверсия, перестановка осей, масштаб |
| Триггер (`InputTrigger`) | Решает, когда действие «сработало»: нажатие, отпускание, удержание, тап, аккорд |
| Контекст (`InputContextDesc`) | Набор привязок с приоритетом: «пешком», «в машине», «меню» |
| `ActionState` | Состояние действия в кадре: значение, `started`/`triggered`/`completed`/`canceled` |

### Источники

| Устройство | Имена |
| --- | --- |
| Клавиатура | `Key.A`…`Key.Z`, `Key.Num0`…`Key.Num9`, `Key.Space`, `Key.Enter`, `Key.Escape`, `Key.Tab`, `Key.LeftShift`, `Key.LeftControl`, `Key.LeftAlt`, стрелки `Key.Up/Down/Left/Right`, `Key.F1`…`Key.F12`, `Key.Kp0`…, полный список — `OX_INPUT_KEYS` в `input.hpp` |
| Кнопки мыши | `Mouse.Left`, `Mouse.Right`, `Mouse.Middle`, `Mouse.Button4`, `Mouse.Button5` |
| Оси мыши | `Mouse.X`, `Mouse.Y`, `Mouse.XY` (смещение за кадр), `Mouse.Wheel`, `Mouse.WheelX` |
| Кнопки геймпада | `Gamepad.A/B/X/Y`, `Gamepad.LeftBumper/RightBumper`, `Gamepad.Back/Start/Guide`, `Gamepad.LeftThumb/RightThumb`, `Gamepad.DpadUp/Right/Down/Left` |
| Оси геймпада | `Gamepad.LeftX/LeftY/RightX/RightY` (−1…1, +Y вверх), `Gamepad.LeftTrigger/RightTrigger` (0…1), `Gamepad.LeftStick/RightStick` (2D) |

Источники геймпада читают все подключённые геймпады, и побеждает наибольшее значение.

## Шаг 1. Раскладка

```cpp
#include <oxwald/runtime/input.hpp>

using ox::InputModifier;
using ox::InputTrigger;
ox::InputMappingConfig m;
m.actions = {
    {"Move", ox::InputValueType::Axis2D},
    {"Look", ox::InputValueType::Axis2D},
    {"Jump", ox::InputValueType::Bool},
    {"Interact", ox::InputValueType::Bool},
    {"Modifier", ox::InputValueType::Bool},
    {"QuickSave", ox::InputValueType::Bool},
};
m.contexts = {{"OnFoot", 0, {
    // WASD -> 2D: D = +X, A = -X, W = +Y (swizzle X->Y), S = -Y.
    {"Move", "Key.D"},
    {"Move", "Key.A", {InputModifier::makeNegate()}},
    {"Move", "Key.W", {InputModifier::makeSwizzle()}},
    {"Move", "Key.S", {InputModifier::makeNegate(), InputModifier::makeSwizzle()}},
    {"Move", "Gamepad.LeftStick", {InputModifier::makeDeadZone(0.2f)}},
    // Мышь: инвертируем Y и масштабируем чувствительность.
    {"Look", "Mouse.XY", {InputModifier::makeScale({0.1f, -0.1f, 1.0f})}},
    {"Jump", "Key.Space", {}, {InputTrigger::pressed()}},
    {"Jump", "Gamepad.A", {}, {InputTrigger::pressed()}},
    {"Interact", "Key.E", {}, {InputTrigger::hold(0.5f)}},
    {"Modifier", "Key.LeftControl"},
    {"QuickSave", "Key.S", {}, {InputTrigger::pressed(), InputTrigger::chord("Modifier")}},
}}};
m.activeContexts = {"OnFoot"};

ox::InputSystem input;      // в игре: engine.input()
input.setMappings(m);
```

`InputBinding` — это `{action, source, modifiers, triggers, consume = true}`. Действию можно назначить сколько угодно привязок, и значения складываются по правилу `accumulation`:

- `HighestAbsolute` (по умолчанию): по каждой компоненте берётся значение с наибольшим модулем;
- `Cumulative`: значения суммируются, поэтому A+D вместе дают 0.

## Шаг 2. Чтение действий

```cpp
// Платформа (GLFW, вьюпорт редактора, тест) кладёт события из любого потока...
input.inject(ox::InputEvent::key(ox::Key::W, true));
input.inject(ox::InputEvent::key(ox::Key::Space, true));
input.inject(ox::InputEvent::mouseMove({100, 100}, {20, 10}));
// ...а игровой поток раз в кадр их применяет (Engine делает это сам до систем).
input.update(dt);

glm::vec2 move = input.axis2D("Move");   // (0, 1)
glm::vec2 look = input.axis2D("Look");   // (2, -1): дельты мыши покадровые
bool jump = input.triggered("Jump");     // true только в кадре нажатия (триггер Pressed)
const ox::ActionState* s = input.action("Interact"); // полное состояние: value, state, elapsed, ...
```

Можно не опрашивать действия, а подписаться на события. Колбэки вызываются из `update()` на игровом потоке:

```cpp
ox::ScopedConnection conn = input.bindAction("Interact", ox::ActionEvent::Triggered,
                                             [&](const ox::ActionState&) { openDoor(); });
```

События действия: `Started` (ввод начался), `Ongoing` (идёт, но триггер ещё не выполнен, например удержание), `Triggered`, `Completed` (отпустили после срабатывания), `Canceled` (отпустили до срабатывания). Для сырого состояния есть `keyDown/keyPressed/keyReleased`, `mouseDown`, `mousePosition`, `mouseDelta`, `mouseWheel`, `gamepadAxis`, `textInput()` для ввода текста.

Полный пример: `samples/guide_examples/06-input/actions.cpp`.

## Шаг 3. Модификаторы

| Модификатор | Фабрика | Что делает |
| --- | --- | --- |
| DeadZone | `makeDeadZone(lower, upper = 1, Radial\|Axial)` | Ниже `lower` → 0, выше `upper` → 1, между ними линейная перенормировка. `Radial` — по длине вектора стика, `Axial` — по каждой оси |
| Negate | `makeNegate(x = true, y = true, z = true)` | Инвертирует выбранные компоненты |
| Swizzle | `makeSwizzle(YXZ\|ZYX\|XZY\|YZX\|ZXY)` | Переставляет оси: так клавиша (1D, значение в X) попадает в Y 2D-действия |
| Scale | `makeScale({x, y, z})` | Покомпонентный множитель (чувствительность мыши, инверсия Y) |

Модификаторы применяются по порядку. Для `S` в WASD сначала `Negate`, потом `Swizzle`, и получается (0, −1).

## Шаг 4. Триггеры

| Триггер | Фабрика | Срабатывает |
| --- | --- | --- |
| Down | `down(threshold = 0.5)` | Каждый кадр, пока ввод выше порога. Так же ведёт себя привязка без триггеров (любое ненулевое значение) |
| Pressed | `pressed()` | Один раз в кадре нажатия. Быстрое нажатие и отпускание внутри одного кадра тоже считается |
| Released | `released()` | В кадре отпускания |
| Hold | `hold(seconds, oneShot = true)` | После удержания `seconds`. При `oneShot = false` — каждый кадр после этого |
| Tap | `tap(maxSeconds = 0.2)` | При отпускании, если держали не дольше `maxSeconds` |
| Chord | `chord("Modifier")` | Только пока действие `Modifier` тоже в состоянии Triggered. Комбинируется с другими триггерами |

```cpp
// Удержание E: отпустили раньше 0.5 с -> Canceled (спрятать индикатор), держим дольше -> один Triggered.
// Ctrl+S: QuickSave срабатывает только вместе с Modifier, просто S — это движение назад.
```

## Шаг 5. Контексты и приоритеты

Активные контексты сортируются по приоритету. Источник, привязанный в контексте с большим приоритетом (при `consume = true`), не виден контекстам ниже. Так «в машине» пробел становится тормозом, а прыжок не срабатывает.

```cpp
input.addAction({"Brake", ox::InputValueType::Bool});
input.addContext({"Vehicle", 10, {{"Brake", "Key.Space"}}});
input.activateContext("Vehicle");            // можно переопределить приоритет: activateContext("Vehicle", 5)
input.activeContexts();                      // {"Vehicle", "OnFoot"} — от старшего к младшему
// ... пробел: Brake = true, Jump = false
input.deactivateContext("Vehicle");          // вышли из машины
```

Источники, которые старший контекст не использует (например, `Escape` для паузы), по-прежнему доходят до младших.

## Шаг 6. Переназначение клавиш

```cpp
// Экран настроек: «нажмите новую клавишу для прыжка».
input.captureNextInput([&](const std::string& source) {
    input.rebind("OnFoot", "Jump", 0, source);   // привязка #0 действия Jump в контексте OnFoot
});
// Захваченное нажатие не запускает действия.

input.bindingSource("OnFoot", "Jump", 0);       // "Key.J" — для подписи на кнопке в меню
input.rebinds();                                 // {"OnFoot/Jump/0": "Key.J"}
input.resetBinding("OnFoot", "Jump", 0);         // или resetAllBindings()
```

- `rebind()` возвращает false для неизвестного источника или несуществующего индекса привязки.
- Сигнал `rebindsChanged` срабатывает при каждом изменении.
- В `Engine` переназначения автоматически сохраняются в `user://settings.json` (`UserSettings::inputRebinds`) и применяются при следующем запуске поверх раскладки проекта.
- `cancelCapture()` отменяет захват.

## Шаг 7. Раскладка в проекте

В `Engine` раскладку обычно не собирают в коде: её берут из секции `input` файла `.oxproj`. Это те же структуры в виде plain JSON:

```json
"input": {
  "actions": [
    { "name": "Move", "type": "Axis2D", "accumulation": "HighestAbsolute", "description": "" },
    { "name": "Jump", "type": "Bool" }
  ],
  "contexts": [
    { "name": "OnFoot", "priority": 0, "bindings": [
      { "action": "Move", "source": "Key.D" },
      { "action": "Move", "source": "Key.W", "modifiers": [ { "type": "Swizzle", "order": "YXZ" } ] },
      { "action": "Jump", "source": "Key.Space", "triggers": [ { "type": "Pressed" } ] }
    ] }
  ],
  "activeContexts": ["OnFoot"]
}
```

Отсутствующие поля получают значения по умолчанию. Чтобы получить JSON текущей раскладки, вызовите `ox::json::toPlain(input.mappings())` (предварительно `ox::registerInputTypes()`).

```cpp
ox::ProjectSettings project;
project.input = makeMappings();         // в реальном проекте — из .oxproj
config.projectSettings = project;
engine.init(config);
engine.input().inject(ox::InputEvent::key(ox::Key::Space, true));
engine.tick(dt);                        // Engine::tick вызывает input.update()
engine.input().triggered("Jump");
```

**Курсор.** `input.setCursorMode(ox::CursorMode::Locked)` захватывает курсор (FPS-камера), `Hidden` прячет, `Normal` возвращает. Платформа применяет режим по сигналу `cursorModeChanged`. В режиме `Locked` `mouseDelta()` продолжает выдавать сырое смещение.

## Шаг 8. Ввод из Lua

Если в сборке есть модуль `script`, движок регистрирует в Lua глобальную таблицу `input` только для чтения. Она доступна в каждой песочнице скриптов.

```lua
local speed = 5.0

function readPlayerInput(dt)
    local move = input.action("Move")          -- Axis2D -> vec2 (move.x, move.y)
    local sprint = input.keyDown("LeftShift")  -- сырое состояние клавиши
    local k = sprint and 2.0 or 1.0
    return {
        dx = move.x * speed * k * dt,
        dz = move.y * speed * k * dt,
        jump = input.triggered("Jump"),        -- фронт действия в этом кадре
        look = input.mouseDelta(),             -- vec2, пиксели за кадр
    }
end

function lockCursorForGameplay()
    input.setCursorMode("Locked")              -- "Normal" | "Hidden" | "Locked"
end
```

| Функция | Возвращает |
| --- | --- |
| `input.keyDown(name)`, `keyPressed(name)`, `keyReleased(name)` | bool. Имя клавиши без префикса: `"W"`, `"Space"`, `"LeftShift"` |
| `input.mouseDown(name)` | bool: `"Left"`, `"Right"`, `"Middle"`, … |
| `input.mousePosition()`, `input.mouseDelta()` | `vec2` |
| `input.mouseWheel()` | number (вертикальная прокрутка) |
| `input.action(name)` | По типу действия: bool, number, `vec2` или `vec3`. `nil`, если действия нет |
| `input.triggered(name)`, `started(name)`, `completed(name)` | bool |
| `input.gamepadAxis(pad, axis)` | number. `axis` — `"LeftX"`, `"RightTrigger"`, … |
| `input.setCursorMode(mode)` | Режим курсора |

Вне `Engine` (в тестах, инструментах) таблицу подключают вручную:

```cpp
#include <oxwald/runtime/script_bindings.hpp>
#include <oxwald/script/script_vm.hpp>

ox::script::ScriptVM vm;
ox::bindInputLuaApi(vm, input);
auto env = vm.createEnvironment();
vm.runFile(std::filesystem::path(OX_GUIDE_DIR) / "player_input.lua", &env);
```

Полный пример: `samples/guide_examples/06-input/lua_input.cpp` и `player_input.lua`.

## Типичные ошибки и подводные камни

- **Действие не срабатывает.** Чаще всего его контекст не активирован (`activeContexts` в раскладке или `activateContext`) либо источник съеден контекстом с большим приоритетом. Проверьте `input.activeContexts()`.
- **Опечатка в имени источника** (`"Key.Spcae"`) не даёт ошибки при загрузке раскладки. В лог пишется только предупреждение `unknown input source`, а привязка никогда не сработает. `rebind()` с неверным именем вернёт false. Проверить имя заранее можно через `ox::parseInputSource(name)`.
- **Ввод проверяют в `FixedUpdate`.** `triggered()` с триггером `Pressed` истинен один кадр, а в этом кадре может быть 0 фиксированных шагов, и нажатие потеряется. Читайте фронты в `Update` или `PreUpdate` и передавайте их в фиксированный шаг флагом.
- **`inject()` без `update()`.** События применяются только в `update()`. В `Engine` это происходит в начале кадра, а в своих тестах `update()` нужно вызывать самому.
- **Две клавиши одной оси при `HighestAbsolute`.** W+S вместе дают ±1, а не 0. Если нужно взаимное гашение, используйте `InputAccumulation::Cumulative`.
- **Мышь в `Axis2D` с триггером `Pressed`.** Для осей обычно не нужны триггеры: без них действие активно, пока значение ненулевое.
- **Потеря фокуса окна** (событие `FocusLost`) отпускает все клавиши. Не храните «зажатость» у себя, опрашивайте `InputSystem`.
- **Несколько игроков на одной машине** пока не поддерживаются: источники геймпада читают «любой геймпад», привязки устройства к игроку нет.

## API

| Заголовок | Что внутри |
| --- | --- |
| [`input.hpp`](../../engine/runtime/include/oxwald/runtime/input.hpp) | `InputSystem`, `InputMappingConfig`, `InputActionDesc`, `InputContextDesc`, `InputBinding`, `InputModifier`, `InputTrigger`, `ActionState`, `InputEvent`, `Key`, `parseInputSource` |
| [`script_bindings.hpp`](../../engine/runtime/include/oxwald/runtime/script_bindings.hpp) | `bindInputLuaApi` — Lua-таблица `input` |
| [`settings.hpp`](../../engine/runtime/include/oxwald/runtime/settings.hpp) | `UserSettings::inputRebinds`, `mouseSensitivity`, `invertY` |
| [`platform.hpp`](../../engine/runtime/include/oxwald/runtime/platform.hpp) | `GlfwPlatform`: события клавиатуры, мыши и геймпадов → `InputSystem::inject` |
| [`project.hpp`](../../engine/runtime/include/oxwald/runtime/project.hpp) | `ProjectSettings::input` |

## Что дальше

- [05. Runtime и игровой цикл](05-runtime.md): где в кадре обновляется ввод, проект `.oxproj`.
- [04. CVar'ы и уровни качества](04-cvars-quality.md): пользовательские настройки, в которых хранятся переназначения.
- [15. Скрипты на Lua](15-scripting-lua.md): скрипты сущностей, которые читают `input`.
- [07. Сохранения](07-savegames.md): быстрое сохранение по действию `QuickSave`.
- [Оглавление](README.md).
