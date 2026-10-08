# 08. Корутины

> Модуль `async` (таргет `Oxwald::async`, пространство имён `ox`). Зависит только от `core`. Дополнительно: `Oxwald::async_net` — запрос/ответ поверх сети (header-only, есть, если сконфигурирован модуль `net`), и мост в Lua `await` внутри модуля `script` (собирается автоматически, когда есть `async`). Общий заголовок: `#include <oxwald/async/async.hpp>`.

## Зачем

Игровая логика почти всегда растянута во времени: «открой дверь, дождись конца анимации, подожди пять секунд, закрой, если в проёме никого нет». Без корутин такое пишут конечными автоматами с флагами и таймерами в `update()`, и через месяц в них никто не разберётся. С корутинами C++20 (coroutines) тот же сценарий читается сверху вниз:

```cpp
door.animator.play("Open");
co_await event(door.animator.onFinished);   // ждём конец клипа
co_await seconds(5.0);                       // 5 секунд игрового времени
co_await until([&] { return !door.playerInside; });
```

Модуль даёт:

- **`Task<T>`** — ленивую корутину, которую можно ждать (`co_await`) из другой корутины;
- **`CoroutineScheduler`** — планировщик на игровом потоке: таймеры по игровому и реальному времени, ожидание кадров, условий и событий, отмена при уничтожении сущности;
- **`Promise<T>` / `Future<T>`** — потокобезопасный «результат потом» для загрузчиков, сети и фоновых задач;
- **переходы между потоками** (`backgroundThread()` / `mainThread()`) — тяжёлая работа на пуле потоков, результат применяется на игровом потоке;
- **комбинаторы** `whenAll` / `whenAny` / `timeout`;
- **Lua `await`** — скрипты ждут те же `Future`, а C++ может ждать Lua-функцию.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `Task<T>` | Корутина-задача. Ленивая: ничего не выполняет, пока её не ждут (`co_await`) или не запустили через `spawn`. Только перемещается (move-only). `Task<>` = `Task<void>`. |
| `CoroutineScheduler` | Планировщик корневых корутин. Поток, где он создан, — его «главный» (игровой) поток. Раз в кадр `tick(dt)`, раз в фиксированный шаг `fixedTick(dt)`. |
| `CoroutineHandle` | Ссылка на запущенную корневую корутину: `status()`, `isDone()`, `cancel()`, `error()`. Дешёвая копия. |
| `SpawnOptions` | `name` (для отладки), `owner` (id сущности), `token` (внешняя отмена), `parent`, `deferStart`. |
| Awaitable (ожидаемое) | То, что можно `co_await`: `nextFrame()`, `frames(n)`, `seconds(s)`, `realSeconds(s)`, `nextFixedUpdate()`, `until(pred)`, `whileTrue(pred)`, `event(signal)`, `Future<T>`, `Task<T>`. |
| `Promise<T>` / `Future<T>` | Одноразовый результат. `Promise` заполняют из любого потока; корутина продолжится в ближайшем `tick` на игровом потоке. |
| `whenAll` / `whenAny` / `timeout` | Параллельное ожидание нескольких задач; проигравшие в `whenAny` отменяются. |
| Владелец (owner) | Любой ненулевой `u64`, обычно id сущности. `cancelOwner(id)` отменяет все её корутины. |
| `Generator<T>` | Синхронный ленивый генератор для `for (x : gen)`. С планировщиком не связан. |

### Игровое и реальное время

| Ожидание | Время | Пауза (`setPaused`) | Замедление (`setTimeScale`) |
| --- | --- | --- | --- |
| `seconds(s)` | игровое | стоит | масштабируется |
| `realSeconds(s)` | реальное | идёт | не влияет |
| `frames(n)`, `nextFrame()` | тики планировщика | идут | не влияет |
| `nextFixedUpdate()` | следующий `fixedTick` | — | — |

