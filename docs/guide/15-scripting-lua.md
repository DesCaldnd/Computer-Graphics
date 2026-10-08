# 15. Скрипты на Lua

> Модуль `script` (таргет `Oxwald::script`, пространство имён `ox::script`). Lua 5.4 + sol2 3.5, зависит от `core` и glm; lua и sol2 — публичные зависимости (в `bindApi` вы получаете `sol::table`). Модуль не знает про ECS: экземпляры скриптов привязывает к сущностям модуль `gameplay` (`ScriptComponent`). Привязки сервисов runtime (ввод) — в [`script_bindings.hpp`](../../engine/runtime/include/oxwald/runtime/script_bindings.hpp).

## Зачем

Lua — язык геймплея, который меняют без перекомпиляции: поведение турелей, дверей, пикапов, катсцен, квестов. В Oxwald скрипт — это файл с объявленными свойствами (их видит и правит инспектор редактора) и функциями жизненного цикла `onCreate` / `onUpdate` / `onDestroy`. Скрипт:

- выполняется в **песочнице** (sandbox): нет файлов, процессов, `load`, `debug`; бесконечный цикл и утечка памяти прерываются лимитами;
- **не роняет игру**: любая ошибка логируется с `файл:строка` и traceback, экземпляр продолжает работать;
- перезагружается **на лету** (hot reload) с сохранением состояния;
- умеет корутины (`spawn`/`wait`), таймеры, события и `await` на C++ `Future`;
- видит API движка, которые C++ открывает через `bindApi`.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `ScriptVM` | Владелец Lua-состояния. Песочницы, загрузка скриптов, время скрипта, события, hot reload, лимиты. Не потокобезопасен — только игровой поток. |
| Окружение (environment) | Песочница со своими глобалами. У каждого экземпляра скрипта, модуля и консольного фрагмента — своё. |
| `ScriptAsset` | Исходник + метаданные файла скрипта: объявленные свойства, версия. Общий для всех экземпляров, кэшируется по пути. |
| `ScriptInstance` | Работающая копия скрипта (одна на сущность). Хранит `self` — таблицу состояния, которая переживает hot reload. |
| `self` | Таблица состояния экземпляра. В ней лежат значения свойств и всё, что скрипт туда запишет. |
| Свойства (properties) | `properties = { ... }` в скрипте: тип, значение по умолчанию, диапазон, подсказка. Видны в инспекторе без создания экземпляра. |
| `ScriptValue` | Простое C++-значение для обмена со скриптами: `monostate`, `bool`, `i64`, `f64`, `std::string`, `glm::vec2/3/4`. |
| `ScriptEventBus` | Именованные события между Lua и C++ (`vm.events()`, в Lua — `events`). |
| `Scheduler` | Корутины и таймеры на **времени скрипта**, которое двигает `vm.update(dt)`. |
| `bindApi` | Точка расширения: C++ регистрирует таблицу (например, `physics`), её видят все песочницы (только чтение). |

### Порядок вызовов в кадре

```
vm.update(dt)            // время скрипта, wait(), таймеры, корутины, опрос hot reload
instance->update(dt)     // onStart (один раз), затем onUpdate(self, dt) — для каждого экземпляра
instance->fixedUpdate()  // onFixedUpdate(self, dt) в фиксированном шаге
```

В `Engine` всё это уже сделано: модуль `script` создаёт `ScriptVM` в `Services` (корень поиска модулей — `<проект>/scripts`), привязывает таблицу `input` и создаёт `AsyncBridge`; `PreUpdate` вызывает `bridge.update(); vm.update(dt)`, а модуль `gameplay` создаёт экземпляры для сущностей с `ScriptComponent` и вызывает их `update` / `fixedUpdate` / `destroy` в play mode. Получить VM из игрового кода: `services.get<ox::script::ScriptVM>()`.

## Шаг 1. Скрипт сущности

`samples/guide_examples/15-scripting-lua/scripts/turret.lua`:

