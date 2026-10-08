# 07. Сохранения

## Зачем

Хорошая система сохранений кажется простой, пока не столкнёшься с деталями:

- сохранять нужно не весь уровень, а только то, что изменилось в игре;
- ссылки между объектами должны остаться целыми после загрузки;
- старые сохранения должны открываться в новой версии игры;
- если игра упала посреди записи, предыдущее сохранение не должно испортиться;
- запись не должна подвешивать кадр.

`ox::SaveGameSystem` из модуля `runtime` закрывает всё это. В `Engine` система уже создана и доступна как `engine.saves()`.

Модель простая. Уровень (`.oxscene`) — неизменяемая основа. Сохранение хранит только **разницу** с ней: изменённые поля, заспавненные и уничтоженные сущности, а также ваши данные вне ECS (квесты, инвентарь, статистику). При загрузке уровень грузится заново, и сохранение накладывается поверх.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| Слот (slot) | Имя сохранения → файл `user://saves/<slot>.oxsave` (+ резервная копия `.oxsave.bak`) |
| `attr::SaveGame` | Атрибут поля компонента: «сохранять это поле у сущностей уровня» |
| `SaveGameComponent` | Компонент-метка: «сохранять сущность целиком» (все компоненты и трансформ) |
| `ISaveable` | Ваша секция данных вне ECS: `saveId()`, `save(Writer&)`, `load(Reader&)`, `onMissing()` |
| Версия данных | `SaveGameConfig::version` (в проекте — `saveVersion`). Старые файлы обновляются миграциями |
| Миграция | Функция `Status(serial::Document&)`, которая поднимает документ с версии N до N+1 |
| `SaveKind` | `Manual`, `Auto`, `Quick` — для меню загрузки |

### Что попадает в сохранение

| Сущность | Что сохраняется | Что происходит при загрузке |
| --- | --- | --- |
| С `SaveGameComponent` | Все сериализуемые компоненты (Transform — если `saveTransform == true`), имя, родитель | Пересоздаётся с тем же UUID, если её нет. Компоненты, добавленные после сохранения, удаляются |
| Сущность уровня с полями `attr::SaveGame` | Только эти поля | Значения применяются к сущности уровня с тем же UUID. Остальные поля берутся из уровня |
| Сущность уровня, уничтоженная в игре | UUID в списке `destroyed` | Уничтожается после загрузки уровня |
| Сущность с `SaveGameComponent`, появившаяся после сохранения | — | Уничтожается |
| Прочие сущности | Ничего | Остаются как в уровне |

Ссылки `EntityRef` остаются валидными, потому что UUID сохраняются.

## Шаг 1. Помечаем данные

```cpp
struct HealthComponent {
    ox::f32 health = 100.0f;    // меняется в игре -> сохраняем
    ox::f32 maxHealth = 100.0f; // данные уровня -> берём из сцены
};

struct InventoryComponent {
    std::vector<std::string> items;
    ox::i32 gold = 0;
};

ox::registerSceneTypes();
ox::registerSaveGameTypes();
OX_REFLECT_TYPE(HealthComponent, "Game.Health")
    .field("health", &HealthComponent::health, ox::attr::SaveGame{})
    .field("maxHealth", &HealthComponent::maxHealth);
OX_REFLECT_TYPE(InventoryComponent, "Game.Inventory")
    .field("items", &InventoryComponent::items)
    .field("gold", &InventoryComponent::gold);
ox::ComponentRegistry::instance().add<HealthComponent>();
ox::ComponentRegistry::instance().add<InventoryComponent>();

// В уровне:
player.add<ox::SaveGameComponent>();          // игрок сохраняется целиком (инвентарь, позиция, ...)
door.add<HealthComponent>().health = 50.0f;   // у двери сохранится только health
```

Сущностям, которые спавнятся во время игры (подобранные предметы, враги из спавнера), тоже нужен `SaveGameComponent`, иначе они не попадут в сохранение.

## Шаг 2. Свои данные: `ISaveable`