`seconds(0)` всё равно ждёт до следующего тика — корутина никогда не продолжается «прямо сейчас» по таймеру.

### Где корутины живут в движке

В `Engine` модуль `async` регистрирует один `CoroutineScheduler` в `Services` (фоновый исполнитель — `JobSystemExecutor` поверх job system движка) и сам его тикает:

- `PreUpdate` (порядок 0): `bridge.update(); vm.update(dt);` — Lua;
- `PreUpdate` (порядок 10): `scheduler.tick(realDt, frame)` с учётом паузы и масштаба времени движка;
- `FixedUpdate` (порядок 10, после шага физики): `scheduler.fixedTick(fixedDt)`;
- при выгрузке мира — `cancelAll()`, при выходе — `shutdown()` с отчётом об «утёкших» корутинах.

Из игрового кода планировщик берут так: `auto& sched = services.get<ox::CoroutineScheduler>();`. Подробнее об игровом цикле — в [главе 05](05-runtime.md). Ниже во всех примерах планировщик создаётся вручную и тикается в тесте — так видно, что именно происходит.

## Шаг 1. Первая корутина

```cpp
#include <oxwald/async/async.hpp>
using namespace ox;

Task<int> countdown(int n, std::vector<int>& log) {
    for (int i = n; i > 0; --i) {
        log.push_back(i);
        co_await seconds(1.0);       // игровое время: учитывает timeScale и паузу
    }
    co_return n;
}

CoroutineScheduler sched;            // поток-создатель = игровой поток планировщика
std::vector<int> log;
CoroutineHandle h = sched.spawn(countdown(3, log), {.name = "Countdown"});
// spawn() сразу выполнил корутину до первого co_await: log == {3}

while (h.isRunning()) sched.tick(0.5);   // в игре это делает движок раз в кадр
// log == {3, 2, 1}, h.status() == CoroutineStatus::Completed
```

`spawn` ведёт себя как `StartCoroutine` в Unity: синхронно выполняет корутину до первой точки ожидания. Чтобы стартовать на следующем тике, передайте `{.deferStart = true}`.

Корутину удобно писать лямбдой — `spawn` хранит лямбду (и её захваты) внутри кадра корутины:

```cpp
sched.spawn([&]() -> Task<> {
    co_await nextFrame();                              // следующий tick
    co_await frames(2);                                // ещё два tick
    co_await until([&] { return bridgeLowered; });     // условие проверяется раз в tick
    co_await nextFixedUpdate();                        // после шага физики
});
```

Пауза и замедление:

```cpp
sched.setPaused(true);        // seconds() замирают, realSeconds() идут
sched.setTimeScale(0.5);      // slow-mo: 1 с реального времени = 0.5 с игрового
```

Полный пример: `samples/guide_examples/08-coroutines/basics.cpp`.

## Шаг 2. Сценарий «дверь»: события, таймер, условие

`event(signal)` ждёт следующего срабатывания `ox::Signal` (см. [главу 01](01-core.md)) и возвращает его аргументы: ничего, одно значение или кортеж. Сигнал можно испускать из любого потока — корутина всё равно продолжится на игровом.

```cpp
struct Animator {
    Signal<std::string> onFinished;   // имя доигравшего клипа
    void play(const std::string& clip);
};
struct Door {
    u64 entityId = 0;
    bool collision = true;
    bool playerInside = false;
    Animator animator;
};

// RAII: при отмене или исключении дверь не останется «проходимой».
struct RestoreCollision {
    Door& door;
    ~RestoreCollision() { door.collision = true; }
};

Task<> openDoor(Door& door) {
    co_await named("OpenDoor");                    // имя для отладки
    RestoreCollision guard{door};
    door.animator.play("Open");
    co_await event(door.animator.onFinished);      // ждём конец клипа
    door.collision = false;
    co_await seconds(5.0);                         // открыта 5 с игрового времени
    co_await until([&] { return !door.playerInside; });
    door.collision = true;
    door.animator.play("Close");
    co_await event(door.animator.onFinished);
}

CoroutineHandle h = sched.spawn(openDoor(door), {.owner = door.entityId});
```