```lua
local aim = require("util.aim")

properties = {
    range    = { type = "float", default = 15, min = 1, max = 50, tooltip = "Дальность, м", order = 0 },
    fireRate = { type = "float", default = 2, min = 0.1, max = 10, tooltip = "Выстрелов в секунду", order = 1 },
    ammo     = { type = "int", default = 5, min = 0, max = 100, order = 2 },
    tint     = { type = "color", default = { 1, 0.2, 0.2, 1 }, order = 3 },
    muzzle   = { type = "vec3", default = vec3(0, 1.5, 0), order = 4 },
    friendly = false, -- короткая форма: bool со значением по умолчанию
}

function onCreate(self)
    self.shots = 0
    self.state = "off"
    -- Подписка принадлежит экземпляру и снимается при destroy().
    events.subscribe("enemy.spotted", function(name, enemy)
        if enemy.distance <= self.range then self.target = enemy.position end
    end)
end

function onStart(self)
    spawn(function()          -- корутина на времени скрипта
        self.state = "warming"
        wait(1.0)
        self.state = "ready"
        log.info("turret ready, ammo:", self.ammo)
    end)
end

function onUpdate(self, dt)
    if self.state ~= "ready" or self.target == nil or self.friendly then return end
    self.yaw = aim.yawTo(self.muzzle, self.target)
    self.cooldown = (self.cooldown or 0) - dt
    if self.cooldown <= 0 and self.ammo > 0 then
        self.ammo = self.ammo - 1
        self.shots = self.shots + 1
        self.cooldown = 1 / self.fireRate
        events.publish("turret.fired", { shots = self.shots, yaw = self.yaw })
    end
end

function onEvent(self, name, payload)
    if name == "reload" then self.ammo = self.ammo + payload end
end

function onDestroy(self)
    events.publish("turret.destroyed", { shots = self.shots })
end
```

Модуль `scripts/util/aim.lua` подключается через `require("util.aim")` (ищется `<корень>/util/aim.lua` или `<корень>/util/aim/init.lua`; результат кэшируется):

```lua
local M = {}
function M.yawTo(from, to)          -- угол по горизонтали в градусах; «вперёд» в движке — это -Z
    local d = to - from
    return math.deg(math.atan(d.x, -d.z))
end
return M
```

### Функции жизненного цикла

| Функция | Когда вызывается |
| --- | --- |
| `onCreate(self)` | `instance->create()`: после выполнения файла и заполнения `self` свойствами. |
| `onStart(self)` | Один раз перед первым `onUpdate` / `onFixedUpdate`. |
| `onUpdate(self, dt)` | Каждый кадр. |
| `onFixedUpdate(self, dt)` | Каждый фиксированный шаг. |
| `onEvent(self, name, payload)` | `instance->sendEvent(name, payload)` — событие конкретному экземпляру. |
| `onDestroy(self)` | `instance->destroy()`. Потом корутины, таймеры и подписки экземпляра снимаются. |
| `on_reload(self)` / `onReload(self)` | После горячей перезагрузки. |

Любая функция необязательна. Можно вызвать и свою: `instance->invoke("toggle", "Player")` → `toggle(self, "Player")`.

### Свойства

| Форма | Пример |
| --- | --- |
| Полная | `speed = { type = "float", default = 5, min = 0, max = 20, tooltip = "м/с", order = 0 }` |
| Короткая | `godMode = false` (bool), `speed = 5` (float), `name = "Bob"` (string) |

Типы: `float`, `int`, `bool`, `string`, `vec2`, `vec3`, `vec4`, `color` (хранится как vec4). Векторы и цвета по умолчанию задаются как `vec3(...)` или `{1, 0.5, 0, 1}`. Инспектор показывает свойства отсортированными по `(order, name)`.

## Шаг 2. Запуск из C++

