# 01. Ядро (core)

## Зачем

`Oxwald::core` — фундамент, на котором стоят все остальные модули: логирование, проверки инвариантов, обработка
ошибок, математика, идентификаторы, внедрение зависимостей, события, многопоточность, файловая система,
аллокаторы и отладочная отрисовка. Почти любой ваш игровой код будет использовать что-то отсюда, поэтому стоит
один раз узнать, «как здесь принято».

Подключение: `target_link_libraries(MyGame PRIVATE Oxwald::core)` (или любой модуль выше — `core` придёт транзитивно).

## Ключевые понятия

| Понятие | Где | Коротко |
| --- | --- | --- |
| Лог | `log.hpp` | `OX_LOG_INFO("категория", "формат {}", x)` — `std::format`, приёмники (sinks) |
| Assert | `assert.hpp` | `OX_ASSERT(cond, "формат", ...)` — активен **во всех сборках** |
| Result | `result.hpp` | `ox::Result<T>` / `ox::Status` — ошибки без исключений |
| Математика | `math.hpp` | glm + `Transform`, `AABB`, `Sphere`, `Plane`, `OBB`, `Frustum`, `Ray`, `Random` |
| UUID и хэши | `uuid.hpp`, `hash.hpp` | `Uuid`, `fnv1a64`, `"..."_hash`, `hashCombine`, `crc32` |
| Сервисы (DI) | `services.hpp` | контейнер «один экземпляр на тип интерфейса» |
| События | `events.hpp` | `Signal<Args...>`, `ScopedConnection`, `EventBus` |
| Задачи | `jobs.hpp` | `JobSystem` (enkiTS): `parallelFor`, `submit`, `TaskGroup`, очередь главного потока |
| Время | `time.hpp` | `Clock`, `Stopwatch`, `FixedTimestep`, `FrameTimer` |
| Профилирование | `profile.hpp` | `OX_PROFILE_*` → Tracy или пустые макросы |
| Файлы | `vfs.hpp`, `paths.hpp`, `file_watcher.hpp` | `engine://`, `project://`, `user://`; hot reload |
| Память | `memory.hpp` | `FrameAllocator`, `PoolAllocator`, `TypedPool`, `HandlePool` |
| Debug draw | `debug_draw.hpp` | линии, сферы, стрелки, текст с временем жизни |

Типы фиксированной ширины берутся из `<oxwald/core/types.hpp>`: `u8 … u64`, `i8 … i64`, `f32`, `f64`, `usize`.

## Логирование

```cpp
#include <oxwald/core/log.hpp>

OX_LOG_INFO("game", "player {} joined, hp={}", "Ivan", 100);
OX_LOG_WARN("game", "low fps: {:.1f}", 24.5);
OX_LOG_ERROR("net", "connection lost: {}", reason);
```

Уровни: `Trace`, `Debug`, `Info`, `Warn`, `Error`, `Fatal`. Сообщения ниже `log::minLevel()` (по умолчанию
`Info`) отбрасываются **до** форматирования — дешёвые отладочные логи можно не убирать. Категория — короткая строка
модуля/подсистемы (`"physics"`, `"ai"`, `"game"`): по ней удобно фильтровать.

Все сообщения печатаются в stderr; дополнительно можно подключить свой приёмник (sink) — файл, внутриигровую
консоль, телеметрию:

```cpp
std::vector<std::string> lines;
const int sink = ox::log::addSink([&](const ox::log::Record& r) {
    lines.push_back(std::format("{}|{}|{}", ox::log::levelName(r.level), r.category, r.message));
});
ox::log::setMinLevel(ox::log::Level::Info);
OX_LOG_DEBUG("game", "не попадёт в лог: уровень ниже Info");
OX_LOG_INFO("game", "player {} joined, hp={}", "Ivan", 100);   // -> "info|game|player Ivan joined, hp=100"
ox::log::removeSink(sink);
```

Полный пример: `samples/guide_examples/01-core/log_result.cpp`.

Приёмники вызываются под мьютексом из того потока, который пишет в лог. Консоль движка (`Console::captureLog`,
см. [04](04-cvars-quality.md)) использует тот же механизм.

### Из Lua

В каждой песочнице Lua есть таблица `log` (`trace`, `debug`, `info`, `warn`, `error`), а `print` пишет в лог с
уровнем `Info`. Категория — `"script"` (настраивается `ScriptVMConfig::logCategory`), к сообщению добавляется
`файл:строка`:

```lua
log.info("player spawned at", 1, 2, 3)   -- аргументы склеиваются через пробел
local hp = 15
if hp < 20 then
    log.warn("hp low: " .. hp)
end
print("print goes to the log too")
```

Полный пример: `samples/guide_examples/01-core/lua_log.cpp` + `log_example.lua`. Подробнее о Lua — в
[15-scripting-lua](15-scripting-lua.md).

## Проверки: OX_ASSERT

```cpp
#include <oxwald/core/assert.hpp>

OX_ASSERT(players > 0, "need at least one player, got {}", players);
OX_ASSERT(ptr != nullptr);           // сообщение необязательно
OX_UNREACHABLE();                    // в ветке, куда управление попасть не должно
```

`OX_ASSERT` **работает и в Release**: пишет `Fatal`-запись в лог (файл, строка, выражение, сообщение) и вызывает
`std::abort()`. Это сознательное решение: проверки инвариантов дешёвые, а тихо испорченные данные отлаживать
гораздо дороже. Поэтому не кладите в assert ничего тяжёлого и не используйте его для ожидаемых ошибок — для них
есть `Result`.

## Ошибки: Result и Status

Движок собирается как C++20, поэтому вместо `std::expected` — собственный `ox::Result<T>`; `ox::Status` — это
`Result<void>`. Исключения через границы модулей не бросаются.

```cpp
#include <oxwald/core/result.hpp>

ox::Result<int> parseHealth(std::string_view text) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size())
        return ox::makeError("'{}' is not a number", text);        // ошибка с форматированием
    if (value < 0)
        return ox::makeError("health must be >= 0, got {}", value);
    return value;                                                  // значение
}

ox::Status applyDamage(int& health, std::string_view amountText) {
    auto amount = parseHealth(amountText);
    if (!amount) return amount.error();                            // пробрасываем наверх
    health -= *amount;
    return {};                                                     // успех
}

auto hp = parseHealth("75");
if (hp) use(*hp); else OX_LOG_WARN("game", "{}", hp.error().message);
int safe = parseHealth("-5").valueOr(0);
```

Полный пример: `samples/guide_examples/01-core/log_result.cpp`.

Обращение к `value()`/`*` у результата с ошибкой (и к `error()` у успешного) — это `OX_ASSERT`, то есть аварийное
завершение. Всегда проверяйте `if (result)` сначала.

## Математика