Привязка к сущности — это `owner`. Когда сущность уничтожается, вызывается `sched.cancelOwner(door.entityId)`: кадр корутины раскручивается немедленно, деструкторы локальных переменных (здесь `RestoreCollision`) выполняются, подписка на сигнал и таймер снимаются, код после прерванного `co_await` не выполняется.

```cpp
EXPECT_EQ(sched.cancelOwner(door.entityId), 1u);
EXPECT_EQ(h.status(), CoroutineStatus::Cancelled);
EXPECT_TRUE(door.collision);                          // RAII отработал
EXPECT_EQ(door.animator.onFinished.slotCount(), 0u);  // подписка снята
```

Вызывайте `cancelOwner` **до** того, как компоненты сущности освобождены — тогда деструкторы в корутине могут безопасно их трогать. С модулем `gameplay` это делается автоматически: `gameplay::startCoroutine(services, entity, task)` привязывает корутину к сущности, а `CoroutineRuntime` отменяет её при уничтожении сущности и при остановке play mode ([`coroutines.hpp`](../../engine/gameplay/include/oxwald/gameplay/coroutines.hpp)).

Полный пример: `samples/guide_examples/08-coroutines/door.cpp`.

## Шаг 3. Future, Promise и callback-API

`Future<T>` — то, что возвращают асинхронные API движка и ваши собственные. Его можно ждать из корутины:

```cpp
Promise<int> scorePromise;
Future<int> score = scorePromise.future();
sched.spawn([&, score]() -> Task<> { received = co_await score; });

std::thread([&] { scorePromise.setValue(1500); }).join();   // любой поток
sched.tick(0.016);                                            // продолжение — здесь, на игровом потоке
```

Правила:

- корутина, которая ждала на игровом потоке, **всегда** продолжается в `tick()` этого планировщика, а не на потоке, который заполнил `Promise`;
- `Promise`, уничтоженный без результата, завершает `Future` исключением `BrokenPromise` — «вечного» ожидания не бывает;
- ошибку передают через `promise.setError("text")` (исключение `AsyncError`) или `setException(...)`; `co_await` перебросит её;
- `Future` копируется (общее состояние), ждать один `Future` могут несколько корутин.

Готовый API на колбэках превращается в `co_await` одной строкой через `awaitCallback`:

```cpp
Task<std::string> loadText(CallbackLoader& loader, std::string path) {
    std::string data = co_await awaitCallback<std::string>([&](auto resume) { loader.loadAsync(path, resume); });
    co_return data;
}
```

`resume` можно копировать и вызывать с любого потока; повторные вызовы игнорируются. Если все копии `resume` уничтожены, так и не вызвавшись, ожидание завершится `BrokenPromise`.

Обычную функцию на исполнителе (executor) запускает `runAsync`:

```cpp
ThreadPoolExecutor pool(2);
Future<int> f = runAsync(pool, [] { return 6 * 7; });
int answer = co_await f;
```

Готовые `Future` для тестов и заглушек: `makeReadyFuture(value)`, `makeErrorFuture<T>("msg")`.

Полный пример: `samples/guide_examples/08-coroutines/basics.cpp`.

## Шаг 4. Ошибки и исключения

Исключение внутри `Task` долетает до того, кто его ждёт (`co_await` перебрасывает). Если исключение не поймано в корневой задаче, она завершается со статусом `Failed`, текст доступен в `handle.error()`, а в лог пишется ошибка. Другие корутины и игра продолжают работать.

```cpp
auto failed = sched.spawn([&]() -> Task<> { co_await mayFail(true); }, {.name = "Reload"});
sched.tick(0.016);
// failed.status() == CoroutineStatus::Failed, failed.error() == "no ammo"
```