```cpp
#include <oxwald/script/script_events.hpp>
#include <oxwald/script/script_instance.hpp>
#include <oxwald/script/script_vm.hpp>
using namespace ox::script;

ScriptVM vm({.hotReloadInterval = 0.0, .searchRoots = {kScripts}});
std::shared_ptr<ScriptAsset> asset = vm.loadScript(kScripts / "turret.lua");   // кэшируется по пути

// Инспектор: свойства доступны без экземпляра.
for (const ScriptPropertyDesc& p : asset->properties()) { /* p.name, p.type, p.defaultValue, p.min, p.max, p.tooltip */ }

auto turret = vm.createInstance(asset, [](ScriptInstance&, sol::table& self) {
    self["entityName"] = "Turret_01";   // до onCreate (в ECS сюда кладут ссылку на сущность)
});
turret->setProperty("ammo", ScriptValue{i64{3}});     // переопределение «из префаба»
turret->setProperty("range", ScriptValue{999.0});     // зажмётся до max = 50
turret->create();                                     // выполнить файл, заполнить self, onCreate(self)

// каждый кадр
vm.update(dt);
turret->update(dt);

turret->sendEvent("reload", ScriptValue{i64{2}});     // onEvent(self, "reload", 2)
turret->destroy();                                     // onDestroy + очистка
```

- `setProperty` приводит тип (int ↔ float) и зажимает в `[min, max]`; для неизвестного свойства или несовместимого типа возвращает `false`. Работает и до, и после `create()`.
- `getProperty(name)` возвращает текущее значение из `self`; `self()` даёт саму таблицу: `turret->self()["state"].get<std::string>()`.
- Ошибки в функциях скрипта не бросают исключений: они в логе, `turret->errorCount()` и `vm.errorCount()` / `vm.lastError()` их считают.

Полный пример: `samples/guide_examples/15-scripting-lua/instance.cpp`.

## Шаг 3. VM, песочница, ошибки и лимиты

```cpp
ScriptVM vm({.memoryLimit = 64u << 20, .instructionLimit = 10'000'000});
sol::environment env = vm.createEnvironment();          // отдельная песочница (консоль, тесты)
ScriptResult r = vm.runString("return vec3(1, 2, 2):length()", "console", &env);
if (r) { f32 len = r.value.as<f32>(); }                  // 3
else   { /* уже залогировано; r.error = "chunk:line: сообщение" + traceback */ }
```

| `ScriptVMConfig` | По умолчанию | Смысл |
| --- | --- | --- |
| `memoryLimit` | 256 МБ | На всю VM; при превышении Lua получает «not enough memory». 0 — без лимита. |
| `instructionLimit` | 50 млн | На **один вызов** из C++ (функция жизненного цикла, событие, шаг корутины). Ловит бесконечные циклы. |
| `hookInterval` | 1000 | Как часто проверять лимит инструкций. |
| `allowIo` / `allowOsExtended` | `false` | Открыть `io` / `os.getenv` и т. п. (`os.execute` и `os.exit` не открываются никогда). |
| `hotReloadInterval` | 0.5 с | Как часто `vm.update()` проверяет файлы; ≤ 0 — только вручную `pollHotReload()`. |
| `searchRoots` | — | Корни для `require`. |

В песочнице **нет**: `io`, `load`/`loadfile`/`dofile`, `debug`, `package`, `collectgarbage`, `string.dump`, опасных функций `os`. Библиотеки `math`, `string`, `table`, `utf8`, `coroutine`, `os` (`clock/time/date/difftime`) и API движка доступны только для чтения — скрипт не может «сломать» их для других. Превышение лимита инструкций нельзя проглотить через `pcall`, `xpcall` или `coroutine.resume`.

Полный пример: `samples/guide_examples/15-scripting-lua/vm_basics.cpp`.

## Шаг 4. Значения: ScriptValue и glm

`ScriptValue` — то, чем C++ обменивается со скриптами без sol2 (свойства, события, сохранения):

```cpp
sol::object v = vm.toLua(ScriptValue{glm::vec3(1.f, 2.f, 3.f)});   // -> Lua vec3
ScriptValue back = ScriptVM::fromLua(r.value);                      // Lua -> C++; таблицы -> monostate
std::get<glm::vec3>(back);
```

Целые числа Lua становятся `i64`, дробные — `f64`. Таблицы `ScriptValue` не выражает — для них используйте `sol::table` / `sol::object` напрямую.