```cpp
class QuestLog final : public ox::ISaveable {
public:
    std::string saveId() const override { return "quests"; }   // ключ секции в файле
    void save(ox::serial::Writer& w) const override {
        w.value("active", active);
        w.value("completed", completed);
    }
    bool load(ox::serial::Reader& r) override {
        active.clear();
        completed = 0;
        r.value("active", active);     // отсутствующие ключи просто пропускаются
        r.value("completed", completed);
        return true;
    }
    void onMissing() override { *this = {}; } // в файле нет этой секции (старое сохранение)

    std::vector<std::string> active;
    ox::i32 completed = 0;
};

QuestLog quests;
engine.saves().registerSaveable(quests);   // не забудьте unregisterSaveable при уничтожении
```

`Writer` и `Reader` ([глава 02](02-reflection-serialization.md)) умеют писать любые отражённые типы, вложенные объекты (`beginObject`/`endObject`), массивы и бинарные блоки (`blob`). `save()` вызывается на игровом потоке в момент снимка, а `load()` — на игровом потоке при применении сохранения.

## Шаг 3. Сохранение и загрузка

```cpp
ox::SaveGameConfig config;
config.directory = tmp / "saves"; // в Engine не нужно: user://saves
config.version = 1;               // ProjectSettings::saveVersion
config.gameVersion = "1.0.0";
ox::SaveGameSystem saves(config);
saves.registerSaveable(quests);
saves.setCurrentLevel("project://levels/forest.oxscene");
saves.trackLevelEntities(*world); // сразу после загрузки уровня: чтобы помнить, какие сущности были в уровне

// ... игра ...

auto saved = saves.save("slot1", *world, ox::SaveKind::Manual, "У старого моста");
if (!saved) { /* saved.error().message */ }

// После перезапуска: уровень загружен заново, накладываем сохранение.
auto loaded = saves.load("slot1", *fresh);
loaded->entitiesCreated;    // заспавненные в игре сущности
loaded->entitiesDestroyed;  // уничтоженные в игре сущности уровня
loaded->fromBackup;         // основной файл был повреждён, использована .bak
```

В `Engine` всё это уже настроено: текущий уровень, отслеживание сущностей, мир, загрузчик уровня. Достаточно вызвать:

```cpp
engine.saveGame("slot1", "Перед боссом");   // = engine.saves().save(slot, engine.world(), Manual, name)
engine.loadGame("slot1");                   // перезагружает уровень из сохранения, затем накладывает данные
engine.saves().quickSave();                 // слот "quicksave"
engine.saves().quickLoad();
engine.console().execute("save slot2");     // консольные команды save / load
```

Полный пример: `samples/guide_examples/07-savegames/savegames.cpp` и `engine_saves.cpp`.

## Шаг 4. Меню слотов

```cpp
saves.setThumbnailProvider([] { return capturePngOfLastFrame(); }); // std::vector<std::byte>
saves.setPlayTime(3600.0); // Engine считает игровое время сам (без меню и паузы)

for (const ox::SaveSlotInfo& slot : saves.listSlots()) {   // от новых к старым
    slot.header.displayName;      // "Глава 1"
    slot.header.timestamp;        // unix-время (UTC), точность — 1 с
    slot.header.playTimeSeconds;
    slot.header.level;            // URI уровня
    slot.header.kind;             // Manual / Auto / Quick
    slot.hasThumbnail;            // картинка: saves.thumbnail(slot.header.slot)
    slot.corrupted;               // основной файл не читается
    slot.hasBackup;
}
saves.exists("slot1");
saves.deleteSlot("slot1");
ox::SaveGameSystem::isValidSlotName("../hack"); // false: имя слота — только безопасные символы
```

Сигналы `saved`, `loaded` и `failed(message)` срабатывают на игровом потоке. Ими удобно показывать значок «Сохранено» или ошибку.

## Шаг 5. Миграции