Для ожидаемых неудач (нет патронов, путь не найден) лучше возвращать `ox::Result<T>` ([глава 01](01-core.md)), а исключения оставить для действительно исключительных ситуаций.

## Шаг 5. Диалог: whenAny и тайм-аут бездействия

`whenAny` ждёт первую из задач. Остальные **отменяются и раскручиваются до того, как `whenAny` вернёт управление**: подписки сняты, таймеры удалены, RAII отработал.

```cpp
Task<> talk(Npc& npc, DialogueUi& ui) {
    co_await named("Dialogue:" + npc.name);
    ui.show("Торговать / Задание / Пока");
    // Кто первый: выбор игрока или 30 с реального времени бездействия.
    auto r = co_await whenAny(toTask(event(ui.onChoice)), toTask(realSeconds(30.0)));
    const int choice = r.index == 0 ? std::get<0>(r.value) : 2;   // молчание = «Пока»
    if (choice == 1) {
        ui.show("Принесёшь десять волчьих шкур?  Да / Нет");
        if (co_await event(ui.onChoice) == 0) npc.questGiven = true;
    }
    ui.hide();
}
```

- `toTask(awaitable)` превращает любое ожидаемое в `Task` — комбинаторы принимают только задачи.
- Результат `whenAny` — `r.index` (кто победил) и `r.value` (`std::variant`; `void` превращается в `std::monostate`).
- Для меню и диалогов берите `realSeconds`: на паузе они продолжают идти.

Полный пример: `samples/guide_examples/08-coroutines/dialogue.cpp`.

## Шаг 6. Загрузка уровня: whenAll и прогресс

`whenAll` ждёт все задачи и возвращает кортеж результатов (вариативная форма) или `std::vector<T>` (форма с вектором задач). Задачи выполняются параллельно внутри ожидающей корутины, у них тот же владелец и та же отмена.

```cpp
Task<std::vector<std::string>> loadLevel(AssetLoader& loader, std::vector<std::string> paths, f32& progress) {
    const usize total = paths.size();
    usize done = 0;
    auto loadOne = [&](std::string path) -> Task<std::string> {
        std::string data = co_await awaitCallback<std::string>([&](auto resume) { loader.loadAsync(path, resume); });
        ++done;
        co_return data;
    };
    auto showProgress = [&]() -> Task<> {
        while (done < total) {
            progress = static_cast<f32>(done) / static_cast<f32>(total);
            co_await nextFrame();
        }
        progress = 1.f;
    };
    std::vector<Task<std::string>> loads;
    for (const std::string& p : paths) loads.push_back(loadOne(p));
    // Все загрузки параллельно + полоска прогресса.
    auto results = co_await whenAll(whenAll(std::move(loads)), showProgress());
    co_return std::move(std::get<0>(results));
}
```

`whenAll` перебрасывает первое исключение из веток, но только после того, как все ветки закончились.

Если операция может зависнуть, оберните её в `timeout` (игровое время) или `realTimeout` (реальное). Результат — `std::optional<T>` (`nullopt` при тайм-ауте) или `bool` для `void`; зависшая операция отменяется:

```cpp
std::optional<std::string> mesh = co_await timeout(
    awaitCallback<std::string>([&](auto resume) { loader.loadAsync("huge.glb", resume); }), 2.0);
if (!mesh) { /* не дождались за 2 с */ }
```

Полный пример: `samples/guide_examples/08-coroutines/dialogue.cpp`.

## Шаг 7. Фоновый поиск пути: переходы между потоками