glm-типы ходят через sol2 как значения: `glm::vec2/3/4`, `glm::quat`, `glm::mat4` в C++ — это `vec2/vec3/vec4`, `quat`, `mat4` в Lua. Трейты для sol2 лежат в [`sol_glm.hpp`](../../engine/script/include/oxwald/script/sol_glm.hpp); он подключается из `script_vm.hpp`. Если в своём `.cpp` вы передаёте glm-значения через sol, не подключая `script_vm.hpp`, подключите `sol_glm.hpp` явно — иначе sol2 примет вектор за контейнер.

### Математика в Lua

| Глобал | Возможности |
| --- | --- |
| `vec2`, `vec3`, `vec4` | `vec3(x, y, z)`, `vec3(s)`, `.x/.y/.z`, `+ - * /`, `==`, `tostring`, `:length() :lengthSquared() :normalize() :dot(b) :distance(b) :lerp(b, t) :min/max/abs() :clone() :unpack()`, `vec3:cross :reflect`, константы `vec3.zero/one/up/right/forward` (forward = −Z) |
| `quat` | `quat()`, `quat(w, x, y, z)`, `quat.fromEuler`, `fromEulerDegrees`, `angleAxis(rad, axis)`, `lookRotation(fwd[, up])`, `fromTo(a, b)`, `q * q`, `q * v`, `:normalize :inverse :slerp :forward :up :right` … |
| `mat4` | `mat4()`, `translation`, `rotation`, `scaling`, `trs`, `lookAt`, `perspective`, `m * m`, `:inverse :transformPoint :transformVector :getTranslation` … |
| `math` | Стандартная + `clamp lerp inverseLerp remap smoothstep sign round approximately` |
| `log` | `log.trace/debug/info/warn/error(...)` → лог движка с `файл:строка`; `print` = `log.info` |

## Шаг 5. События

Шина событий связывает скрипты между собой и с C++. Обработчики вызываются синхронно; подписки скрипта принадлежат его экземпляру.

```cpp
// C++ подписывается на событие из Lua
vm.events().subscribe("turret.destroyed", [&](std::string_view, const sol::object& p) {
    shots = p.as<sol::table>()["shots"].get<int>();
});

// C++ публикует: простое значение...
vm.events().publish("lava.touch", ScriptValue{40.0});
// ...или таблицу
sol::table enemy = vm.lua().create_table();
enemy["distance"] = 12.0;
enemy["position"] = glm::vec3(10.f, 1.5f, 0.f);
vm.events().publish("enemy.spotted", enemy);
```

```lua
local id = events.subscribe("lava.touch", function(name, dps)
    events.publish("player.hurt", { amount = dps * 0.5 })
end)
events.unsubscribe(id)
```

| | Кому |
| --- | --- |
| `events.publish` / `vm.events().publish` | Всем подписчикам события по имени |
| `instance->sendEvent(name, payload)` | Одному экземпляру (`onEvent`) |

Мост к событиям движка — обычная пара «подписался в C++ на событие движка → `publish` здесь» и наоборот.

Полный пример: `samples/guide_examples/15-scripting-lua/vm_basics.cpp` (тест `EventBusBetweenCppAndLua`).

## Шаг 6. Корутины и таймеры скрипта

```lua
spawn(function(tag)            -- запускается сразу, до первого wait
    wait(1.0)                  -- секунды времени скрипта
    wait()                     -- до следующего кадра
end, "intro")

local id = timer.every(0.5, function() self.blinks = self.blinks + 1 end)
timer.after(1.2, function() timer.cancel(id) end)
local t = time()               -- время скрипта, с
```

