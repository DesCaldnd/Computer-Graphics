# 03. ECS и сцены

## Зачем

Всё, что есть в игровом мире (игрок, камера, источник света, дерево, триггер), в OxwaldEngine — **сущность** (entity) в **мире** (`World`). Сущность сама по себе почти пустая: её поведение и данные задаются **компонентами** (components), простыми структурами без логики. Логику пишут в **системах** (systems), которые каждый кадр обходят сущности с нужным набором компонентов.

Модуль `scene` (таргет `Oxwald::scene`) — тонкий слой над [EnTT](https://github.com/skypjack/EnTT). Он добавляет то, чего нет в «голом» ECS, но что нужно игре и редактору:

- иерархию «родитель → дети» и трансформы с кэшем мировых матриц;
- стабильные UUID и ссылки между сущностями (`EntityRef`), которые переживают сохранение, копирование и префабы;
- планировщик систем по фазам кадра с фиксированным шагом;
- сохранение и загрузку сцен (бинарный формат и JSON), копирование и вставку, префабы с overrides, клонирование мира для play-in-editor.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `World` | Контейнер сущностей поверх `entt::registry`, индекс по UUID, список корней иерархии |
| `Entity` | Лёгкий копируемый хэндл: `entt::entity` + указатель на мир. Пустой `Entity{}` невалиден |
| Компонент | Структура с данными. Чтобы его сохраняли и видел редактор, его отражают (`OX_REFLECT_TYPE`) и регистрируют в `ComponentRegistry` |
| `EntityRef` | Ссылка на сущность по UUID. Её кладут в поля компонентов вместо `entt::entity` |
| `ISystem` | Логика: `name()`, `phase()`, `order()`, `update(SystemContext&)` |
| `SystemScheduler` | Запускает системы по фазам `PreUpdate → FixedUpdate×N → Update → PostUpdate → Extract` |
| Сцена (`.oxscene`) | Сериализованный мир: сущности в порядке обхода иерархии, у каждой UUID, имя, родитель и компоненты |
| Префаб (`.oxprefab`) | Шаблон поддерева сущностей. Экземпляры помнят свой префаб и собственные изменения (overrides) |

Перед работой со сценами один раз вызовите `ox::registerSceneTypes()`. Функция идемпотентна, а `Engine` вызывает её сам.

## Шаг 1. Сущности и компоненты

```cpp
#include <oxwald/scene/scene.hpp>

ox::registerSceneTypes();
ox::World world;
ox::Entity lamp = world.create("Lamp"); // сразу есть Id, Name, Transform, Hierarchy, WorldTransform, Active

auto& light = lamp.add<ox::LightComponent>();
light.type = ox::LightType::Spot;
light.intensity = 1200.0f; // люмены

if (auto* mr = lamp.tryGet<ox::MeshRendererComponent>()) { /* компонента может не быть */ }
lamp.has<ox::LightComponent>();     // true
lamp.remove<ox::LightComponent>();

// Поиск: по UUID, по EntityRef, по имени (линейный, не для горячего кода).
world.find(lamp.uuid());
world.resolve(lamp.ref());
world.findByName("Lamp");

// Обход всех сущностей с набором компонентов: обычный EnTT view.
for (auto [e, l] : world.view<ox::LightComponent>().each()) {
    if (l.type == ox::LightType::Spot) { /* ... */ }
}
```

`add<C>()` проверяет (assert), что компонента ещё нет. Если не уверены, используйте `addOrReplace<C>()`. Если компонент нужно изменить так, чтобы подписчики EnTT (например, пересчёт трансформов) узнали об этом, используйте `patch<C>(fn)`.

Полный пример: `samples/guide_examples/03-ecs-scene/world.cpp`.

### Встроенные компоненты

| Компонент (имя в файле) | Поля | Назначение |
| --- | --- | --- |
| `NameComponent` (`Name`) | `name` | Имя для редактора и `findByName` |
| `IdComponent` (`Id`) | `id` (UUID) | Стабильный идентификатор |
| `TransformComponent` (`Transform`) | `position`, `rotation` (quat), `scale` | Локальный трансформ относительно родителя |
| `HierarchyComponent` (`Hierarchy`) | `parent`, `children` | Только в рантайме: в файл пишется UUID родителя |
| `WorldTransformComponent` | `matrix`, `previous` | Кэш мировой матрицы и матрица прошлого кадра (motion vectors, интерполяция) |
| `ActiveComponent` (`Active`) | `active` | Флаг включённости, см. `activeInHierarchy()` |
| `CameraComponent` (`Camera`) | `projection`, `verticalFov` (градусы), `orthographicSize`, `nearPlane`, `farPlane` (≤ 0 — бесконечность), `aperture`, `shutterSpeed`, `iso`, `exposureCompensation`, `primary` | Камера с физической экспозицией: `projectionMatrix(aspect)` (reversed-Z), `ev100()`, `exposure()` |
| `LightComponent` (`Light`) | `type` (Directional/Point/Spot/AreaRect), `color`, `intensity` (люксы для направленного, люмены для остальных), `range`, `innerConeAngle`/`outerConeAngle` (градусы), `castShadows`, `shadowResolution`, `shadowBias`, `shadowNormalBias`, `sourceRadius`, `volumetric`, `volumetricIntensity` | Источник света |
| `MeshRendererComponent` (`MeshRenderer`) | `mesh`, `materials` (UUID ассетов), `castShadows`, `receiveShadows`, `visible`, `layerMask` | Отрисовка меша |
| `EnvironmentComponent` (`Environment`) | `skybox`, `skyIntensity`, `sun` (`EntityRef`), `ambientIntensity`, `fog*` | Небо, окружение, туман |
| `TagComponent` (`Tags`) | `tags` | Строковые теги, `has("enemy")` |
| `PrefabInstanceComponent` (`PrefabInstance`) | `prefab`, `sourceId`, `isRoot`, `overrides` | Связь с префабом (см. шаг 6) |
| `SaveGameComponent` (`SaveGame`) | `saveTransform` | Сохранять сущность целиком (см. [главу 07](07-savegames.md)) |
| `UnknownComponents` | `entries` | Данные компонентов из модулей, которых нет в этой сборке; пишутся обратно без потерь |

Готовые компоненты геймплея (физические тела, аниматоры, источники звука, ИИ-агенты, скрипты) описаны в отдельной главе *(скоро)*.

## Шаг 2. Удаление сущностей

`entity.destroy()` (или `world.destroy(e)`) только **помечает** сущность и всё её поддерево тегом `PendingDestroyTag`. Реально они удаляются в `world.flushDestroyed()`, а его в конце каждого кадра вызывает `SystemScheduler::tick`. Так системы могут спокойно уничтожать сущности прямо во время обхода view.

```cpp
ox::Entity car = world.create("Car");
ox::Entity wheel = world.create("Wheel", car);
car.destroy();               // помечены Car и Wheel
wheel.valid();               // всё ещё true
world.flushDestroyed();      // == 2, обычно это делает планировщик
```

Если сущность нужно удалить немедленно (инструменты, тесты), есть `world.destroyImmediate(e)`.

## Шаг 3. Иерархия и трансформы

Родителя задают при создании (`world.create("Wheel", car)`) или позже через `setParent`. Порядок детей явный (`children()`), его можно менять через параметр `index`.

```cpp
ox::Entity car = world.create("Car");
ox::Entity wheel = world.create("Wheel", car);
car.setPosition({10, 0, 0});
car.setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)));
wheel.setPosition({1, 0, 0}); // локально, относительно Car

world.updateTransforms();     // пересчитать кэш грязных поддеревьев
wheel.get<ox::WorldTransformComponent>().matrix; // мировая позиция (10, 0, -1)
wheel.worldPosition();                           // то же, но через иерархию, без кэша

// Сменить родителя, не сдвигая объект в мире (по умолчанию keepWorldTransform = true).
wheel.setParent(garage, /*keepWorldTransform*/ true);
wheel.setWorldPosition({0, 0, 0}); // мировые сеттеры сами пересчитают локальный трансформ

world.forEachInHierarchy([&](entt::entity e) { /* родители раньше детей */ });
```

Как устроен кэш:

- `TransformComponent` хранит локальный трансформ, а `WorldTransformComponent::matrix` хранит кэш мировой матрицы.
- Сеттеры `Entity` (`setPosition`, `setRotation`, `setScale`, `setLocalTransform`, `transform()`) и `patch<TransformComponent>` помечают сущность грязной (`TransformDirtyTag`).
- `world.updateTransforms()` пересчитывает только грязные поддеревья. В игровом цикле это делает `TransformSystem` (фаза `PostUpdate`, `order` −1000), поэтому системы в `PostUpdate` и `Extract` видят свежие матрицы.
- `snapshotPreviousTransforms()` в начале кадра копирует `matrix` в `previous`. Рендерер интерполирует между ними (см. [главу 05](05-runtime.md)).
- Методы `worldMatrix()`, `worldPosition()` и `worldRotation()` всегда считают через иерархию и от кэша не зависят. Ими можно пользоваться посреди кадра.

**Активность.** `setActive(false)` выключает сущность, а `activeInHierarchy()` возвращает false, если выключена сама сущность или любой её предок. Системы геймплея и рендера пропускают такие сущности.

Полный пример: `samples/guide_examples/03-ecs-scene/world.cpp`.

## Шаг 4. Свой компонент и система

Компонент — это обычная структура. Чтобы его сохраняли в сцену, показывали в инспекторе, копировали и клонировали, его нужно отразить (подробно в [главе 02](02-reflection-serialization.md)) и зарегистрировать:

```cpp
struct SpinComponent {
    ox::f32 degreesPerSecond = 90.0f;
};

void registerGameTypes() {
    ox::registerSceneTypes();
    OX_REFLECT_TYPE(SpinComponent, "Game.Spin")
        .attributes(ox::attr::Category{"Gameplay"})
        .field("degreesPerSecond", &SpinComponent::degreesPerSecond);
    ox::ComponentRegistry::instance().add<SpinComponent>({.icon = "refresh"});
}
```

Имя из `OX_REFLECT_TYPE` (`"Game.Spin"`) становится ключом компонента в файлах сцены. Не меняйте его после выхода игры. Опции `ComponentOptions`: `name`, `category`, `icon`, `removable`, `hiddenInInspector`, `serializable`.

Система — это класс-наследник `ISystem`:

```cpp
class SpinSystem final : public ox::ISystem {
public:
    std::string_view name() const override { return "Game.Spin"; }
    ox::SystemPhase phase() const override { return ox::SystemPhase::FixedUpdate; }
    void update(ox::SystemContext& ctx) override {
        for (auto [e, spin] : ctx.world.view<SpinComponent>().each()) {
            ox::Entity entity = ctx.world.wrap(e);
            const glm::quat step = glm::angleAxis(glm::radians(spin.degreesPerSecond * ctx.dt), glm::vec3(0, 1, 0));
            entity.setRotation(step * entity.localTransform().rotation); // помечает трансформ грязным
        }
    }
};

ox::SystemScheduler scheduler(1.0 / 60.0 /*fixed dt*/, 8 /*max fixed steps*/);
scheduler.emplace<ox::TransformSystem>();
scheduler.emplace<SpinSystem>();
scheduler.attach(world, services);                       // вызовет onAttach у систем
scheduler.tick(world, services, frameDt);                // один кадр
```

В игре планировщик создаёт `Engine`, а системы добавляют из `IEngineModule::registerSystems` (см. [главу 05](05-runtime.md)). Отдельный `SystemScheduler` нужен в тестах и инструментах.

Полный пример: `samples/guide_examples/03-ecs-scene/systems.cpp`.

### Фазы кадра

| Фаза | Сколько раз | Для чего |
| --- | --- | --- |
| `PreUpdate` | 1 за кадр | Сеть, скрипты, корутины, подготовка ввода |
| `FixedUpdate` | 0..N за кадр, шаг `fixedDt` | Физика и детерминированная игровая логика. `ctx.dt == ctx.fixedDt` |
| `Update` | 1 за кадр | Обычная логика, камера, UI. `ctx.dt` — переменная дельта, `ctx.alpha` — доля до следующего фиксированного шага |
| `PostUpdate` | 1 за кадр | `TransformSystem` (order −1000), звук, всё, что читает мировые матрицы |
| `Extract` | 1 за кадр | Подготовка данных для рендера |

После `Extract` планировщик вызывает `world.flushDestroyed()`.

`SystemContext` содержит `world`, `services` (DI-контейнер, [глава 01](01-core.md)), `dt`, `fixedDt`, `alpha`, `frame`, `fixedStep`, `phase` и `playing`.

Внутри фазы системы сортируются по `order()` (меньше — раньше), а при равных `order` идут в порядке регистрации. `playModeOnly() == true` означает, что система не запускается в режиме редактирования (Edit mode) редактора. Подходит для ИИ, скриптов, спавнеров. `scheduler.setEnabled("Game.Spin", false)` временно выключает систему, `lastTimeMs(name)` показывает её время за прошлый кадр.

**Фиксированный шаг.** Если кадр длился 35 мс при `fixedDt` = 10 мс, `FixedUpdate` выполнится 3 раза, а 5 мс останутся в аккумуляторе (`alpha` = 0.5). Если кадр затянулся (брейкпоинт, загрузка), шагов будет не больше `maxFixedSteps`, а лишнее время отбрасывается. Так игра не уходит в «спираль смерти».

## Шаг 5. Сохранение и загрузка сцен

Ссылки на другие сущности храните в полях типа `EntityRef`, а не `entt::entity`. `entt::entity` — номер слота в памяти, и после загрузки он будет другим.

```cpp
struct FollowComponent {
    ox::EntityRef target;
    ox::f32 speed = 2.0f;
    ox::f32 debugDistance = 0.0f; // рантайм-данные
};

OX_REFLECT_TYPE(FollowComponent, "Game.Follow")
    .field("target", &FollowComponent::target)
    .field("speed", &FollowComponent::speed)
    .field("debugDistance", &FollowComponent::debugDistance, ox::attr::NoSerialize{});
ox::ComponentRegistry::instance().add<FollowComponent>();

// Бинарный формат или JSON (по суффиксу .json).
ox::saveScene(world, dir / "level.oxscene");
ox::saveScene(world, dir / "level.oxscene.json");

ox::World loaded;                                        // загружаем в пустой мир
auto status = ox::loadScene(loaded, dir / "level.oxscene");
if (!status) { /* status.error().message */ }
ox::Entity camera = loaded.findByName("Camera");
loaded.resolve(camera.get<FollowComponent>().target);   // ссылка цела: UUID сохранены
```

Что нужно знать:

- Файл — это документ OXB1 с kind `"scene"`. В нём массив сущностей в порядке обхода иерархии: `{id, name, parent, components: {"Light": {...}, "Game.Follow": {...}}}`. Бинарный формат и JSON взаимно конвертируются без потерь утилитой `oxdump` ([глава 02](02-reflection-serialization.md)).
- Поля с `attr::NoSerialize` не пишутся. При загрузке у них остаётся значение по умолчанию.
- Если в файле есть компонент, тип которого не зарегистрирован в этой сборке (модуль не подключён), его данные лягут в `UnknownComponents` и при следующем сохранении запишутся обратно без изменений.
- `loadScene` добавляет сущности с теми же UUID, что в файле. Загрузка в мир, где эти UUID уже есть, завершится ошибкой.
- Для работы в памяти есть `serializeWorld(world, options)` → `serial::Document` и `deserializeWorld(world, doc)`. В `SceneSerializeOptions` можно задать `entityFilter` (false — пропустить сущность с поддеревом), `componentFilter` и фильтр полей `convert`. На этом построены сохранения игры.

Полный пример: `samples/guide_examples/03-ecs-scene/serialization.cpp`.

## Шаг 6. Копирование и вставка

```cpp
ox::Entity selection[] = {player, camera};
std::vector<std::byte> clipboard = ox::copyEntities(world, selection); // буфер OXB1

auto pasted = ox::pasteEntities(world, clipboard, group); // вставить детьми group (или в корень)
// pasted -> std::vector<Entity> корней вставки
```

- Если выделены и родитель, и ребёнок, поддерево копируется один раз.
- Вставленные сущности получают **новые** UUID.
- Ссылки (`EntityRef`) **внутри** скопированного набора перенаправляются на новые копии: камера из копии следит за копией игрока. Ссылки **наружу** не меняются: если скопировать одну камеру, она продолжит следить за исходным игроком.

## Шаг 7. Префабы и overrides

Префаб — это шаблон поддерева сущностей, документ с kind `"prefab"`. Экземпляры несут `PrefabInstanceComponent` и список собственных изменений (overrides) в виде путей к свойствам: `"Light.intensity"`, `"Tags"` (компонент добавлен на экземпляре), `"-Light"` (компонент удалён), `"Name"`.

```cpp
#include <oxwald/scene/prefab.hpp>

// 1. Шаблон: фонарь с лампочкой. createPrefab делает lamp экземпляром нового префаба.
ox::serial::Document prefab = ox::createPrefab(world, lamp);
ox::serial::saveDocument(path, prefab, ox::serial::Format::Binary); // .oxprefab
prefab = *ox::serial::loadDocument(path);

// 2. Экземпляр: новые UUID, ссылки внутри переназначены.
ox::Entity root = *ox::instantiatePrefab(world, prefab /*, parent */);
root.setPosition({10, 0, 0});     // трансформ и имя корня — всегда свои, это не override
ox::Entity iBulb = root.children()[0];

// 3. Изменение на экземпляре -> override.
iBulb.get<ox::LightComponent>().intensity = 1500.0f;
ox::detectOverrides(iBulb, prefab);          // {"Light.intensity"}
ox::recordDetectedOverrides(root, prefab);   // или recordOverride(iBulb, "Light.intensity")

// 4. Новая версия префаба из изменённого экземпляра и рассылка всем экземплярам.
ox::serial::Document prefabV2 = ox::applyInstanceToPrefab(world, lamp, prefab);
ox::updatePrefabInstances(world, prefabV2);  // overrides сохраняются, новые сущности добавляются, удалённые — удаляются

// 5. Откат.
ox::revertOverride(iBulb, "Light.intensity", prefabV2);
ox::revertAllOverrides(root, prefabV2);
```

Полный пример: `samples/guide_examples/03-ecs-scene/prefabs.cpp`.

## Шаг 8. Клонирование мира

`world.clone()` делает глубокую копию всех зарегистрированных компонентов с теми же UUID и даже теми же `entt::entity`. На этом построен play-in-editor: редактор клонирует редактируемый мир, симулирует копию и выбрасывает её при выходе из режима игры (`Engine::enterPlayMode/exitPlayMode`).

```cpp
std::unique_ptr<ox::World> play = edit.clone();
ox::Entity copy = play->find(box.uuid());
copy.setPosition({9, 9, 9}); // оригинал в edit не меняется
```

## Типичные ошибки и подводные камни

- **Запись через `registry().get<TransformComponent>()` не помечает трансформ грязным.** Кэш мировой матрицы устареет, и объект «не сдвинется» на экране. Используйте `entity.transform()`, `setPosition()` и другие сеттеры или `entity.patch<TransformComponent>(...)`.
- **`entt::entity` в полях компонентов.** После загрузки, вставки или инстанцирования префаба такие значения указывают в никуда. Для ссылок используйте `EntityRef` и `world.resolve(ref)`.
- **Компонент не сохраняется / не виден редактору.** Его забыли отразить или зарегистрировать в `ComponentRegistry`. Компонент без полей зарегистрировать нельзя (static_assert): EnTT не хранит пустые типы. Для меток используйте хотя бы одно поле.
- **Кэшированный `WorldTransformComponent::matrix` посреди `Update`** ещё не учитывает перемещения этого кадра (пересчёт идёт в `PostUpdate`). Если нужна точная позиция сразу после сдвига, используйте `worldPosition()`.
- **`destroy()` отложенный.** До конца кадра `valid()` возвращает true, и сущность видна во view. Проверяйте `has<ox::PendingDestroyTag>()`, если это важно.
- **Хэндл `Entity` после удаления** невалиден (`valid() == false`), но `Entity` не обнуляется сам. Не храните хэндлы между кадрами, храните `EntityRef`.
- **`findByName` — линейный поиск** по всем сущностям. Не вызывайте его каждый кадр.
- **Сущности, созданные напрямую через `registry().create()`,** не попадают ни в иерархию, ни в индекс UUID. Создавайте сущности только через `World::create*`.
- **Вложенные префабы** пока «расплющиваются»: если в префаб попадает экземпляр другого префаба, он становится обычными сущностями внешнего.
- **Гранулярность overrides — поле компонента целиком.** Изменение `position.x` переопределяет всё поле `position`.
- **`World::clone()` копирует только компоненты из `ComponentRegistry`.** Незарегистрированные рантайм-компоненты в копию не попадут.

## API

| Заголовок | Что внутри |
| --- | --- |
| [`scene.hpp`](../../engine/scene/include/oxwald/scene/scene.hpp) | Общий заголовок модуля, `registerSceneTypes()` |
| [`world.hpp`](../../engine/scene/include/oxwald/scene/world.hpp) | `World`, `Entity` |
| [`components.hpp`](../../engine/scene/include/oxwald/scene/components.hpp) | Встроенные компоненты, `TransformDirtyTag`, `PendingDestroyTag` |
| [`entity_ref.hpp`](../../engine/scene/include/oxwald/scene/entity_ref.hpp) | `EntityRef` |
| [`component_registry.hpp`](../../engine/scene/include/oxwald/scene/component_registry.hpp) | `ComponentRegistry`, `ComponentInfo`, `ComponentOptions` |
| [`system.hpp`](../../engine/scene/include/oxwald/scene/system.hpp) | `ISystem`, `SystemPhase`, `SystemContext`, `SystemScheduler`, `TransformSystem` |
| [`scene_serializer.hpp`](../../engine/scene/include/oxwald/scene/scene_serializer.hpp) | `saveScene`/`loadScene`, `serializeWorld`/`deserializeWorld`, `copyEntities`/`pasteEntities`, `SceneSerializeOptions` |
| [`prefab.hpp`](../../engine/scene/include/oxwald/scene/prefab.hpp) | `createPrefab`, `instantiatePrefab`, overrides, `applyInstanceToPrefab`, `updatePrefabInstances` |

Заметки для разработчиков модуля: [`docs/dev/modules/scene.md`](../dev/modules/scene.md).

## Что дальше

- [02. Рефлексия и сериализация](02-reflection-serialization.md): атрибуты полей, формат OXB1, `oxdump`.
- [05. Runtime и игровой цикл](05-runtime.md): как `Engine` запускает системы, загружает уровни и передаёт мир рендеру.
- [07. Сохранения](07-savegames.md): как сохраняются сущности и поля `attr::SaveGame`.
- [15. Скрипты на Lua](15-scripting-lua.md): поведение сущностей на Lua.
- ECS-компоненты геймплея *(скоро)*: готовые компоненты физики, анимации, звука, ИИ и скриптов.
- [Оглавление](README.md).