```cpp
Task<> repath(Agent& agent, const NavGrid& nav, Cell goal) {
    const Cell from = agent.cell;              // игровое состояние читаем на главном потоке
    co_await backgroundThread();               // -> фоновый исполнитель планировщика
    std::vector<Cell> path = nav.findPath(from, goal);   // тяжёлый потокобезопасный запрос
    co_await mainThread();                     // <- обратно; продолжим в ближайшем tick
    agent.path = std::move(path);              // безопасно: снова на игровом потоке
}

ThreadPoolExecutor pool(2);
CoroutineScheduler sched(&pool);               // в движке — JobSystemExecutor
sched.spawn(repath(agent, nav, {3, 2}), {.name = "Repath", .owner = agentId});
```

Десяток агентов ищут путь параллельно так: `co_await whenAll(std::move(vectorOfRepathTasks));`.

| Вызов | Что делает |
| --- | --- |
| `co_await backgroundThread()` | Продолжить на фоновом исполнителе планировщика (`CoroutineScheduler(IExecutor*)`; по умолчанию — небольшой собственный пул). |
| `co_await mainThread()` | Вернуться на игровой поток (в следующем `tick`). |
| `co_await switchTo(executor)` | Продолжить на любом `IExecutor`. |
| `runAsync(executor, fn)` | Выполнить обычную функцию на исполнителе, вернуть `Future`. |

Исполнители: `ThreadPoolExecutor` (свой пул), `JobSystemExecutor` (адаптер к job system движка), `InlineExecutor` (выполнить сразу — для тестов). Сам `CoroutineScheduler` тоже `IExecutor`: `sched.post(fn)` выполнит `fn` на игровом потоке в следующем тике — так можно вернуть результат из любого потока без корутин.

Ожидания планировщика (`seconds`, `frames`, `event`, …) нельзя использовать на фоновом потоке — сработает assert. Сначала вернитесь через `mainThread()`.

Полный пример: `samples/guide_examples/08-coroutines/pathfinding.cpp`.

## Шаг 8. Отмена

| Способ | Когда |
| --- | --- |
| `handle.cancel()` | Остановить конкретную корутину. Потокобезопасно. |
| `sched.cancelOwner(entityId)` | Сущность уничтожена. Возвращает число отменённых. |
| `SpawnOptions::token` + `CancellationSource::cancel()` | Отменить группу (например, все эффекты квеста). Источники можно связывать: `CancellationSource child(parent.token())`. |
| `co_await spawnChild(task, "name")` | Дочерняя корутина: не ждём её, но она умирает вместе с родителем и наследует владельца. |
| `sched.cancelAll()` | Смена сцены. |

```cpp
CancellationSource questEffects;
sched.spawn(burn(alive, ticks), {.name = "QuestFx", .token = questEffects.token()});
questEffects.cancel();

auto boss = sched.spawn([&]() -> Task<> {
    sparks = co_await spawnChild(burn(alive, ticks), "Sparks");
    co_await seconds(100.0);
}, {.name = "BossFight", .owner = 5});
boss.cancel();   // искры погасли вместе с боссом
```

Подвешенная корутина отменяется сразу: весь стек вложенных задач уничтожается, деструкторы выполняются. Если корутина отменяет сама себя (или её отменили, пока она выполняется), раскрутка произойдёт на ближайшей точке ожидания.

Пока часть задачи выполняется на **фоновом потоке**, уничтожить её нельзя — отмена откладывается до возвращения на игровой поток. Длинные фоновые циклы должны сами проверять флаг:

```cpp
co_await backgroundThread();
auto cancel = co_await currentCancellation();
while (!cancel.isCancelled()) { /* порция работы */ }
co_await mainThread();   // здесь отменённая задача и будет уничтожена
```

Полный пример: `samples/guide_examples/08-coroutines/cancellation.cpp`.

## Шаг 9. Отладка: имена, интроспекция, утечки