Движок использует [glm](https://github.com/g-truc/glm) с соглашениями: правая система координат, **Y вверх**,
**−Z — «вперёд»** (`kWorldForward`), глубина в диапазоне [0, 1], в рендерере — **reversed-Z** (ближняя плоскость = 1,
дальняя = 0). Повороты в рантайме — только кватернионы (`glm::quat`); углы Эйлера — лишь для отображения в
редакторе. Углы в функциях — в радианах (`toRadians`/`toDegrees`).

### Transform

`Transform{position, rotation, scale}`, матрица = T·R·S.

```cpp
#include <oxwald/core/math.hpp>
using namespace ox;

Transform parent;
parent.position = {10.0f, 0.0f, 0.0f};
parent.rotation = glm::angleAxis(toRadians(90.0f), kWorldUp);   // 90° вокруг Y

Transform child;
child.position = {0.0f, 0.0f, -2.0f};                           // 2 м «впереди» родителя

Transform world = parent * child;          // = compose(parent, child) -> позиция (8, 0, 0)
glm::vec3 fwd = parent.forward();          // (-1, 0, 0); также right(), up()
glm::mat4 m = world.toMatrix();            // и Transform::fromMatrix(m)
glm::vec3 local = parent.inverse().transformPoint(world.position);   // обратно в локальные координаты

// Камера, смотрящая на цель: lookRotation направляет -Z вдоль forward.
Transform camera{eye, lookRotation(target - eye)};
```

Также есть `transformVector` (поворот + масштаб), `transformDirection` (только поворот), `Transform::lerp`
(slerp для поворота), `fromToRotation`, `decompose`, `nearlyEqual` для векторов/кватернионов/матриц/трансформов.

### Объёмы, фрустум, лучи

```cpp
AABB box;                                   // по умолчанию ПУСТОЙ (valid() == false)
box.expand(glm::vec3{-1, 0, -1});
box.expand(glm::vec3{1, 2, 1});             // center() = (0,1,0), extents() = (1,1,1) — половина размера

const glm::mat4 view = glm::lookAt(glm::vec3{0, 1, 10}, glm::vec3{0, 1, 0}, kWorldUp);
const glm::mat4 proj = perspectiveReversedZ(toRadians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
const Frustum frustum = Frustum::fromViewProj(proj * view);   // reversedZ = true по умолчанию
bool visible = frustum.intersects(box);                       // консервативный тест

const Ray ray{{0, 1, 10}, {0, 0, -1}};
if (auto t = intersectRayAABB(ray, box)) hitPoint = ray.at(*t);   // t = 9
if (auto hit = intersectRayTriangle(ray, a, b, c)) { hit->t; hit->normal; hit->u; hit->v; hit->frontFace; }
```

Есть также `Sphere`, `Plane` (`dot(n, p) + d = 0`), `OBB`, `intersectRaySphere/Plane`, `AABB::transformed(mat4)`,
проекции `perspectiveInfiniteReversedZ` и `orthoReversedZ`.

### Детерминированный Random

`ox::Random` (xoshiro256**) выдаёт одну и ту же последовательность при одинаковом seed на всех платформах — важно
для процедурной генерации, сетевой игры и воспроизводимых тестов. Не потокобезопасен: один экземпляр на поток.

```cpp
Random rng(42);
int dice = rng.rangeInt(1, 6);       // включительно
float f = rng.range(0.0f, 10.0f);    // [a, b)
bool crit = rng.chance(0.1f);
glm::vec3 dir = rng.unitVector();
glm::quat q = rng.rotation();
```

Полный пример: `samples/guide_examples/01-core/math.cpp`.

## UUID и хэши

```cpp
#include <oxwald/core/hash.hpp>
#include <oxwald/core/uuid.hpp>
using namespace ox::literals;

ox::Uuid id = ox::Uuid::generate();                 // случайный v4
std::string s = id.toString();                      // "8-4-4-4-12"
std::optional<ox::Uuid> back = ox::Uuid::parse(s);  // nullopt при мусоре
ox::Uuid builtin = ox::Uuid::fromName("builtin/cube");   // детерминированный
bool none = ox::Uuid{}.isNil();                     // нулевой UUID = «нет ссылки»
std::unordered_map<ox::Uuid, std::string> names;    // std::hash<Uuid> определён

// FNV-1a 64 вычисляется при компиляции — годится для switch:
switch (eventId) {
case "player.died"_hash: ...; break;
case "player.spawned"_hash: ...; break;
}
ox::u64 key = 0;
ox::hashCombineInto(key, std::string("Shadows"));   // составной ключ
ox::hashCombineInto(key, 2048);
ox::u32 crc = ox::crc32(data, size);                // CRC-32 (IEEE), как в архивах OXB1
```

UUID'ы идентифицируют сущности сцен и ассеты (`.meta`-файлы); хэши строк — имена типов, полей, событий.

Полный пример: `samples/guide_examples/01-core/ids_hashes.cpp`.

## Сервисы (внедрение зависимостей, DI)

В движке нет глобальных синглтонов для сервисов. Всё, что живёт «одно на движок» (физический мир, аудио, VM Lua,
файловая система, система задач, …), регистрируется в контейнере `ox::Services` — по одному экземпляру на тип
**интерфейса**. `Engine` создаёт контейнер и передаёт его системам и модулям (`engine.services()`,
`SystemContext::services`, `IEngineModule::init(engine, services)`).

```cpp
#include <oxwald/core/services.hpp>

class IScoreService {
public:
    virtual ~IScoreService() = default;
    virtual void add(int points) = 0;
    virtual int total() const = 0;
};
class LocalScore final : public IScoreService { /* ... */ };

class Achievements {                                  // зависит от другого сервиса
public:
    explicit Achievements(IScoreService& score) : m_score(score) {}
    bool unlocked() const { return m_score.total() >= 100; }
private:
    IScoreService& m_score;
};

ox::Services services;
services.add<IScoreService>(std::make_unique<LocalScore>());      // регистрация по интерфейсу
services.emplace<Achievements>(services.get<IScoreService>());   // по конкретному типу

services.get<IScoreService>().add(120);      // get: assert, если сервиса нет
if (auto* a = services.tryGet<Achievements>()) a->unlocked();   // tryGet: nullptr, если нет
```

Полный пример: `samples/guide_examples/01-core/services_events.cpp`.

- Сервисы уничтожаются **в обратном порядке регистрации** — поздние могут зависеть от ранних.
- `addExternal<T>(ref)` регистрирует объект, которым владеете вы; `remove<T>()` удаляет сервис.
- Ключ — ровно тот тип, который указан при регистрации: зарегистрированный как `IScoreService` сервис не найдётся
  через `tryGet<LocalScore>()`.

## События и сигналы

### Signal

`ox::Signal<Args...>` — типобезопасный сигнал/слот. `connect` возвращает `Connection`; `ScopedConnection`
отключает слот в деструкторе — храните её как член класса, и подписка умрёт вместе с объектом.

```cpp
#include <oxwald/core/events.hpp>

ox::Signal<int, const std::string&> damaged;          // (урон, источник)
int total = 0;
{
    ox::ScopedConnection c = damaged.connect([&](int amount, const std::string&) { total += amount; });
    damaged.emit(10, "fall");
    damaged(5, "fire");                                // operator() == emit
}                                                      // отключено автоматически
damaged.emit(100, "ignored");                          // total == 15
```

`connect`/`disconnect`/`emit` потокобезопасны; слоты вызываются в потоке, который вызвал `emit`, в порядке
подключения. Сигналы есть у многих объектов движка: `Engine::levelLoaded`, `Engine::frameEnded`,
`Settings::changed`, `Console::cvarChanged`, …

### EventBus

`ox::EventBus` — шина событий «опубликовал — кто-то подписан», где событие — любая структура.

```cpp
struct EnemyKilled { std::string enemy; int points = 0; };

ox::EventBus bus;          // в движке уже есть сервис: engine.services().get<ox::EventBus>()
ox::ScopedConnection sub = bus.subscribe<EnemyKilled>([&](const EnemyKilled& e) { score += e.points; });

bus.publish(EnemyKilled{"orc", 10});      // немедленно, в текущем потоке
bus.enqueue(EnemyKilled{"troll", 50});    // из любого потока — в очередь
bus.dispatch();                           // доставить очередь (движок делает это раз в кадр на главном потоке)
```

Полный пример: `samples/guide_examples/01-core/services_events.cpp`.

Правило: из рабочих потоков используйте `enqueue` — тогда обработчики выполнятся на главном потоке в начале
следующего кадра, и им не нужна синхронизация с игровым кодом.

## Система задач (job system)

`ox::JobSystem` — пул потоков поверх enkiTS. Поток, создавший систему, — «главный» (индекс 0). В движке система уже
создана и зарегистрирована: `engine.jobs()` или `services.get<ox::JobSystem>()`.

```cpp
#include <oxwald/core/jobs.hpp>

ox::JobSystem jobs(4);   // 4 потока, включая текущий; 0 = по числу ядер

// Параллельный цикл: [0, count) режется на куски не меньше grain; вызов блокирующий.
jobs.parallelFor(static_cast<ox::u32>(heights.size()), 256, [&](ox::u32 begin, ox::u32 end, ox::u32 thread) {
    for (ox::u32 i = begin; i < end; ++i) heights[i] = computeHeight(i);
});

ox::JobHandle h = jobs.submit([&] { buildNavMeshTile(); });
jobs.wait(h);            // ожидающий поток выполняет другие задачи — дедлока нет

{
    ox::TaskGroup group(jobs);                       // fork/join
    for (int i = 1; i <= 10; ++i) group.run([&sum, i] { sum += i; });
    group.wait();                                    // деструктор тоже ждёт
}

// Результат фоновой работы — обратно на главный поток:
jobs.submit([&] {
    const int computed = heavyWork();
    jobs.enqueueMainThread([&appliedOnMain, computed] { appliedOnMain = computed; });
});
jobs.runMainThreadQueue();                           // движок вызывает раз в кадр
```

Полный пример: `samples/guide_examples/01-core/jobs.cpp`.

Для асинхронного игрового кода (`co_await` загрузки, ожидание кадров и таймеров) удобнее корутины —
см. [08-coroutines](08-coroutines.md); они работают поверх этой же системы задач.

## Время и фиксированный шаг

```cpp
#include <oxwald/core/time.hpp>

ox::FixedTimestep fixed(1.0 / 60.0, /*maxStepsPerFrame*/ 8);
const ox::u32 steps = fixed.advance(frameDt);
for (ox::u32 i = 0; i < steps; ++i) simulate(fixed.fixedDt());
render(interpolate(prev, curr, fixed.alpha()));     // alpha в [0, 1)
// Если накопилось больше 8 шагов (зависание, точка останова), лишнее отбрасывается: fixed.droppedTime().
```

Игровой цикл `Engine` уже делает это сам (фаза `FixedUpdate`, частота из проекта), вам это нужно в своих
инструментах и тестах. Для замеров — `ox::Stopwatch` (`start/stop/lap/elapsedMs`), `ox::Clock::now()` (секунды) и
`ox::FrameTimer` (сглаженный FPS).

## Профилирование (Tracy)

```cpp
#include <oxwald/core/profile.hpp>

void AISystem::update(ox::SystemContext& ctx) {
    OX_PROFILE_ZONE();                        // зона с именем функции
    {
        OX_PROFILE_ZONE_N("Perception");      // имя — строковый литерал
        ...
    }
    OX_PROFILE_PLOT("AI agents", static_cast<int64_t>(agentCount));
}
```

Ещё: `OX_PROFILE_ZONE_C(name, 0xRRGGBB)`, `OX_PROFILE_FRAME()` / `OX_PROFILE_FRAME_N("Render")`,
`OX_PROFILE_ALLOC/FREE`, `OX_PROFILE_THREAD_NAME`, `OX_PROFILE_MESSAGE`. Когда сборка сконфигурирована с
`OX_ENABLE_TRACY=ON` (по умолчанию в `debug`/`dev`) и пакет Tracy установлен, макросы превращаются в зоны Tracy —
подключитесь к игре программой Tracy Profiler. Иначе (пресет `release`) это пустые макросы без побочных эффектов.
Движок уже размечает кадр, фазы систем, рендер и ожидания.

Полный пример: `samples/guide_examples/01-core/time_profile.cpp`.

## Файлы: VFS, пути, отслеживание изменений

### Виртуальная файловая система

Игровой код обращается к файлам по URI `схема://путь`:

| Схема | Что | Запись |
| --- | --- | --- |
| `engine://` | исходники движка: шейдеры, встроенные ассеты (`paths::engineSourceDir()`) | нет |
| `project://` | каталог проекта (в редакторе — с записью) | только редактор |
| `user://` | данные пользователя: `settings.json`, `saves/` | да |

В движке VFS — сервис `engine.vfs()`. Можно создать и свою:

```cpp
#include <oxwald/core/vfs.hpp>

ox::Vfs vfs;
vfs.mount("project", std::make_unique<ox::DirectoryMount>(root / "game", /*writable*/ false));
// Больший приоритет перекрывает одноимённые файлы (так подключаются патчи и .oxpak-архивы).
vfs.mount("project", std::make_unique<ox::DirectoryMount>(root / "patch", false), /*priority*/ 10);
vfs.mount("user", std::make_unique<ox::DirectoryMount>(root / "user"));

auto text = vfs.readText("project://levels/b.txt");              // Result<std::string>
auto bytes = vfs.readBytes("project://textures/grass.ktx2");      // Result<std::vector<std::byte>>
auto files = vfs.list("project://levels", /*recursive*/ false);   // полные URI, отсортированы
vfs.writeText("user://saves/notes.txt", "hello");                  // запись атомарна, каталоги создаются
auto native = vfs.resolveNative("user://saves/notes.txt");         // путь на диске (если он есть)
```

Полный пример: `samples/guide_examples/01-core/vfs.cpp`.

Пути нормализуются; `..`, выводящие за пределы точки монтирования, абсолютные пути и буквы дисков отклоняются —
скрипт или мод не сможет прочитать файл вне песочницы. Свой источник (архив, сеть, память) — это реализация
`IMountSource`.

Стандартные каталоги — `ox::paths`: `engineSourceDir()` (переопределяется переменной окружения
`OXWALD_SOURCE_DIR`), `executableDir()`, `userDataDir("MyGame")` (`~/Library/Application Support/MyGame`,
`~/.local/share/MyGame`, `%APPDATA%\MyGame`), `tempDir()`.

### FileWatcher

Опрашивающий (polling) наблюдатель за файлами: основа hot reload шейдеров, скриптов и ассетов.

```cpp
#include <oxwald/core/file_watcher.hpp>

ox::FileWatcher watcher;
watcher.watchDirectory(scriptsDir, [&](const ox::FileChange& c) {
    if (c.kind != ox::FileChange::Kind::Removed) reloadScript(c.path);
}, /*recursive*/ true, {".lua"});
watcher.watchFile(configPath, onConfigChanged);   // файла может ещё не быть — его появление придёт как Added

// раз в кадр:
watcher.poll();          // колбэки выполняются в потоке, вызвавшем poll()
// или watcher.start(250ms) — сканирование в фоне, poll() только доставляет изменения
```

Полный пример: `samples/guide_examples/01-core/file_watcher.cpp`.

Изменение сообщается, когда файл перестал меняться на время debounce (`setDebounce`, по умолчанию 100 мс) —
редакторы часто пишут файл в несколько приёмов. Файлы, существовавшие на момент добавления наблюдения, не
сообщаются. В движке сервис `FileWatcher` опрашивается каждый кадр, если `EngineConfig::fileWatching = true`
(редактор).

## Аллокаторы и хэндлы

Все типы из `memory.hpp` **не потокобезопасны** — один экземпляр на поток.

```cpp
#include <oxwald/core/memory.hpp>

// Память на один кадр: выделение — сдвиг указателя, освобождение — reset() всего сразу.
ox::FrameAllocator frame(64 * 1024);
frame.reset();                                               // начало кадра
std::span<Particle> visible = frame.allocArray<Particle>(500);
auto* center = frame.create<glm::vec3>(0.0f, 1.0f, 0.0f);
// Только тривиально разрушаемые типы: frame.create<std::string>() не скомпилируется.

// Generational-хэндлы вместо указателей: устаревший хэндл не «висит», а просто перестаёт резолвиться.
ox::HandlePool<Projectile> pool;
auto arrow = pool.create(Projectile{"archer", 30.0f});
pool.destroy(arrow);
auto bolt = pool.create(Projectile{"crossbow", 50.0f});     // тот же слот, новое поколение
pool.get(arrow);                                             // nullptr
ox::u64 packed = bolt.toU64();                               // удобно для Lua, сети, сохранений
```

Полный пример: `samples/guide_examples/01-core/memory.cpp`.

Ещё: `MultiFrameAllocator` / `FrameAllocatorRing<N>` (по аллокатору на кадр «в полёте», `beginFrame(i)`),
`PoolAllocator` (блоки фиксированного размера) и `TypedPool<T>` (то же с конструкторами/деструкторами).

## Отладочная отрисовка (debug draw)

`ox::DebugDraw` — сервис, куда любой код из любого потока добавляет линии, фигуры и 3D-текст; рендерер раз в кадр
забирает их. Физика, сплайны, ИИ и редактор рисуют через него же.

```cpp
#include <oxwald/core/debug_draw.hpp>

auto& dd = engine.services().get<ox::DebugDraw>();
dd.aabb(ox::AABB::fromCenterExtents({0, 1, 0}, {1, 1, 1}), ox::debug_color::kGreen);   // на 1 кадр
dd.arrow({0, 0, 0}, {0, 2, 0}, /*headSize*/ 0.2f, glm::vec4{1, 0.5f, 0, 1});
dd.sphere({3, 1, 0}, 0.5f, ox::debug_color::kRed, /*duration*/ 2.0f, /*depthTest*/ false);   // 2 с, поверх всего
dd.text3D({0, 2.5f, 0}, "spawn point", ox::debug_color::kYellow);
```

Полный пример: `samples/guide_examples/01-core/debug_draw.cpp`.

Фигуры: `line`, `ray`, `aabb`, `box`, `obb`, `sphere`, `circle`, `capsule`, `cone`, `cylinder`, `frustum`, `arrow`,
`axes` (X красная, Y зелёная, Z синяя), `grid`, `point`, `text3D`. У каждой — цвет (`debug_color::k*` или
`glm::vec3/vec4`), длительность в секундах (0 — ровно один кадр) и флаг теста глубины. `setEnabled(false)`
выключает приём новых фигур.

## Типичные ошибки и подводные камни

- **Assert вместо обработки ошибки.** `OX_ASSERT` роняет игру и в Release. Отсутствующий файл, неверный ввод
  игрока, разрыв сети — это `Result`/`Status`, а не assert.
- **`*result` без проверки.** Разыменование `Result` с ошибкой — тоже assert. Сначала `if (!result) ...`.
- **Тяжёлая работа в приёмнике лога.** Приёмники вызываются под общим мьютексом лога; не пишите в лог изнутри
  приёмника (дедлок) и не делайте там блокирующий ввод-вывод на каждой строке.
- **Пустой `AABB` по умолчанию.** `AABB{}` невалиден (min = +inf, max = −inf), пока в него ничего не добавили.
- **Углы в градусах.** `glm::angleAxis`, `perspectiveReversedZ` и прочие ждут радианы — `toRadians(90.0f)`.
- **Неравномерный масштаб.** `Transform::inverse()` и композиция точны только при равномерном масштабе; с
  неравномерным масштабом и поворотом используйте `glm::inverse(t.toMatrix())`.
- **Обычная проекция вместо reversed-Z.** `Frustum::fromViewProj` по умолчанию ждёт reversed-Z; для обычной
  `glm::perspective` передайте `reversedZ = false`.
- **Сервис «не тот тип».** `services.get<LocalScore>()` упадёт, если регистрировали как `IScoreService`.
- **`publish` из рабочего потока.** Обработчики выполнятся в рабочем потоке и будут гоняться с игровым кодом;
  используйте `enqueue`.
- **Ссылки на локальные переменные в `submit`.** Задача может пережить функцию — захватывайте по значению или
  дождитесь `wait`. `waitAll()` и уничтожение `JobSystem` — только на главном потоке.
- **`FrameAllocator`-данные после `reset()`.** Указатели становятся недействительными; для данных, которые читает
  поток рендера в следующем кадре, берите `MultiFrameAllocator`.
- **Абсолютные пути вместо VFS.** Код, читающий `/Users/.../level.oxscene`, сломается в собранной игре и в архиве
  `.oxpak`. Пишите `project://levels/level.oxscene`.

## API

- [`log.hpp`](../../engine/core/include/oxwald/core/log.hpp), [`assert.hpp`](../../engine/core/include/oxwald/core/assert.hpp), [`result.hpp`](../../engine/core/include/oxwald/core/result.hpp), [`types.hpp`](../../engine/core/include/oxwald/core/types.hpp)
- [`math.hpp`](../../engine/core/include/oxwald/core/math.hpp) — `Transform`, `AABB`, `Sphere`, `Plane`, `OBB`, `Frustum`, `Ray`, `Random`
- [`uuid.hpp`](../../engine/core/include/oxwald/core/uuid.hpp), [`hash.hpp`](../../engine/core/include/oxwald/core/hash.hpp)
- [`services.hpp`](../../engine/core/include/oxwald/core/services.hpp), [`events.hpp`](../../engine/core/include/oxwald/core/events.hpp)
- [`jobs.hpp`](../../engine/core/include/oxwald/core/jobs.hpp), [`time.hpp`](../../engine/core/include/oxwald/core/time.hpp), [`profile.hpp`](../../engine/core/include/oxwald/core/profile.hpp)
- [`vfs.hpp`](../../engine/core/include/oxwald/core/vfs.hpp), [`paths.hpp`](../../engine/core/include/oxwald/core/paths.hpp), [`file_watcher.hpp`](../../engine/core/include/oxwald/core/file_watcher.hpp)
- [`memory.hpp`](../../engine/core/include/oxwald/core/memory.hpp), [`debug_draw.hpp`](../../engine/core/include/oxwald/core/debug_draw.hpp)
- [`script_vm.hpp`](../../engine/script/include/oxwald/script/script_vm.hpp) — Lua `log.*`

## Что дальше

- [00. Начало работы](00-getting-started.md)
- [02. Рефлексия и сериализация](02-reflection-serialization.md) — описать свои типы для редактора, сохранений и сети.
- [03. ECS и сцены](03-ecs-scene.md) — сущности, компоненты, системы.
- [04. CVar'ы и качество графики](04-cvars-quality.md)
- [08. Корутины](08-coroutines.md) — асинхронный код поверх системы задач.
- [Оглавление](README.md)