Когда меняется формат данных, увеличьте `saveVersion` в `.oxproj` и зарегистрируйте миграцию со старой версии. Миграция правит дерево значений (`serial::Value`) до того, как данные попадут в компоненты и `ISaveable`. Миграции применяются цепочкой: 1→2→3…

```cpp
// Версия 2: в секции квестов поле "done" переименовано в "completed".
saves.registerMigration(1, [](ox::serial::Document& doc) -> ox::Status {
    ox::serial::Value* q = doc.root.find("sections")->find("quests");
    if (!q) return {}; // секции может не быть
    if (const ox::serial::Value* done = q->find("done")) {
        q->set("completed", ox::serial::Value::makeInt(done->getInt(), ox::serial::Tag::I32));
        q->erase("done");
    }
    return {};
});
auto r = saves.load("old", world);   // r->migratedFrom == 1
```

Структура документа: `root` → `header`, `thumbnail`, `world` {`level`, `entities`: [{`id`, `name`, `parent`, `full`, `components`: {"Game.Health": {...}}}], `destroyed`: [uuid]}, `sections` {"quests": {...}}. Посмотреть реальный файл проще всего в JSON (шаг 7). Если миграция вернула ошибку (`ox::makeError("...")`), загрузка прерывается с этой ошибкой.

Для простого переименования поля компонента миграция не нужна: достаточно атрибута `attr::FormerName{"oldName"}` на поле ([глава 02](02-reflection-serialization.md)).

## Шаг 6. Асинхронно и автосейв

```cpp
// Снимок мира берётся сразу на игровом потоке, кодирование и запись идут в job system.
ox::SaveHandle handle = saves.saveAsync("slot1", world);
// мир можно менять сразу после вызова
handle->done();      // не блокирует
handle->result();    // блокирует до завершения: Result<SaveResult>

// Асинхронная загрузка: чтение, распаковка и миграции в фоне, применение — в update() на игровом потоке.
ox::LoadHandle load = saves.loadAsync("slot1");

// Автосейв каждые 5 минут игрового времени, по кругу autosave0..autosave2 (maxAutosaves = 3).
saves.setAutosaveInterval(300.0);
saves.update(realDt, /*playing*/ true);  // Engine вызывает это каждый кадр; в меню и на паузе playing = false
saves.autosave();                        // принудительно
saves.waitIdle();                        // дождаться всех фоновых операций (Engine делает это при shutdown)
```

- Для фоновых операций `SaveGameSystem` нужны `JobSystem` и провайдер мира (`setWorldProvider`). В `Engine` оба есть.
- Автосейв делается и при асинхронной смене уровня (`autosaveOnLevelChange`, по умолчанию включено). Ротация продолжается и между сессиями: следующим перезаписывается самый старый файл.
- Несколько одновременных записей в один слот выполняются по очереди, и файл остаётся целым.

**Надёжность записи.** Файл пишется во временный, затем fsync, старая версия переименовывается в `.bak`, новая — на место основного файла. Если сбой случился на любом этапе, изменения откатываются. При чтении проверяется CRC, и при повреждении автоматически берётся `.bak` (`LoadResult::fromBackup`).

## Шаг 7. Отладка: дамп в JSON и `oxdump`

Сохранения — это бинарные архивы OXB1. Посмотреть или поправить сохранение можно в JSON, причём преобразование туда и обратно не теряет ни байта:

```cpp
auto json = saves.exportJson("debug", dir / "debug.oxsave.json"); // строка + файл
auto binary = ox::serial::jsonToBinary(*json);                     // байт в байт равно исходному .oxsave
```

Из командной строки (`tools/oxdump`):

```sh
oxdump user/saves/slot1.oxsave -o slot1.json      # бинарный архив -> читаемый JSON
oxdump --to-binary slot1.json slot1.oxsave         # обратно (например, после ручной правки)
oxdump --info user/saves/slot1.oxsave              # заголовок, чанки, CRC, таблица схем
oxdump --check user/saves/slot1.oxsave             # CRC + проверка binary -> JSON -> binary
```