- Имя задаётся в `SpawnOptions::name` или изнутри: `co_await named("OpenDoor")`.
- `sched.coroutines()` возвращает снимок `CoroutineInfo` для панели «Coroutines» редактора: `id`, `name`, `owner`, `parentId`, `state`, `waitingOn` (например, `"seconds(1.50 left)"`, `"future"`, `"event"`, `"background"`), возраст в секундах и кадрах.
- `sched.handlesForOwner(id)` — все корутины сущности.
- `shutdown()` (и деструктор планировщика) пишет в лог предупреждение `[async]` о каждой ещё живой корутине и отменяет их — это отчёт об утечках.
- `coroutineFrameStats()` (`detail/frame_pool.hpp`) — число живых кадров корутин и статистика пула. Кадры выделяются из пула, поэтому массовый запуск корутин не нагружает кучу.

```cpp
std::vector<CoroutineInfo> list = sched.coroutines();
// list[0].name == "OpenDoor", list[0].owner == 9, list[0].waitingOn == "seconds(1.50 left)"
```

Полный пример: `samples/guide_examples/08-coroutines/cancellation.cpp`.

## Шаг 10. Сетевой запрос с тайм-аутом

`Oxwald::async_net` добавляет запрос/ответ поверх «выстрелил и забыл» RPC из [главы 14](14-networking.md). Подключите в CMake `Oxwald::async_net` и заголовок `<oxwald/async/net_rpc.hpp>`.

```cpp
// Сервер: обработчик возвращает ответ, исключение уходит клиенту как AsyncError.
net::serveRequest<Inventory, u32>(server, "inv.fetch", [](net::PeerId from, u32 slot) {
    if (slot > 3) throw std::runtime_error("no such slot");
    return Inventory{{"sword", "potion"}};
});

// Клиент: RpcCall<Ответ, Аргументы...> держите членом класса, пока возможны запросы.
net::RpcCall<Inventory, u32> fetch(client, "inv.fetch");

Task<> openInventory(Hud& hud, net::RpcCall<Inventory, u32>& fetch, u32 slot) {
    hud.spinner = true;
    std::optional<Inventory> inv;
    try {
        inv = co_await realTimeout(fetch(slot), 3.0);   // сеть живёт в реальном времени, даже на паузе
    } catch (const AsyncError& e) {
        hud.toasts.push_back(e.what());                 // исключение на сервере или нет соединения
    }
    hud.spinner = false;
    if (inv) hud.shown = std::move(*inv);
    else if (hud.toasts.empty()) hud.toasts.push_back("Сервер не ответил");
}
```

- Тип ответа должен иметь конструктор по умолчанию и `NetCodec` (как писать свой — в [главе 14](14-networking.md)).
- Ответы приходят из `client.poll()` на игровом потоке; корутина продолжится в следующем `tick`.
- Без соединения `fetch(...)` сразу возвращает ошибочный `Future` (`"rpc '...': not connected"`).
- Запрос, по которому истёк тайм-аут, остаётся в `fetch.pending()` до ответа или `failAll(reason)` — вызывайте `failAll` при разрыве соединения.

Полный пример: `samples/guide_examples/08-coroutines/net_rpc.cpp`.

## Шаг 11. Lua: `await` и вызов скриптов из C++

`script::AsyncBridge` ([`async_bridge.hpp`](../../engine/script/include/oxwald/script/async_bridge.hpp)) соединяет `Future` с планировщиком Lua-корутин (`spawn`/`wait` из [главы 15](15-scripting-lua.md)). В движке он уже создан модулем `script` и лежит в `Services`.

C++: отдаём `Future` в Lua через `bridge.wrap(...)`:

```cpp
ScriptVM vm(ScriptVMConfig{.hotReloadInterval = 0.0});
AsyncBridge bridge(vm);   // регистрирует `await`, `async.await` и тип Future во всех песочницах
vm.bindApi("assets", [&](sol::state_view, sol::table& api) {
    api["load"] = [&](const std::string& path) { return bridge.wrap(assets.loadAsync(path)); };
});

// Каждый кадр, на игровом потоке, именно в таком порядке:
bridge.update();   // будит Lua-корутины, чьи Future завершились (с любого потока)
vm.update(dt);
```

