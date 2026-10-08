# 02. Рефлексия и сериализация

## Зачем

C++ не умеет сам перечислять поля структуры. Движку это нужно постоянно: инспектор редактора показывает поля
компонента, undo/redo запоминает изменения, сцены и сохранения пишутся на диск, сеть реплицирует поля, Lua читает
и пишет компоненты. Всё это работает от **одного** описания типа — регистрации в рефлексии (reflection). Опишите
тип один раз — и он сразу сохраняется, показывается в редакторе и переживает изменения формата.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `TypeRegistry` | процесс-глобальный реестр метаданных типов (`ox::reflect`) |
| `TypeInfo` / `FieldInfo` | описание типа (имя, вид, поля, значения enum'а) и поля (имя, тип, атрибуты, доступ) |
| Атрибуты (`ox::attr::`) | подсказки для редактора, сохранений, сети: `Range`, `Tooltip`, `SaveGame`, … |
| `ValueRef` | типостёртая ссылка на значение + доступ по пути `"stats.damage"` |
| `serial::Value` | типизированное самоописывающее дерево значений |
| `serial::Document` | архив: `kind` (вид содержимого), `version` (версия ваших данных), `root` |
| OXB1 | основной двоичный формат: чанки с CRC32, таблица схемы, поля с размерами |
| JSON | человекочитаемое представление того же дерева; конвертация без потерь в обе стороны |
| `Writer` / `Reader` | высокоуровневый API записи/чтения архивов |
| `oxdump` | утилита командной строки для просмотра и конвертации архивов |

## Регистрация типов

Регистрируйте типы **явной функцией** `registerXxxTypes()`, которую вызывает инициализация вашего модуля (например,
`IEngineModule::registerTypes()`). Статические инициализаторы в статических библиотеках выкидываются линкером,
поэтому «магическая» саморегистрация ненадёжна. Повторный вызов регистрации безопасен (идемпотентен).

```cpp
#include <oxwald/core/reflect.hpp>

namespace game {
enum class WeaponKind : ox::u8 { Sword, Bow, Staff };

struct WeaponStats {
    float damage = 10.0f;
    float range = 1.5f;
};

struct Weapon {
    std::string name = "Rusty sword";
    WeaponKind kind = WeaponKind::Sword;
    WeaponStats stats;
    glm::vec3 tint{1.0f};
    std::vector<std::string> tags;
    std::optional<ox::Uuid> icon;   // ссылка на ассет-текстуру (может отсутствовать)
    int durability = 100;
    float cachedDps = 0.0f;         // вычисляется в рантайме, не сохраняется
};

void registerGameTypes() {
    OX_REFLECT_ENUM(WeaponKind, "game.WeaponKind")
        .value("Sword", WeaponKind::Sword)
        .value("Bow", WeaponKind::Bow)
        .value("Staff", WeaponKind::Staff, ox::attr::DisplayName{"Magic staff"});

    OX_REFLECT_TYPE(WeaponStats, "game.WeaponStats")
        .field("damage", &WeaponStats::damage, ox::attr::Range{0.0, 1000.0}, ox::attr::Step{0.5})
        .field("range", &WeaponStats::range, ox::attr::Range{0.0, 100.0}, ox::attr::Tooltip{"Metres"});

    OX_REFLECT_TYPE(Weapon, "game.Weapon")
        .attributes(ox::attr::Category{"Gameplay"}, ox::attr::Meta{"icon", "sword"})
        .field("name", &Weapon::name, ox::attr::DisplayName{"Display name"})
        .field("kind", &Weapon::kind)
        .field("stats", &Weapon::stats)                  // вложенная отражённая структура
        .field("tint", &Weapon::tint, ox::attr::Color{})
        .field("tags", &Weapon::tags)
        .field("icon", &Weapon::icon, ox::attr::AssetRef{"Texture"})
        .field("durability", &Weapon::durability, ox::attr::SaveGame{}, ox::attr::Replicated{})
        .field("cachedDps", &Weapon::cachedDps, ox::attr::NoSerialize{}, ox::attr::ReadOnly{});
}
} // namespace game
```

Полный пример: `samples/guide_examples/02-reflection-serialization/reflect_types.cpp`.

Имя типа (`"game.Weapon"`) попадает в файлы — выберите его один раз и не меняйте; используйте префикс
пространства имён, чтобы не столкнуться с типами движка. Наследование: `.base<Base>()` (базовый класс должен быть
зарегистрирован раньше; его поля идут первыми).

### Поддерживаемые типы полей

| Тип | Примечание |
| --- | --- |
| `bool`, все целые, `float`, `double`, `std::string` | |
| `glm::vec2/3/4`, `ivec2/3/4`, `quat`, `mat4` | |
| `ox::Uuid` | с `attr::AssetRef` — ссылка на ассет |
| `enum` / `enum class` | зарегистрированные пишутся по имени |
| отражённые структуры | вложенные, с базовыми классами |
| `std::vector<T>` | массивы чисел/векторов хранятся сплошным блоком; `std::vector<bool>` **не поддерживается** — берите `std::vector<u8>` |
| `std::optional<T>` | |
| `std::map<std::string, T>`, `std::unordered_map<std::string, T>` | ключи — только строки |
| пользовательские «листья» | `reflect::registerCustomLeaf<T>(...)` (так сделан `EntityRef` из scene) |

`std::array` и указатели не отражаются.

### Атрибуты

| Атрибут | Кто использует | Смысл |
| --- | --- | --- |
| `DisplayName{"..."}` | редактор | подпись поля / значения enum'а |
| `Tooltip{"..."}` | редактор | всплывающая подсказка |
| `Range{min, max}`, `Step{v}` | редактор | ограничения и шаг слайдера |
| `Color{hdr}` | редактор | `vec3/vec4` как цвет (HDR — без ограничения 1.0) |
| `AssetRef{"Texture"}` | редактор, ассеты | поле `Uuid` — ссылка на ассет этого типа |
| `Category{"..."}` | редактор | категория типа/компонента в меню |
| `Hidden`, `ReadOnly` | редактор | скрыть / запретить правку |
| `SaveGame` | сохранения | поле попадает в сохранение игры (см. [07](07-savegames.md)) |
| `Replicated` | сеть | поле реплицируется (см. [14](14-networking.md)) |
| `NoSerialize` | сериализация | поле никогда не пишется на диск |
| `FormerName{"old"}` | чтение | старое имя поля — для переименований |
| `Meta{key, value}` | любые модули | произвольные подсказки (`{"icon", "heart"}`) |

### Чтение метаданных и обобщённый доступ

```cpp
const ox::reflect::TypeInfo& type = ox::reflect::typeOf<game::Weapon>();
for (const ox::reflect::FieldInfo& f : type.fields) {
    if (f.attributes.saveGame) { /* f.name, f.type->name, f.displayName(), f.get(&weapon) */ }
}
type.findField("stats");                                            // FieldInfo* или nullptr
ox::reflect::TypeRegistry::instance().find("game.Weapon");          // по имени
ox::reflect::typeOf<game::WeaponKind>().findEnum("Bow")->value;     // 1

// Доступ по пути — как в инспекторе и undo/redo:
auto root = ox::reflect::ValueRef::of(bow);
ox::reflect::resolvePath(root, "stats.range").setAs(25.0);          // double -> float: конверсия терпимая
ox::reflect::resolvePath(root, "tint.g").setAs(0.5f);               // компоненты x/y/z/w или r/g/b/a
ox::reflect::resolvePath(root, "tags[1]").getAs<std::string>();     // индексы: [1] или .1; ключи map; .value у optional
ox::reflect::resolvePath(root, "kind").get().getString();           // "Bow"
```

Полный пример: `samples/guide_examples/02-reflection-serialization/reflect_types.cpp`.

## Свой компонент ECS

Компонент — обычная структура, отражённая и зарегистрированная в `ComponentRegistry` (модуль `scene`). После этого
он появляется в меню «Add component» редактора, сохраняется в сценах и префабах, копируется при клонировании мира:

```cpp
#include <oxwald/scene/component_registry.hpp>

struct HealthComponent {
    float max = 100.0f;
    float current = 100.0f;
    bool invulnerable = false;
    float regenPerSecond = 0.0f;
};

void registerGameComponents() {
    OX_REFLECT_TYPE(HealthComponent, "Health")
        .attributes(ox::attr::Category{"Gameplay"}, ox::attr::Meta{"icon", "heart"})
        .field("max", &HealthComponent::max, ox::attr::Range{1.0, 10000.0})
        .field("current", &HealthComponent::current, ox::attr::SaveGame{}, ox::attr::Replicated{})
        .field("invulnerable", &HealthComponent::invulnerable)
        .field("regenPerSecond", &HealthComponent::regenPerSecond, ox::attr::DisplayName{"Regen / s"});
    ox::ComponentRegistry::instance().add<HealthComponent>();   // имя = отражённое имя ("Health")
}

ox::World world;
world.create("Boss").add<HealthComponent>(5000.0f, 4200.0f, false, 5.0f);
ox::saveScene(world, "boss.oxscene");          // OXB1; "boss.oxscene.json" — JSON
ox::World loaded;
ox::loadScene(loaded, "boss.oxscene");
loaded.findByName("Boss").get<HealthComponent>().current;    // 4200
```

Полный пример: `samples/guide_examples/02-reflection-serialization/component_scene.cpp`.

`ComponentRegistry::add` принимает `ComponentOptions{name, category, icon, removable, hiddenInInspector,
serializable}`. Сначала регистрируйте рефлексию, потом компонент (иначе assert). Пустые структуры-«теги» без полей
регистрировать нельзя. Подробнее о мире, сущностях и системах — в [03-ecs-scene](03-ecs-scene.md).

## Сериализация: Writer и Reader

Один API пишет два взаимозаменяемых формата. Формат выбирается по расширению: `.json` в конце — JSON, иначе —
двоичный OXB1 (`.oxscene`, `.oxprefab`, `.oxsave`, …). При чтении формат определяется по содержимому.

```cpp
#include <oxwald/core/serial/archive.hpp>

ox::serial::Writer w("profile", /*version*/ 1);       // kind — вид содержимого, version — версия ВАШИХ данных
w.value("player", player);                            // любой отражённый тип
w.value("playTimeSeconds", 3600.0);                   // или встроенный
w.beginObject("quests");                              // ручная структура
w.value("active", std::vector<std::string>{"find_sword", "save_village"});
w.value("completed", 12);
w.endObject();
w.blob("thumbnail", pngBytes);                        // произвольные байты
w.save(dir / "profile.oxsave");                       // OXB1, запись атомарная (временный файл + rename)
w.save(dir / "profile.oxsave.json");                  // JSON

auto r = ox::serial::Reader::load(dir / "profile.oxsave");   // Result<Reader>
if (!r) { /* r.error().message */ }
r->kind();  r->version();
r->value("player", loaded);                           // false и значение не тронуто, если ключа нет
if (r->beginObject("quests")) {
    r->value("active", active);
    r->endObject();
}
if (auto n = r->beginArray("log")) {                  // число элементов
    for (size_t i = 0; i < *n; ++i) { std::string s; r->value(i, s); }
    r->endArray();
}
```

Полный пример: `samples/guide_examples/02-reflection-serialization/serialize.cpp`.

Также: `w.toBinary()` / `w.toJson()` (в память), `Reader::fromBytes(bytes)`, `beginArray`/`element`,
`beginMap`. Для частичной записи — `serial::ConvertOptions::fieldFilter` (например, только поля с `SaveGame`);
поля с `NoSerialize` пропускаются всегда. Низкоуровнево: `serial::toValue(obj)` / `serial::fromValue(value, obj)`.

### Форматы

**OXB1** (двоичный, little-endian): заголовок `"OXB1"` + версия формата + версия данных, далее чанки
`META` (kind), `STRS` (строки), `TYPE` (таблица схемы: имена типов и полей с хэшами и дескрипторами) и `DATA`;
у каждого чанка CRC32 — повреждённый файл обнаруживается при чтении. Каждое поле записано с размером, поэтому
незнакомые поля можно пропустить. Enum'ы хранятся хэшем имени (переупорядочивание значений не ломает файлы),
массивы чисел/векторов — одним сырым блоком.

**JSON** — то же дерево: `{"format": "oxb1-json", "kind", "version", "schema", "rootType", "data"}`, у объектов есть
`"$type"`, enum'ы — строками. Благодаря схеме JSON → OXB1 даёт **байт-в-байт** исходный файл. Это удобно для
диффов в системе контроля версий и ручной правки.

```cpp
auto json = ox::serial::binaryToJson(binary);   // не нужно знать C++-типы
auto back = ox::serial::jsonToBinary(*json);    // *back == binary
auto info = ox::serial::inspectBinary(binary);  // заголовок, чанки (crcValid), схема
```

JSON можно писать и руками, без схемы и `$type` — типы выводятся, а преобразование выполняется при чтении в
C++-тип (числа конвертируются, enum'ы принимаются по имени, векторы — массивами):

```cpp
auto doc = ox::serial::parseJsonString(R"({
    "player": { "name": "Bot", "level": 5, "position": [1, 2, 3], "difficulty": "Easy" }
})");
ox::serial::Reader(*doc).value("player", p);
```

Голый объект без конверта `{"format": "oxb1-json", ...}` становится корнем документа с пустыми `kind` и `version`;
чтобы задать их, используйте конверт (см. `sword.json` в примерах).

## Версионирование: как менять формат, не ломая старые файлы

Чтение терпимо к изменениям структуры:

| Изменение | Что происходит со старым файлом |
| --- | --- |
| Добавили поле | его нет в файле — остаётся значение по умолчанию из C++ |
| Удалили поле | запись в файле пропускается |
| Переименовали поле | добавьте `attr::FormerName{"старое"}` — старые файлы прочитаются |
| Изменили числовой тип (`int` → `double`) | значение конвертируется |
| Добавили значение enum'а | старые значения не сдвигаются (хранятся по имени) |
| Изменился **смысл** данных (единицы, структура) | нужна миграция по версии документа |

И наоборот: старая сборка читает новый файл, пропуская незнакомые поля (полезно для отката и
совместимости клиент/сервер).

```cpp
// Было (v1)                                   // Стало (v2)
struct Character {                             struct Character {
    std::string playerName = "anon";               std::string displayName;    // переименовано
    int level = 1;                                 double level = 1.0;         // тип расширен
    float health = 1.0f;   // доля 0..1            float health = 100.0f;      // теперь 0..100
    int legacyKarma = 0;                           bool hardcore = false;      // новое поле
};                                             };

OX_REFLECT_TYPE(Character, "guide.Character")
    .field("displayName", &Character::displayName, ox::attr::FormerName{"playerName"})
    .field("level", &Character::level)
    .field("health", &Character::health)
    .field("hardcore", &Character::hardcore);
```

Смену смысла (доля → абсолютные единицы) поля не выразить — для этого служит `Document::version`. Пишите
документ с текущей версией и мигрируйте дерево значений перед чтением:

```cpp
constexpr ox::u32 kCharacterVersion = 2;

ox::Status migrate(ox::serial::Document& doc) {
    if (doc.version < 2) {
        ox::serial::Value* character = doc.root.find("character");
        if (!character) return ox::makeError("no 'character' in {} v{}", doc.kind, doc.version);
        if (ox::serial::Value* hp = character->find("health"))
            *hp = ox::serial::Value::makeF32(static_cast<float>(hp->getDouble() * 100.0));
        doc.version = 2;
    }
    return {};
}

ox::Result<Character> loadCharacter(std::span<const std::byte> bytes) {
    auto doc = ox::serial::decodeAny(bytes);                  // OXB1 или JSON
    if (!doc) return doc.error();
    if (doc->version > kCharacterVersion) return ox::makeError("file is from a newer game version");
    if (auto st = migrate(*doc); !st) return st.error();
    Character c;
    ox::serial::Reader(std::move(*doc)).value("character", c);
    return c;
}
```

Полный пример: `samples/guide_examples/02-reflection-serialization/versioning.cpp`.

Для сохранений игры то же самое встроено в `SaveGameSystem::registerMigration(fromVersion, fn)` — см.
[07-savegames](07-savegames.md).

## oxdump: архивы из командной строки

`oxdump` (`tools/oxdump`, собирается при включённом каталоге `tools`) читает и конвертирует любые OXB1-архивы, не
зная C++-типов:

```sh
oxdump level.oxscene                         # -> красивый JSON в stdout
oxdump save.oxsave -o save.json --indent 4   # в файл
oxdump --to-binary save.json save.oxsave     # JSON -> OXB1 (байт-в-байт, если JSON получен из oxdump)
oxdump --info level.oxscene                  # заголовок, чанки с CRC, таблица схемы (типы и поля)
oxdump --check level.oxscene                 # CRC + проверка binary -> JSON -> binary
```

`--info` для `sword.json` из примеров, переведённого в OXB1:

```
format         OXB1 v1 (flags 0x0000)
kind           item
data version   1
chunks         4
  META  offset       28  size        5  crc 0dfdabdf  crc ok
  STRS  offset       45  size       51  crc a40a60c0  crc ok
  TYPE  offset      108  size       41  crc de79c6b5  crc ok
  DATA  offset      161  size       60  crc 18fb0bca  crc ok
strings        8
types          1
  [0] <anonymous>  (hash cbf29ce484222325, 3 fields)
        name                         string
        damage                       f64
        tags                         array<string>
```

Типичный сценарий: сохранение игрока не грузится — `oxdump --check` покажет, повреждён ли файл; `oxdump save.oxsave`
— что внутри; правка руками в JSON и `--to-binary` — быстрый «чит» для тестирования. Код возврата: 0 — успех,
1 — ошибка (в том числе несовпадение CRC), 2 — неверные аргументы.

Пример (ctest): `samples/guide_examples/02-reflection-serialization/CMakeLists.txt` гоняет `oxdump` по
`sword.json`.

## Типичные ошибки и подводные камни

- **Тип не зарегистрирован.** Если забыть вызвать `registerXxxTypes()`, структура сериализуется как пустой объект,
  а `ComponentRegistry::add` упадёт на assert. Вызывайте регистрацию из `IEngineModule::registerTypes()`.
- **Регистрация в статическом инициализаторе** библиотеки — линкер может выкинуть объектный файл, и регистрация не
  выполнится. Только явные функции.
- **Переименовали тип или поле без `FormerName`.** Старые файлы молча потеряют значения (поле получит значение по
  умолчанию). Имена типов в файлах (`"game.Weapon"`) не меняйте вовсе.
- **`std::vector<bool>`, `std::array`, ключи map не-строки** — не отражаются; ошибка компиляции или
  `static_assert`.
- **Важное поле с `NoSerialize`** — не будет записано никогда, даже в JSON. Для полей «только не в сохранения»
  используйте фильтр по `SaveGame`.
- **`Reader::value` возвращает `false`** при отсутствии ключа и оставляет значение нетронутым — инициализируйте
  переменные разумными значениями по умолчанию.
- **Пересчёт смысла без версии.** Изменили единицы измерения — увеличьте `version` в `Writer` и добавьте миграцию;
  отвергайте файлы с версией новее текущей.
- **NaN/бесконечности в JSON** пишутся строками `"nan"`, `"inf"` (без битов полезной нагрузки NaN); ключ `"$type"`
  в объектах зарезервирован.
- **Разнородные массивы в рукописном JSON** (`[1, "a"]`) отклоняются: массивы однородны.

## API

- [`reflect.hpp`](../../engine/core/include/oxwald/core/reflect.hpp) — `OX_REFLECT_TYPE`, `OX_REFLECT_ENUM`, `ox::attr::*`, `TypeInfo`, `ValueRef`, `resolvePath`
- [`serial/archive.hpp`](../../engine/core/include/oxwald/core/serial/archive.hpp) — `Writer`, `Reader`
- [`serial/format.hpp`](../../engine/core/include/oxwald/core/serial/format.hpp) — `encodeBinary`, `decodeAny`, `binaryToJson`, `jsonToBinary`, `inspectBinary`, `loadDocument`/`saveDocument`
- [`serial/value.hpp`](../../engine/core/include/oxwald/core/serial/value.hpp) — `Value`, `Document`, `Tag`
- [`serial/convert.hpp`](../../engine/core/include/oxwald/core/serial/convert.hpp) — `toValue`, `fromValue`, `ConvertOptions`
- [`component_registry.hpp`](../../engine/scene/include/oxwald/scene/component_registry.hpp), [`scene_serializer.hpp`](../../engine/scene/include/oxwald/scene/scene_serializer.hpp)
- [`tools/oxdump/main.cpp`](../../tools/oxdump/main.cpp)

## Что дальше

- [01. Ядро (core)](01-core.md)
- [03. ECS и сцены](03-ecs-scene.md) — компоненты, сцены, префабы.
- [07. Сохранения](07-savegames.md) — `SaveGame`-поля, слоты, миграции.
- [14. Сеть](14-networking.md) — `Replicated`-поля.
- [15. Lua](15-scripting-lua.md) — доступ к отражённым компонентам из скриптов.
- [Оглавление](README.md)