Так же работает с `.oxscene` и `.oxprefab`. JSON-сохранение можно и загрузить напрямую: `saves.apply(*ox::serial::parseJsonString(text), world)`.

## Типичные ошибки и подводные камни

- **Поле не сохраняется.** У сущности уровня сохраняются только поля с `attr::SaveGame`. Либо пометьте поле, либо добавьте сущности `SaveGameComponent`. Компонент также должен быть зарегистрирован в `ComponentRegistry`.
- **Заспавненный объект пропал после загрузки.** Сущности, созданные в игре, сохраняются, только если у них есть `SaveGameComponent`.
- **Уничтоженный объект уровня вернулся.** Сохранение не знает, что объект был в уровне, если после загрузки уровня не вызвали `trackLevelEntities(world)`. `Engine` делает это сам. При ручной работе с `SaveGameSystem` вызывайте его сами.
- **`load(slot, world)` против `load(slot)`.** Первая форма накладывает сохранение на переданный мир как есть. Вторая сначала загружает уровень из заголовка сохранения через `LevelLoader`. В игре нужна вторая (её и вызывает `engine.loadGame`).
- **Хэндлы `Entity` после `loadGame`** указывают на старый мир: уровень загружается заново. Храните `EntityRef` или перезапрашивайте сущности по сигналу `levelLoaded`.
- **Переименовали компонент** (имя в `OX_REFLECT_TYPE`). Старые сохранения положат его данные в `UnknownComponents`. Переименование компонента требует миграции.
- **`ISaveable` уничтожен, но не снят с регистрации.** При следующем сохранении будет обращение к удалённому объекту. Вызывайте `unregisterSaveable` в деструкторе или `shutdown` модуля.
- **Сигналы `saved`/`loaded` после `saveAsync`/`loadAsync`** приходят только из `update()`. Вне `Engine` его нужно вызывать самому.
- **Время в заголовке** имеет точность 1 секунду. Два сохранения в одну секунду в `listSlots()` могут идти в любом порядке.
- **`listSlots()` читает каждый файл целиком.** Для сотен слотов это заметно, и это стоит учитывать в меню.

## API

| Заголовок | Что внутри |
| --- | --- |
| [`save_game.hpp`](../../engine/runtime/include/oxwald/runtime/save_game.hpp) | `SaveGameSystem`, `SaveGameConfig`, `ISaveable`, `SaveGameHeader`, `SaveSlotInfo`, `SaveResult`, `LoadResult`, `SaveHandle`/`LoadHandle`, `SaveKind` |
| [`engine.hpp`](../../engine/runtime/include/oxwald/runtime/engine.hpp) | `Engine::saves()`, `saveGame()`, `loadGame()` |
| [`components.hpp`](../../engine/scene/include/oxwald/scene/components.hpp) | `SaveGameComponent` |
| [`reflect.hpp`](../../engine/core/include/oxwald/core/reflect.hpp) | `attr::SaveGame`, `attr::NoSerialize`, `attr::FormerName` |
| [`archive.hpp`](../../engine/core/include/oxwald/core/serial/archive.hpp) | `serial::Writer`, `serial::Reader` |
| [`format.hpp`](../../engine/core/include/oxwald/core/serial/format.hpp) | `jsonToBinary`, `binaryToJson`, `parseJsonString`, `loadDocument` |
| [`tools/oxdump/main.cpp`](../../tools/oxdump/main.cpp) | Утилита `oxdump` |

## Что дальше

- [02. Рефлексия и сериализация](02-reflection-serialization.md): атрибуты полей, `Writer`/`Reader`, формат OXB1.
- [03. ECS и сцены](03-ecs-scene.md): UUID, `EntityRef` и сериализация сцен, на которых построены сохранения.
- [05. Runtime и игровой цикл](05-runtime.md): смена уровней и автосейв при переходе.
- [06. Ввод](06-input.md): действие `QuickSave` по Ctrl+S.
- [Оглавление](README.md).