Lua: `await` внутри `spawn`-корутины «паркует» её без опроса, пока `Future` не завершится:

```lua
function onStart(self)
    spawn(function()
        -- Ошибка C++ Future превращается в Lua-ошибку: ловим pcall.
        local ok, err = pcall(function() return await(assets.load("sfx/missing.ogg")) end)
        if not ok then self.loadError = err end
    end)
end

function open(self, degreesPerSecond)
    self.state = "opening"
    self.clip = await(assets.load("anims/door_open.anim"))
    wait(self.openAngle / degreesPerSecond)
    self.state = "open"
    return self.openAngle
end
```

У объекта `Future` в Lua есть методы `f:isReady()`, `f:hasError()`, `f:error()`, `f:get()`. `wrap` поддерживает `void`, `bool`, числа, `std::string`, glm `vec2/3/4/quat`, `ScriptValue`, `sol::object` (таблицы) и всё, что умеет sol2.

Обратное направление — C++ ждёт Lua-функцию, которая сама может `wait()`/`await()`:

```cpp
sched.spawn([&]() -> Task<> {
    // open(self, 45) как Lua-корутина, принадлежащая экземпляру скрипта.
    sol::main_object r = co_await bridge.invoke(*door, "open", 45.0);
    openedTo = r.as<f64>();
});
```

| Вызов | Что возвращает |
| --- | --- |
| `bridge.call(fn, args...)` | `Future<sol::main_object>` — первое возвращённое значение; Lua-ошибка → `AsyncError`. |
| `bridge.invoke(instance, "fn", args...)` | То же для `fn(self, args...)` экземпляра; уничтожение экземпляра отменяет корутину. |
| `bridge.callValue(fn, args...)` | `Future<ScriptValue>` — простые данные, таблицы становятся `monostate`. |

Объекты sol (`sol::main_object`, таблицы) трогайте только на игровом потоке — ждите эти `Future` из корутин планировщика.

Полный пример: `samples/guide_examples/08-coroutines/lua_await.cpp` и `door.lua`.

## Шаг 12. Генераторы

`Generator<T>` не связан с планировщиком — это обычный ленивый генератор для `for`:

```cpp
Generator<Cell> cellsAround(Cell c, int radius) {
    for (int y = c.y - radius; y <= c.y + radius; ++y)
        for (int x = c.x - radius; x <= c.x + radius; ++x) co_yield Cell{x, y};
}
for (Cell c : cellsAround({10, 5}, 1)) { /* 9 клеток без промежуточного вектора */ }
```

Внутри генератора `co_await` запрещён (ошибка компиляции).

## Типичные ошибки и подводные камни