- Время скрипта двигает `vm.update(dt)`; в движке его вызывает система `PreUpdate` с `dt` игрового кадра, только в play mode.
- `wait` вне `spawn`-корутины — ошибка.
- Корутины, таймеры и подписки, созданные из экземпляра, принадлежат ему и снимаются при `destroy()`.
- `await(future)` ждёт C++ `Future` — см. [главу 08](08-coroutines.md#шаг-11-lua-await-и-вызов-скриптов-из-c).

Полный пример: `samples/guide_examples/15-scripting-lua/instance.cpp` (тест `TimersAndErrorsDoNotStopTheGame`).

## Шаг 7. Свой API для скриптов: bindApi

```cpp
vm.bindApi("physics", [](sol::state_view, sol::table& api) {
    api["raycast"] = [](glm::vec3 from, glm::vec3 dir, f32 maxDist) -> sol::optional<glm::vec3> {
        if (dir.y >= 0.f) return sol::nullopt;      // nil в Lua
        const f32 t = from.y / -dir.y;
        if (t > maxDist) return sol::nullopt;
        return from + dir * t;
    };
    api["gravity"] = -9.81;
});
```

```lua
local hit = physics.raycast(vec3(3, 10, 0), vec3(0, -1, 0), 100)
if hit then log.info("ground at", hit) end
```

- Таблица строится один раз; каждая песочница видит её только для чтения.
- Исключение из C++-функции превращается в Lua-ошибку с traceback — игра не падает.
- Асинхронные функции возвращают `bridge.wrap(future)` — тогда скрипт делает `await(...)` ([глава 08](08-coroutines.md)).
- Лямбда, захватывающая объекты по ссылке, должна не пережить их: VM живёт долго.

Полный пример: `samples/guide_examples/15-scripting-lua/vm_basics.cpp`.

### Привязки runtime: таблица `input`

`bindInputLuaApi(vm, inputSystem)` ([`script_bindings.hpp`](../../engine/runtime/include/oxwald/runtime/script_bindings.hpp)) открывает скриптам ввод из [главы 06](06-input.md). В `Engine` это делает модуль `script` автоматически.

| Функция | Результат |
| --- | --- |
| `input.action("Move")` | Значение действия: `bool`, число, `vec2` или `vec3` — по типу действия |
| `input.triggered("Jump")`, `input.started(...)`, `input.completed(...)` | Состояние триггеров действия |
| `input.keyDown("W")`, `keyPressed`, `keyReleased` | «Сырые» клавиши |
| `input.mouseDown("Left")`, `mousePosition()`, `mouseDelta()`, `mouseWheel()` | Мышь |
| `input.gamepadAxis(pad, "LeftX")` | Ось геймпада |
| `input.setCursorMode("Normal" \| "Hidden" \| "Locked")` | Режим курсора |

```lua
function onUpdate(self, dt)
    local move = input.action("Move")                 -- Axis2D -> vec2
    self.position = self.position + vec3(move.x, 0, -move.y) * (self.speed * dt)
    if input.triggered("Jump") then self.jumps = self.jumps + 1 end
    self.sprinting = input.keyDown("LeftShift")
end
```

Полный пример: `samples/guide_examples/15-scripting-lua/input_bindings.cpp` и `scripts/player_controller.lua`.

## Шаг 8. Горячая перезагрузка

Сохранили файл в редакторе — через `hotReloadInterval` (0.5 с) `vm.update()` заметит новую дату изменения:

1. новый исходник сначала компилируется; **синтаксическая ошибка** логируется («keeping version N»), продолжает работать старая версия;
2. каждый экземпляр получает свежее окружение с новым кодом, но **ту же таблицу `self`** — всё состояние сохраняется;
3. новые свойства получают значения по умолчанию, инспектор сразу видит новое объявление;
4. вызывается `on_reload(self)` (или `onReload`);
5. изменение модуля из `require` перезагружает все файловые скрипты.

```cpp
ScriptVM vm({.hotReloadInterval = 0.0});    // 0 = опрос вручную
auto counter = vm.createInstance(vm.loadScript(file));
counter->create();
// ... файл изменён на диске ...
vm.pollHotReload();                          // число перезагруженных ассетов
counter->loadedVersion();                    // версия кода, которую выполняет экземпляр
```

Корутины, таймеры и подписки, запущенные старым кодом, продолжают работать со старыми замыканиями. Если это важно, перезапустите их в `on_reload`.

Полный пример: `samples/guide_examples/15-scripting-lua/hot_reload.cpp`.

## Шаг 9. Скрипты на сущностях

В игре скрипты к сущностям вешает модуль `gameplay`: компонент `ScriptComponent { script, asset, properties, enabled }` ([`gameplay/script.hpp`](../../engine/gameplay/include/oxwald/gameplay/script.hpp)). Переопределения свойств хранятся в компоненте и редактируются в инспекторе; экземпляры существуют только в play mode. В `self.entity` скрипт получает свою сущность (`self.entity.transform`, `entity:get("Transform")`, `scene.find(...)` и API физики, звука, ИИ и анимации) — это описано в главе о геймплейных компонентах (*скоро*).

## Типичные ошибки и подводные камни

- **Глобалы вместо `self`.** `count = count + 1` в функции скрипта — глобал окружения экземпляра: он не виден в инспекторе и пропадает при hot reload (окружение создаётся заново). Состояние держите в `self`.
- **Состояние в локальных переменных уровня файла.** `local counter = 0` вверху файла сбрасывается при hot reload (файл выполняется заново).
- **`bindApi` после `createEnvironment()`.** Экземпляры скриптов получают API, привязанный позже, а «сырые» окружения из `createEnvironment()` — нет. Привязывайте API до создания окружений.
- **`wait` вне `spawn`.** Ошибка; в `onUpdate` ждать нельзя — запустите корутину.
- **Корутина не может «уступить» изнутри C++-колбэка** (например, из обработчика `events.subscribe`). Запустите `spawn` в обработчике.
- **Лимит инструкций — на вызов, не на кадр.** Тысяча экземпляров по 10 млн инструкций каждый всё равно повесят кадр; лимит — защита от зависания, а не профайлер.
- **Время скрипта стоит**, если не вызывать `vm.update(dt)` (в своих инструментах и тестах).
- **Целые и дробные.** `5` в Lua — целое (`i64` в `ScriptValue`), `5.0` — дробное (`f64`). Свойства приводятся к объявленному типу сами, а значения из событий — нет.
- **Объекты sol на другом потоке.** `sol::object`, `sol::table` и `ScriptVM` — только игровой поток.
- **Ссылки на C++-объекты в замыканиях `bindApi`**, которые умирают раньше VM, — висячие ссылки при следующем вызове из скрипта.
- **`io` выключен.** Чтение файлов будет через VFS-API; `allowIo = true` — только для инструментов.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`script_vm.hpp`](../../engine/script/include/oxwald/script/script_vm.hpp) | `ScriptVM`, `ScriptVMConfig`, `ScriptResult` |
| [`script_instance.hpp`](../../engine/script/include/oxwald/script/script_instance.hpp) | `ScriptAsset`, `ScriptInstance` |
| [`script_value.hpp`](../../engine/script/include/oxwald/script/script_value.hpp) | `ScriptValue`, `ScriptPropertyType`, `ScriptPropertyDesc`, `coerceProperty`, `toDebugString` |
| [`script_events.hpp`](../../engine/script/include/oxwald/script/script_events.hpp) | `ScriptEventBus`, `Scheduler` |
| [`sol_glm.hpp`](../../engine/script/include/oxwald/script/sol_glm.hpp) | Трейты sol2 для glm-типов |
| [`async_bridge.hpp`](../../engine/script/include/oxwald/script/async_bridge.hpp) | `AsyncBridge`: Lua `await`, вызов скриптов из C++ корутин (при наличии `async`) |
| [`script_bindings.hpp`](../../engine/runtime/include/oxwald/runtime/script_bindings.hpp) | `bindInputLuaApi` — таблица `input` |
| [`gameplay/script.hpp`](../../engine/gameplay/include/oxwald/gameplay/script.hpp) | `ScriptComponent`, `ScriptRuntime` — скрипты на сущностях |

Заметки для разработчиков модуля (песочница, план интеграции с ECS, ограничения): [`docs/dev/modules/script.md`](../dev/modules/script.md).

## Что дальше

- [08. Корутины](08-coroutines.md) — `await` на C++ `Future` и вызов Lua-функций из C++ корутин.
- [06. Ввод](06-input.md) — действия и контексты, которые читает таблица `input`.
- [05. Runtime и игровой цикл](05-runtime.md) — где VM живёт в `Services` и когда тикается.
- [03. ECS и сцены](03-ecs-scene.md) — сущности, к которым привязываются скрипты.
- [07. Сохранения](07-savegames.md) — сохранение игрового состояния.
- [Оглавление](README.md).
