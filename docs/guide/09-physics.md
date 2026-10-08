# 09. Физика

> Модуль `physics` (таргет `Oxwald::physics`, пространство имён `ox::physics`). Зависит только от `core` и glm; внутри — [Jolt Physics](https://github.com/jrouwe/JoltPhysics) 5.5. Компоненты ECS, связывающие физику со сценой, описаны в главе [32. Компоненты ECS](32-gameplay-components.md); здесь — сам физический мир, которым они пользуются.

## Зачем

Физика отвечает на три вопроса игры:

1. **Как движутся объекты** — твёрдые тела (rigid bodies) падают, сталкиваются, катятся, висят на петлях.
2. **Что где находится** — лучи (raycast), «толстые лучи» (shape cast), проверки перекрытия (overlap) для стрельбы, прицеливания, AI и камеры.
3. **Что с чем столкнулось** — события контактов и триггеров для звуков ударов, урона, зон чекпоинтов.

Плюс отдельный **контроллер персонажа** (character controller): капсула, которая ходит по лестницам и склонам и не ведёт себя как мячик.

Jolt — приватная зависимость: публичные заголовки не включают ни одного файла Jolt, наружу торчат только glm-типы и типы `core`. Вам не нужно знать Jolt, чтобы пользоваться модулем, и не нужно повторять его флаги компиляции (`JPH_*`) — в том числе модуль собран без RTTI (`-fno-rtti`), но ваш код может использовать RTTI как обычно.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `PhysicsWorld` | Физический мир: тела, персонажи, соединения, запросы, события. Все вызовы — из одного потока (поток фиксированного обновления); Jolt сам распараллеливает `step()`. |
| `PhysicsWorldDesc` | Параметры мира: лимиты, потоки, гравитация, слои коллизий, настройки решателя. |
| `BodyDesc` / `BodyHandle` | Описание тела и его handle. Handle хранит индекс и номер поколения — устаревший handle распознаётся (`isValid`). |
| `MotionType` | `Static` (не движется), `Kinematic` (двигаете вы, на него не действуют силы), `Dynamic` (симулируется). |
| `ShapeDesc` / `ShapeRef` | Описание формы (box, sphere, capsule, cylinder, convex hull, triangle mesh, height field, compound) и ссылка на готовую форму с подсчётом ссылок. Одну форму можно делить между тысячами тел. |
| `ShapeCache` | Дедупликация форм по описанию: одинаковые `ShapeDesc` → один объект. У мира есть свой: `world.shapeCache()`. |
| `ObjectLayer` / `CollisionLayers` | До 32 слоёв объектов и симметричная матрица «кто с кем сталкивается». |
| Sensor (триггер) | Тело с `isSensor = true`: не отталкивает, а генерирует Enter/Stay/Exit. |
| `CharacterHandle` | Кинематический персонаж с collide-and-slide (Jolt `CharacterVirtual`). |
| `ConstraintHandle` | Соединение двух тел (или тела с миром): петля, слайдер, шарнир, дистанция, конус, сварка. |

Единицы — СИ: метры, килограммы, секунды, ньютоны. Система координат движка: правая, **Y вверх**. Сборка Jolt — одинарной точности, поэтому держите игровой мир в пределах ~±10 км от начала координат.

## Шаг 1. Мир, тела и формы

```cpp
#include <oxwald/physics/physics.hpp>
using namespace ox::physics;

// 1. Описание мира: слои коллизий настраиваются ДО создания мира.
PhysicsWorldDesc desc;
desc.workerThreads = 2; // потоки Jolt (по умолчанию: число ядер - 1)
ObjectLayer props = *desc.layers.addLayer("Props"); // пользовательский слой
desc.layers.setCollides(props, layers::Debris, false);
PhysicsWorld world(desc);

// 2. Статический пол: верхняя грань на y = 0.
BodyDesc floor;
floor.shape = world.shapeCache().getOrCreate(ShapeDesc::box({50.f, 0.5f, 50.f}));
floor.position = {0.f, -0.5f, 0.f};
floor.motionType = MotionType::Static;
world.createBody(floor);

// 3. Динамический ящик.
BodyDesc crate;
crate.shape = world.shapeCache().getOrCreate(ShapeDesc::box(glm::vec3(0.5f)));
crate.position = {0.f, 5.f, 0.f};
crate.rotation = glm::angleAxis(0.3f, glm::vec3(0, 1, 0)); // только кватернионы
crate.layer = props;
crate.mass = 20.f;        // кг; 0 = из плотности формы
crate.restitution = 0.2f; // упругость
crate.userData = 1234;    // обычно id сущности
BodyHandle body = world.createBody(crate);

world.addImpulse(body, {0.f, 0.f, 50.f}); // Н·с, применяется сразу
for (int i = 0; i < 180; ++i) {
    world.step(1.f / 60.f);
}
Transform t = world.getTransform(body); // position + rotation
```

Полный пример: `samples/guide_examples/09-physics/basics.cpp`.

`BodyDesc::position` — это начало координат формы, а не центр масс (центр масс отдельно: `getCenterOfMassPosition`). Поле `userData` (u64) — ваш мост обратно в игру: оно приходит во всех попаданиях лучей и событиях.

### Поля `BodyDesc`

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `shape` | — | Форма (обязательно). |
| `position`, `rotation` | 0, identity | Начальный трансформ. |
| `linearVelocity`, `angularVelocity` | 0 | Начальные скорости. |
| `motionType` | `Dynamic` | `Static` / `Kinematic` / `Dynamic`. |
| `layer` | `layers::Auto` | `Auto` выбирает Static/Kinematic/Dynamic/Trigger по типу движения и флагу сенсора. |
| `isSensor` | `false` | Триггер: события вместо столкновений. |
| `mass`, `inertiaDiagonal` | 0, 0 | 0 → считаются из формы (плотность `ShapeDesc::density`, по умолчанию 1000 кг/м³). |
| `friction`, `restitution` | 0.5, 0 | Трение и упругость. |
| `linearDamping`, `angularDamping` | 0.05, 0.05 | Затухание скоростей. |
| `gravityFactor` | 1 | Множитель гравитации (0 — невесомость). |
| `ccd` | `false` | Непрерывная детекция столкновений для быстрых объектов (пули, мячи). |
| `allowSleeping`, `startActive` | `true`, `true` | Засыпание покоящихся тел. |
| `reportContacts` | `true` | Генерировать ли `ContactEvent` для тела. |
| `allowMotionTypeChange` | `false` | Нужно, чтобы позже вызвать `setMotionType()` у тела, созданного статическим. |
| `lockAxes` | `lock::None` | Блокировка степеней свободы, напр. `lock::AllRotation` или `lock::Plane2D`. |
| `userData` | 0 | Ваш идентификатор (id сущности). |

После создания почти всё меняется методами мира: `setTransform`, `setLinearVelocity`, `addForce`/`addImpulse`/`addTorque` (и варианты `...AtPoint`), `setFriction`, `setGravityFactor`, `setMotionType`, `setLayer`, `setShape`, `activate`/`deactivate`. Кинематические тела двигайте через `moveKinematic(body, targetPos, targetRot, dt)` — он выставляет скорости, и тело корректно толкает динамические объекты; `setTransform` — это телепорт.

### Формы

```cpp
// Составная форма (compound): «гантель» из двух сфер и бруска.
ShapeRef dumbbell = createShape(ShapeDesc::compound({
    {ShapeDesc::sphere(0.3f), {-0.6f, 0.f, 0.f}},
    {ShapeDesc::sphere(0.3f), {0.6f, 0.f, 0.f}},
    {ShapeDesc::box({0.6f, 0.08f, 0.08f}), {}},
}));

// Выпуклая оболочка (convex hull) из точек — октаэдр.
ShapeRef rock = createShape(ShapeDesc::convexHull(
    {{0.5f, 0, 0}, {-0.5f, 0, 0}, {0, 0.5f, 0}, {0, -0.5f, 0}, {0, 0, 0.5f}, {0, 0, -0.5f}}));

// Карта высот (height field) 33x33 — только для статических тел.
ShapeRef terrain = createShape(ShapeDesc::heightField(heights, 33, {-16.f, 0.f, -16.f}, {1.f, 1.f, 1.f}));

// Ошибки описания не падают, а возвращают невалидный ShapeRef + текст.
std::string error;
ShapeRef bad = createShape(ShapeDesc::convexHull({{0, 0, 0}}), &error); // bad == false
```

Полный пример: `samples/guide_examples/09-physics/basics.cpp`.

| Форма | Конструктор | Замечания |
| --- | --- | --- |
| Box | `ShapeDesc::box(halfExtents)` | Полуразмеры. |
| Sphere | `ShapeDesc::sphere(radius)` | |
| Capsule | `ShapeDesc::capsule(halfHeight, radius)` | Вдоль локальной Y; `halfHeight` — половина цилиндрической части. |
| Cylinder | `ShapeDesc::cylinder(halfHeight, radius)` | Вдоль локальной Y. |
| Convex hull | `ShapeDesc::convexHull(points)` | Камни, обломки, упрощённые коллайдеры. |
| Triangle mesh | `ShapeDesc::triangleMesh(vertices, indices)` | Только Static/Kinematic. Лицевая сторона — CCW. |
| Height field | `ShapeDesc::heightField(heights, n, offset, scale)` | Только Static; `kHeightFieldHole` — дыра. |
| Compound | `ShapeDesc::compound({{desc, pos, rot}, ...})` | Сложные динамические тела. |

Поверх любой формы в описании можно задать `scale` и `centerOfMassOffset` (смещённый центр масс — неваляшка, машина с низким центром тяжести). Для готовых `ShapeRef` есть `makeScaled`, `makeRotatedTranslated`, `makeOffsetCenterOfMass`.

## Шаг 2. Слои коллизий

`CollisionLayers::makeDefault()` (это значение `PhysicsWorldDesc::layers` по умолчанию) создаёт шесть слоёв:

| Слой | Сталкивается с |
| --- | --- |
| `Static` | Dynamic, Character, Debris |
| `Dynamic` | всеми |
| `Kinematic` | Dynamic, Character, Trigger, Debris |
| `Character` | Static, Dynamic, Kinematic, Character, Trigger |
| `Trigger` | Dynamic, Kinematic, Character |
| `Debris` | Static, Dynamic, Kinematic |

Свои слои добавляются через `addLayer(name, broadPhaseLayer, collidesWith)` — по умолчанию новый слой сталкивается со Static/Dynamic/Kinematic/Character. Матрица симметрична: `setCollides(a, b, false)` выключает обе стороны. После создания мира слои **неизменяемы** (Jolt кеширует фильтры), так что настраивайте их до конструктора `PhysicsWorld`. `world.layers().find("Props")` находит слой по имени.

Слои — это ещё и маски запросов: `layerBit(layer)` даёт бит, `layers().collisionMask(layer)` — маску всех слоёв, с которыми слой сталкивается.

## Шаг 3. Фиксированный шаг и интерполяция

Физика должна шагать с фиксированным `dt`, а рендер — с любым. Для этого есть `FixedStepper` (накопитель времени) и `TransformInterpolator` (хранит предыдущее и текущее состояние тел):

```cpp
FixedStepper stepper(60.f);   // 60 Гц, не больше 4 шагов за кадр
TransformInterpolator interp; // хранит предыдущее и текущее состояние
interp.track(ball);

// каждый кадр:
for (u32 i = stepper.advance(frameDt); i > 0; --i) {
    world.step(stepper.fixedDt());
    interp.capture(world);
}
Transform renderXf = interp.get(ball, stepper.alpha()); // это и рисуем
```

Полный пример: `samples/guide_examples/09-physics/basics.cpp`.

`advance` возвращает число шагов на этот кадр и отбрасывает лишнее время сверх `maxStepsPerFrame` (защита от «спирали смерти» на просадках). После телепорта тела вызовите `interp.reset(body, transform)`, иначе объект «проедет» через полэкрана. Удалённые тела выпадают из интерполятора сами при следующем `capture`.

`step(dt, collisionSteps)` принимает число подшагов коллизий — увеличивайте его для очень быстрых объектов или большого `dt`.

## Шаг 4. Запросы

```cpp
const glm::vec3 eye{0.f, 10.f, 0.f}, down{0.f, -1.f, 0.f}; // направление можно не нормировать
if (auto hit = world.raycast(eye, down, 100.f)) {
    // hit->body, hit->userData, hit->point, hit->normal, hit->distance, hit->fraction
}

// Исключить слой: маска — битовое поле по слоям.
QueryFilter noProps;
noProps.layerMask = kAllLayers & ~layerBit(props);

// Игнорировать конкретные тела (например, самого стреляющего).
BodyHandle self[] = {prop};
QueryFilter ignoreSelf;
ignoreSelf.ignoreBodies = self; // span: массив должен жить до конца запроса

// Произвольный предикат (вызывается под блокировкой тела — не трогайте world внутри).
QueryFilter onlyTagged;
onlyTagged.predicate = [](BodyHandle, u64 userData) { return userData == 77; };

// Все попадания, отсортированные по расстоянию.
std::vector<RayHit> all = world.raycastAll(eye, down, 100.f);
```

Полный пример: `samples/guide_examples/09-physics/queries.cpp`.

| Запрос | Возвращает | Типичное применение |
| --- | --- | --- |
| `raycast` / `raycastAll` | `optional<RayHit>` / `vector<RayHit>` | Выстрел, линия взгляда, проверка земли. |
| `sphereCast`, `boxCast`, `capsuleCast`, `shapeCast` | `optional<ShapeCastHit>` | Камера, отбрасываемая от стен; «толстые» пули; проверка, пролезет ли персонаж. |
| `overlapSphere`, `overlapBox`, `overlapShape` | `vector<BodyHandle>` | Взрыв, зона атаки, поиск объектов рядом. |
| `overlapAabb` | `vector<BodyHandle>` | Быстрая грубая проверка только по AABB (broad phase). |
| `closestPoint` | `optional<ClosestPointResult>` | Ближайшая поверхность (магнит, подсказки AI). |

```cpp
auto sweep = world.sphereCast({0.f, 5.f, 0.f}, 0.5f, {0.f, -1.f, 0.f}, 10.f);
std::vector<BodyHandle> nearby = world.overlapSphere({5.f, 1.2f, 0.f}, 0.5f);
auto cp = world.closestPoint({5.f, 3.f, 0.f}, 5.f, noFloor); // body, point, distance
```

По умолчанию сенсоры в запросы не попадают (`QueryFilter::includeSensors = false`). У `ShapeCastHit` нормаль смотрит обратно на движущуюся форму, а `startedPenetrating` сообщает, что форма уже стояла внутри препятствия.

## Шаг 5. Триггеры и события контактов

```cpp
BodyDesc zone;
zone.shape = createShape(ShapeDesc::box({2.f, 1.f, 2.f}));
zone.motionType = MotionType::Static;
zone.isSensor = true; // слой по умолчанию станет layers::Trigger
zone.userData = 500;
BodyHandle trigger = world.createBody(zone);

world.setTriggerCallback([&](const TriggerEvent& e) {
    if (e.type == TriggerEventType::Enter) { /* e.trigger, e.other, e.otherUserData */ }
});
```

```cpp
world.step(1.f / 60.f);
// События валидны до следующего step().
for (const ContactEvent& c : world.contactEvents()) {
    if (c.type == ContactEventType::Begin && c.normalImpulse > 50.f) {
        playImpactSound(c.points[0], c.normalImpulse); // оценка импульса удара, Н·с
    }
}
```

Полный пример: `samples/guide_examples/09-physics/events.cpp`.

Два равноправных способа получить события: прочитать списки после `step()` (`contactEvents()`, `triggerEvents()`, `constraintBrokenEvents()`) или зарегистрировать колбэки (`setContactCallback`, `setTriggerCallback`, `setConstraintBrokenCallback`). Колбэки вызываются **в потоке, вызвавшем `step()`**, после симуляции — из них можно спокойно менять мир.

| Событие | Типы | Что внутри |
| --- | --- | --- |
| `ContactEvent` | `Begin`, `Persist`, `End` | `bodyA`/`bodyB` (`bodyA.id < bodyB.id`), userData обоих, `normal` (от A к B), `penetration`, до 4 точек `points`, `normalImpulse` (только Begin, оценка). |
| `TriggerEvent` | `Enter`, `Stay`, `Exit` | `trigger`, `other` и их userData. Stay — каждый шаг, пока перекрываются. |
| `ConstraintBrokenEvent` | — | `constraint`, `force`, `torque` в момент разрыва. |

Семантика, которую важно знать:

- События агрегируются **по паре тел**: составная форма даёт одно событие, а не по событию на подформу. Порядок событий детерминирован.
- `Persist` приходит раз в шаг на пару; если он не нужен, выключите `PhysicsWorldDesc::reportPersistContacts`. Отдельное тело можно «заглушить» через `BodyDesc::reportContacts = false`.
- Уснувшее тело **не** генерирует `End`/`Exit` — ящик, лежащий на полу, остаётся «в контакте».
- Удаление тела даёт `End`/`Exit` на следующем шаге.
- `normalImpulse` — оценка до решателя (Jolt `EstimateCollisionResponse`), а не точный импульс; для громкости звука её достаточно.

## Шаг 6. Контроллер персонажа

```cpp
CharacterDesc cd;
cd.position = {0.f, 0.1f, 0.f}; // позиция = ступни (низ капсулы)
cd.height = 1.8f;
cd.radius = 0.3f;
cd.maxSlopeAngle = glm::radians(45.f);
cd.maxStepHeight = 0.35f;
cd.userData = 0xC0FFEE;
CharacterHandle ch = world.createCharacter(cd);

// Каждый фиксированный шаг: сначала персонаж, потом step().
world.moveCharacter(ch, dt, CharacterMoveInput{.desiredVelocity = wishDir * 4.f, .jump = jumpPressed});
world.step(dt);

CharacterState s = world.getCharacterState(ch);
if (s.groundState == GroundState::OnGround) { /* ... */ }
```

Полный пример: `samples/guide_examples/09-physics/character.cpp`.

`moveCharacter` реализует привычную логику: на земле скорость = скорость опоры (движущейся платформы) + ввод (+ прыжок); в воздухе вертикальная скорость сохраняется, горизонталью управляет `airControl`; плюс гравитация. Затем — collide-and-slide с автоматическим подъёмом на ступеньки (`maxStepHeight`) и «прилипанием» к полу на спусках (`stickToFloorDistance`). Если нужна своя логика движения — `setCharacterVelocity` + `updateCharacter`.

| `CharacterDesc` | По умолчанию | Смысл |
| --- | --- | --- |
| `height`, `radius` | 1.8, 0.3 | Капсула (или `customShape` с началом в ступнях). |
| `maxSlopeAngle` | 45° (рад) | Круче — это стена, персонаж соскальзывает. |
| `maxStepHeight` | 0.35 | Высота автоматически преодолеваемой ступеньки; 0 — выключить. |
| `stickToFloorDistance` | 0.5 | Прижимание к полу при сходе с небольших уступов. |
| `mass`, `maxStrength` | 70, 100 | Давление на тела под ногами; максимальная сила толкания динамических тел (Н). |
| `layer` | `Character` | Слой коллизий. |
| `innerBody` | `true` | Кинематическое «внутреннее» тело, чтобы персонажа видели лучи, триггеры и динамика. |

| `CharacterState` | Смысл |
| --- | --- |
| `position`, `rotation`, `linearVelocity` | Текущее состояние. |
| `groundState` | `OnGround`, `OnSteepGround` (слишком круто), `NotSupported` (касается, но не стоит), `InAir`. |
| `groundNormal`, `groundVelocity`, `groundBody`, `groundUserData` | На чём стоим. |

Присесть — `setCharacterShape(ch, smallerCapsule)`: вернёт `false`, если новая форма во что-то упрётся (нельзя встать под низким потолком). Телепорт — `setCharacterTransform`. Внутреннее тело — `getCharacterInnerBody(ch)`; именно его вы увидите в `RayHit::body`.

## Шаг 7. Соединения (constraints)

```cpp
ConstraintDesc hinge;
hinge.type = ConstraintType::Hinge;
hinge.bodyA = doorBody;           // bodyB пустой -> крепление к миру
hinge.pointA = {0.f, 1.f, 0.f};   // мировые координаты петли
hinge.axis = {0.f, 1.f, 0.f};
hinge.limitsEnabled = true;
hinge.limitMin = -1.5f;           // радианы
hinge.limitMax = 0.f;
ConstraintHandle h = world.createConstraint(hinge);

world.setMotor(h, MotorMode::Velocity, -1.f); // открыть с 1 рад/с
world.setMotor(h, MotorMode::Position, -0.5f); // или поставить на угол
f32 angle = world.getJointValue(h);
```

```cpp
ConstraintDesc weld;
weld.type = ConstraintType::Fixed;
weld.bodyA = lamp;
weld.pointA = {0.f, 5.5f, 0.f};
weld.breakForce = 500.f; // держит вес 98 Н, но не рывок
world.setConstraintBrokenCallback([&](const ConstraintBrokenEvent& e) { /* лампа оторвалась */ });
```

Полный пример: `samples/guide_examples/09-physics/constraints.cpp`.

| Тип | Что делает | Важные поля |
| --- | --- | --- |
| `Fixed` | Сваривает тела в текущем взаимном положении. | `pointA` |
| `Point` | Шаровой шарнир: `pointA` на A совпадает с `pointB` на B. | `pointA`, `pointB` |
| `Hinge` | Вращение вокруг `axis` через `pointA` (двери, колёса). | `limitMin/Max` (рад), мотор |
| `Slider` | Перемещение вдоль `axis` (лифты, поршни). | `limitMin/Max` (м), мотор |
| `Distance` | Держит расстояние в `[minDistance, maxDistance]` (верёвка, пружина). | `springFrequency`, `springDamping` |
| `Cone` | Шарнир, ось которого не выходит из конуса (плечо, цепь). | `coneHalfAngle` |

Все точки и оси задаются **в мировых координатах на момент создания**. Мотор (`motorMode`, `motorTarget`, `motorMaxForce`, для позиционного — `motorFrequency`/`motorDamping`) работает у Hinge и Slider. `breakForce`/`breakTorque` делают соединение разрушаемым: при превышении оно выключается (не удаляется) и приходит `ConstraintBrokenEvent`. Удаление тела удаляет и все его соединения.

## Шаг 8. Отладочная отрисовка

```cpp
struct MySink final : PhysicsDebugSink {
    void line(const glm::vec3& a, const glm::vec3& b, const Color& c) override { debugDraw.line(a, b, c); }
    void text(const glm::vec3& p, std::string_view s, const Color& c) override { debugDraw.text(p, s, c); }
};

DebugDrawOptions o;
o.aabbs = true;  // + рамки AABB
o.labels = true; // + подписи (id тела / userData)
world.debugDraw(sink, o);
```

Полный пример: `samples/guide_examples/09-physics/advanced.cpp`.

Каркасы строятся из данных форм (составные и масштабированные формы раскладываются на части). Цвета различают static/dynamic/kinematic/sensor/sleeping/character; всё настраивается в `DebugDrawOptions` — там же `contacts`, `velocities`, `centerOfMass`, `constraints`, `filledTriangles` (меши через `triangle()`) и лимит `maxTrianglesPerBody` для огромных мешей и ландшафта. `triangle()` и `text()` по умолчанию реализованы через `line()` / пустышку, так что переопределять обязательно только `line()`.

## Шаг 9. Свой планировщик задач (job executor)

По умолчанию Jolt держит собственный пул потоков (`PhysicsWorldDesc::workerThreads`; `0` — однопоточно). Если в игре уже есть система задач, отдайте работу ей, реализовав `IPhysicsJobExecutor`:

```cpp
class SimpleExecutor final : public IPhysicsJobExecutor {
public:
    u32 maxConcurrency() const override { return u32(m_threads.size()) + 1; }
    void submit(void (*fn)(void*), void* ctx) override { // не блокировать!
        { std::lock_guard lock(m_mutex); m_queue.emplace_back(fn, ctx); }
        m_cv.notify_one();
    }
    // ... пул потоков, вызывающий fn(ctx) ровно один раз
};

SimpleExecutor executor(3);
PhysicsWorldDesc desc;
desc.jobExecutor = &executor; // не владеет — executor должен пережить мир
PhysicsWorld world(desc);
```

Полный пример (с рабочим пулом потоков): `samples/guide_examples/09-physics/advanced.cpp`.

Контракт: `submit` не блокируется и рано или поздно вызывает `fn(ctx)` ровно один раз на любом потоке.

## Шаг 10. Снапшоты и детерминизм

```cpp
std::vector<u8> snap = world.saveState(); // скорости, контакты, соединения, персонажи
// ... симулируем дальше ...
world.restoreState(snap); // те же тела должны существовать
```

Полный пример: `samples/guide_examples/09-physics/advanced.cpp`.

Восстановление и повторная симуляция дают побитово те же трансформы и те же события — основа для отката (rollback), реплеев и отладки. Снапшот не создаёт и не удаляет объекты: набор тел, соединений и персонажей должен совпадать (создан в том же порядке). Jolt детерминирован в пределах **одного бинарника** при одинаковом порядке вызовов, независимо от числа потоков; между разными CPU/компиляторами побитового совпадения нет (сборка без `JPH_CROSS_PLATFORM_DETERMINISTIC`) — для сетевой игры используйте авторитарный сервер и коррекцию состояния (см. главу 14).

## Типичные ошибки и подводные камни

- **Вызовы мира из нескольких потоков.** `PhysicsWorld` не потокобезопасен снаружи: всё — из потока фиксированного обновления и никогда параллельно со `step()`.
- **Слои после создания мира.** `CollisionLayers` копируются в мир в конструкторе; позже изменить матрицу нельзя.
- **Переменный `dt`.** Вызов `step(frameDt)` даёт нестабильные стопки и дрожание. Используйте `FixedStepper` + интерполяцию.
- **Телепорт кинематики через `setTransform`.** Динамические тела не получат скорость и будут «проваливаться» в платформу. Для движущихся платформ — `moveKinematic`.
- **Triangle mesh на динамическом теле.** Меши и height field — только для статики (mesh — и для кинематики). Для динамики используйте convex hull или compound.
- **`setMotionType` у статического тела** без `allowMotionTypeChange = true` при создании.
- **Сохранение `std::span` в `QueryFilter::ignoreBodies`** на временный массив — массив должен жить, пока идёт запрос.
- **Обращение к миру из `QueryFilter::predicate`.** Предикат вызывается под блокировкой тела — только чистая логика по `userData`.
- **Ожидание `End` для уснувшего тела.** Покоящиеся тела «замораживают» контакт, End придёт только при пробуждении и расхождении или удалении.
- **Порядок «персонаж → step».** `moveCharacter` нужно вызывать до `step()` в том же фиксированном шаге.
- **Ящики слегка «тонут» друг в друге** на `penetrationSlop` (2 см) — это нормально и держит стопки стабильными.
- **Огромные координаты.** Одинарная точность: дальше ~10 км от начала координат точность падает; используйте сдвиг начала координат (origin shifting) в открытом мире.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`physics.hpp`](../../engine/physics/include/oxwald/physics/physics.hpp) | Зонтичный заголовок, `backendInfo()` |
| [`physics_world.hpp`](../../engine/physics/include/oxwald/physics/physics_world.hpp) | `PhysicsWorldDesc`, `PhysicsWorld` |
| [`types.hpp`](../../engine/physics/include/oxwald/physics/types.hpp) | Handles, `ObjectLayer`, `layers::*`, `LayerMask`, `MotionType`, `Transform`, `Aabb` |
| [`collision_layers.hpp`](../../engine/physics/include/oxwald/physics/collision_layers.hpp) | `CollisionLayers`, `broadphase::*` |
| [`shape.hpp`](../../engine/physics/include/oxwald/physics/shape.hpp) | `ShapeDesc`, `ShapeRef`, `createShape`, `ShapeCache`, `make*` |
| [`body.hpp`](../../engine/physics/include/oxwald/physics/body.hpp) | `BodyDesc`, `lock::*` |
| [`query.hpp`](../../engine/physics/include/oxwald/physics/query.hpp) | `QueryFilter`, `RayHit`, `ShapeCastHit`, `ClosestPointResult` |
| [`events.hpp`](../../engine/physics/include/oxwald/physics/events.hpp) | `ContactEvent`, `TriggerEvent`, `ConstraintBrokenEvent` |
| [`character.hpp`](../../engine/physics/include/oxwald/physics/character.hpp) | `CharacterDesc`, `CharacterMoveInput`, `CharacterState`, `GroundState` |
| [`constraint.hpp`](../../engine/physics/include/oxwald/physics/constraint.hpp) | `ConstraintDesc`, `ConstraintType`, `MotorMode` |
| [`interpolation.hpp`](../../engine/physics/include/oxwald/physics/interpolation.hpp) | `FixedStepper`, `TransformInterpolator` |
| [`debug_draw.hpp`](../../engine/physics/include/oxwald/physics/debug_draw.hpp) | `PhysicsDebugSink`, `DebugDrawOptions` |
| [`job_executor.hpp`](../../engine/physics/include/oxwald/physics/job_executor.hpp) | `IPhysicsJobExecutor` |

Заметки для разработчиков модуля (сборка Jolt, детерминизм, ограничения): [`docs/dev/modules/physics.md`](../dev/modules/physics.md).

## Что дальше

- [10. Анимация](10-animation.md) — foot IK использует лучи физики, root motion двигает персонажа.
- [11. Сплайны](11-splines.md) — кинематические платформы и камеры, едущие по пути.
- [05. Рантайм](05-runtime.md) — где в игровом цикле живёт фиксированное обновление.
- [14. Сеть](14-networking.md) — снапшоты, предсказание и коррекция состояния.
- [32. Компоненты ECS](32-gameplay-components.md) — компоненты тел, коллайдеров и персонажей поверх этого модуля.
- [Оглавление](README.md).