- **`sched.spawn([&]() -> Task<> {...}())` — со скобками в конце.** Лямбда-временный объект умрёт сразу после вызова, а кадр корутины останется со ссылками на её захваты. Передавайте саму лямбду: `sched.spawn([&]() -> Task<> {...})` — тогда `spawn` сохранит её.
- **Ссылки в параметрах корутины.** `Task<> f(const std::string& s)` хранит ссылку, а не копию. Если корутина переживёт аргумент, будет висячая ссылка. Принимайте по значению всё, что может умереть раньше корутины (временные строки, локальные переменные вызывающего).
- **`Task` ленив.** `loadMesh(path);` без `co_await` или `spawn` ничего не делает (компилятор предупредит: `Task` помечен `[[nodiscard]]`).
- **Ожидания планировщика на фоновом потоке.** `seconds`, `frames`, `event`, `until` после `backgroundThread()` — assert. Сначала `co_await mainThread()`.
- **Игровое состояние на фоновом потоке.** Читайте и пишите компоненты только до `backgroundThread()` и после `mainThread()`.
- **`cancelOwner` после удаления компонентов.** Деструкторы в корутине могут обращаться к сущности — отменяйте раньше.
- **Длинный фоновый цикл без `currentCancellation()`.** Отмена такой задачи будет ждать конца цикла.
- **`seconds` в меню паузы.** Игровое время стоит, корутина не проснётся. Для UI — `realSeconds`/`realTimeout`.
- **`Future::wait()` на игровом потоке** для `Future`, который завершается в `tick()` этого же планировщика, — взаимоблокировка. На игровом потоке только `co_await`.
- **Move-only значение в `Future`**, которое ждут несколько корутин: первая заберёт значение, остальные получат «пустое». Такие `Future` ждите из одного места.
- **`whenAny` вне планировщика** (без `spawn`) не может уничтожить проигравших и ждёт их завершения.
- **Lua `await` вне `spawn`** — ошибка «... spawn()». Уже готовый `Future` можно `await`-ить где угодно.
- **Lua `await` продолжается на кадр позже**, чем завершился C++ `Future` (`bridge.update()` будит, следующий `vm.update()` возобновляет).
- **`RpcCall` уничтожен, пока запросы в полёте** — они завершатся ошибкой `"rpc '...' destroyed"`. Держите `RpcCall` членом долгоживущего объекта.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`async.hpp`](../../engine/async/include/oxwald/async/async.hpp) | Общий заголовок модуля |
| [`task.hpp`](../../engine/async/include/oxwald/async/task.hpp) | `Task<T>`, `toTask` |
| [`scheduler.hpp`](../../engine/async/include/oxwald/async/scheduler.hpp) | `CoroutineScheduler`, `CoroutineHandle`, `SpawnOptions`, `CoroutineInfo`, `nextFrame`, `frames`, `seconds`, `realSeconds`, `nextFixedUpdate`, `until`, `whileTrue`, `event`, `mainThread`, `backgroundThread`, `switchTo`, `named`, `currentCancellation`, `currentScheduler`, `spawnChild` |
| [`future.hpp`](../../engine/async/include/oxwald/async/future.hpp) | `Promise<T>`, `Future<T>`, `AsyncError`, `BrokenPromise`, `runAsync`, `awaitCallback`, `makeReadyFuture`, `makeErrorFuture` |
| [`combinators.hpp`](../../engine/async/include/oxwald/async/combinators.hpp) | `whenAll`, `whenAny`, `timeout`, `realTimeout` |
| [`cancellation.hpp`](../../engine/async/include/oxwald/async/cancellation.hpp) | `CancellationSource`, `CancellationToken`, `CancellationRegistration` |
| [`executor.hpp`](../../engine/async/include/oxwald/async/executor.hpp) | `IExecutor`, `ThreadPoolExecutor`, `InlineExecutor`, `JobSystemExecutor` |
| [`generator.hpp`](../../engine/async/include/oxwald/async/generator.hpp) | `Generator<T>` |
| [`net_rpc.hpp`](../../engine/async/net/include/oxwald/async/net_rpc.hpp) | `RpcCall<Resp, Args...>`, `serveRequest` (таргет `Oxwald::async_net`) |
| [`async_bridge.hpp`](../../engine/script/include/oxwald/script/async_bridge.hpp) | `script::AsyncBridge` — Lua `await`, `call`/`invoke`/`callValue` |

Заметки для разработчиков модуля (пул кадров, замеры, ограничения): [`docs/dev/modules/async.md`](../dev/modules/async.md).

## Что дальше

- [05. Runtime и игровой цикл](05-runtime.md) — где тикается планировщик и как получить его из `Services`.
- [14. Сеть](14-networking.md) — транспорт, сообщения и RPC, на которых построен `RpcCall`.
- [15. Скрипты на Lua](15-scripting-lua.md) — `spawn`, `wait`, таймеры и экземпляры скриптов.
- [13. ИИ](13-ai.md) — навмеш и поиск пути, которые удобно запускать в фоне.
- [10. Анимация](10-animation.md) — события аниматора, которых ждут корутины.
- [Оглавление](README.md).
