# 10. Анимация

> Модуль `animation` (таргет `Oxwald::animation`, пространство имён `ox::anim`). Зависит только от `core` и glm; assimp — приватная зависимость импортёра. Модуль ничего не знает про ECS и рендерер: компоненты аниматора и скиннинга на сцене описаны в главе [32. Компоненты ECS](32-gameplay-components.md).

## Зачем

Скелетная анимация — это конвейер, который каждый кадр превращает «персонаж бежит и целится» в набор матриц для GPU:

```
клипы ─► сэмплирование ─► блендинг / blend spaces ─► state machine (Animator) ─► IK ─► палитра скиннинга
                                                            │
                                                  события, root motion
```

Модуль закрывает весь путь: скелеты и клипы (с импортом из glTF/FBX), компактный рантайм-формат, блендинг поз и маски, аддитивные анимации, 1D/2D blend spaces, машину состояний в духе Unity Animator с кроссфейдами и слоями, root motion, события (шаги, удары), IK (две кости, прицеливание, взгляд, FABRIK, ноги по земле) и скиннинг (CPU-эталон и палитры для compute-шейдера).

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `Skeleton` | Иерархия суставов (joints): имена, родители, bind-поза, inverse bind-матрицы. Суставы **топологически отсортированы**: родитель всегда раньше ребёнка. |
| `Transform` | Перенос / вращение (кватернион) / масштаб. `parent * child` — композиция. |
| `Pose` | Локальная поза: по `Transform` на сустав, в порядке скелета. |
| Модельное пространство | Трансформы суставов относительно корня персонажа (`localToModel`). |
| `AnimationClip` | Клип: треки T/R/S на сустав (SoA), интерполяция Step/Linear/CubicSpline, события, root motion. |
| `CompactClip` | Рантайм-формат клипа: плоские массивы, кватернионы по 48 бит. |
| `JointMask` | Веса по суставам (верх тела, одна рука). |
| `BlendSpace1D/2D` | Смешивание клипов по 1–2 параметрам (скорость, направление). |
| `AnimatorController` | Неизменяемый «ассет»: параметры, слои, состояния, переходы. Делится между персонажами. |
| `Animator` | Рантайм-экземпляр контроллера на одного персонажа. |
| Палитра скиннинга | `palette[j] = model[j] * inverseBind[j]` — то, что уходит в шейдер. |

## Шаг 1. Скелет и клип

Для примеров главы скелет и клипы строятся прямо в коде — так проще понять формат (в игре они приходят из импортёра, см. шаг 7).

```cpp
#include <oxwald/animation/animation.hpp>
using namespace ox::anim;

// Shoulder (начало координат) -> Elbow (+1 по Y) -> Wrist (+1 по Y).
Skeleton skel;
const i32 shoulder = skel.addJoint("Shoulder", kNoJoint, Transform{});
const i32 elbow = skel.addJoint("Elbow", shoulder, Transform{{0, 1, 0}, {1, 0, 0, 0}, {1, 1, 1}});
skel.addJoint("Wrist", elbow, Transform{{0, 1, 0}, {1, 0, 0, 0}, {1, 1, 1}});
skel.finalize(); // inverse bind-матрицы из bind-позы

// Клип 1 с: локоть сгибается 0 -> 90°, плечо уезжает на 2 м по +Z.
AnimationClip walk;
walk.name = "bend";
walk.tracks.resize(skel.jointCount());
auto& elbowRot = walk.tracks[1].rotation;
elbowRot.times = {0.0f, 1.0f};
elbowRot.values = {glm::quat(1, 0, 0, 0), glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1))};
auto& rootPos = walk.tracks[0].translation;
rootPos.times = {0.0f, 1.0f};
rootPos.values = {{0, 0, 0}, {0, 0, 2}};
walk.computeDuration(); // = время последнего ключа
walk.addEvent(0.5f, "footstep", 0.8f);

Pose pose;
walk.sample(skel, 0.5f, pose); // pose.local: по Transform на сустав

// Последовательное воспроизведение: курсор кеширует индексы ключей (O(1)).
SamplingCursor cursor;
for (f32 t = 0.0f; t <= 1.0f; t += 1.0f / 60.0f) walk.sample(skel, t, pose, &cursor);

// События в окне (t0, t1].
std::vector<const AnimEvent*> fired;
walk.collectEvents(0.4f, 0.6f, fired);

// Модельное пространство.
std::vector<Transform> model;
localToModel(skel, pose, model);
```

Полный пример: `samples/guide_examples/10-animation/clips.cpp` (скелет — в `guide_rig.hpp`).

Что важно знать о клипах:

- `tracks` индексируются суставом скелета. Пустой трек — сустав остаётся в bind-позе (у аддитивных клипов — «без изменений»).
- Вращения интерполируются по кратчайшей дуге (`rotationBlend`: `Slerp` или более дешёвый `Nlerp`). `fixQuaternionHemispheres()` выравнивает знаки соседних ключей — импортёр делает это сам.
- `CubicSpline` — сплайн Эрмита с семантикой glTF (тангенсы масштабируются интервалом ключа).
- `sample` зажимает время в `[0, duration]`; зацикливанием занимается `Animator` (или вы сами).
- `collectEvents(t0, t1)` при `t1 < t0` считает, что окно перешло через точку цикла.

## Шаг 2. Компактные клипы и сериализация

```cpp
// Компактный рантайм-формат: плоские массивы, кватернионы 48 бит.
const CompactClip compact = CompactClip::build(clip, {.quantizeRotations = true, .resampleRate = 30.0f});
compact.sample(skel, 0.7f, pose);

// Бинарная сериализация (little-endian, magic + версия, проверка границ).
ByteWriter w;
serialize(w, skel);
serialize(w, clip);
serialize(w, compact);
const std::vector<u8> bytes = w.take();

ByteReader r(bytes);
Skeleton skel2;
AnimationClip clip2;
CompactClip compact2;
bool ok = deserialize(r, skel2) && deserialize(r, clip2) && deserialize(r, compact2);
```

Полный пример: `samples/guide_examples/10-animation/clips.cpp`.

`CompactClip` хранит все ключи всех суставов в нескольких плоских массивах, кубические треки пересэмплирует в линейные с частотой `resampleRate`, а вращения (при `quantizeRotations`) пакует методом «smallest three» в 6 байт вместо 16 с ошибкой ≤ 2·10⁻⁴ рад. `memoryBytes()` показывает размер; `toClip()` возвращает обычный клип для инструментов. Сериализуются `Skeleton`, `AnimationClip`, `CompactClip` и `SkinnedMeshData`; обрезанные или испорченные данные дают `false` и никогда не читают за границей буфера.

## Шаг 3. Блендинг поз, маски, аддитивы

```cpp
// Линейный бленд двух поз (вращения — по кратчайшей дуге).
blendPoses(straight, bent, 0.5f, out);

// Взвешенный бленд N поз: веса нормализуются.
const Pose* poses[] = {&straight, &bent};
const f32 weights[] = {3.0f, 1.0f};
std::vector<f32> normalized = blendPosesWeighted(poses, weights, out); // {0.75, 0.25}

// Маска по ветке: только Wrist и его потомки.
JointMask handOnly = JointMask::fromBranch(skel, skel.findJoint("Wrist"));
blendPoses(straight, bent, 1.0f, out, &handOnly);

// Аддитив: «+30° к локтю» поверх любой базовой позы.
Pose bind;
bind.setBind(skel);
const AnimationClip lean = makeAdditiveClip(leanClip, skel, bind);
Pose delta;
lean.sample(skel, 0.0f, delta);
applyAdditive(result, delta, 1.0f); // вес и маска — опционально
```

Полный пример: `samples/guide_examples/10-animation/blending.cpp`.

Аддитивная анимация хранит разницу с опорной позой (обычно первый кадр клипа или bind-поза) и накладывается поверх чего угодно: дыхание, отдача оружия, наклон корпуса. Для горячих путей с множеством поз есть `PoseAccumulator` (`begin` → `add(pose, weight, mask)` → `finish`) — без временных векторов.

## Шаг 4. Blend spaces

```cpp
// 1D: скорость -> idle / walk / run.
BlendSpace1D loco;
loco.addSample(0.0f, idleClip);
loco.addSample(1.5f, walkClip);
loco.addSample(4.0f, runClip);
std::vector<f32> w;
loco.computeWeights(2.75f, w); // ровно между walk и run: {0, 0.5, 0.5}

// 2D: (strafe, forward). Для локомоции — FreeformDirectional.
BlendSpace2D strafe(BlendSpace2D::Mode::FreeformDirectional);
strafe.addSample({0, 0}, idleClip);
strafe.addSample({0, 2}, fwdClip);
strafe.addSample({0, -2}, backClip);
strafe.addSample({2, 0}, rightClip);
strafe.addSample({-2, 0}, leftClip);
strafe.computeWeights({1.0f, 1.0f}, w); // веса всегда в сумме 1
```

Полный пример: `samples/guide_examples/10-animation/blending.cpp`.

| Режим 2D | Как считает веса | Когда использовать |
| --- | --- | --- |
| `Delaunay` | Триангуляция сэмплов, барицентрические веса; снаружи — проекция на ближайшее ребро. | Произвольные параметры (наклон × скорость). |
| `FreeformDirectional` | Gradient band interpolation в полярных координатах. | Локомоция: одни направления на разных скоростях. |
| `FreeformCartesian` | Gradient band interpolation в декартовых координатах. | Параметры без «направленного» смысла. |

1D-пространство интерполирует два соседних сэмпла и зажимает значения вне диапазона. Сами по себе blend spaces только считают веса; сэмплирует и синхронизирует клипы по фазе `Animator` (шаг 5).

## Шаг 5. Animator: состояния, переходы, слои

```cpp
// Контроллер — неизменяемый «ассет», который можно делить между персонажами.
auto ctrl = std::make_shared<AnimatorController>();
const u32 speed = ctrl->addParameter("speed", ParamType::Float);
const u32 jump = ctrl->addParameter("jump", ParamType::Trigger);
const u32 base = ctrl->addLayer("Base");

auto loco = std::make_shared<BlendSpace1D>();
loco->addSample(0.0f, idleClip);
loco->addSample(2.0f, runClip);
const i32 locoState = ctrl->addState(base, {"Locomotion", Motion::fromBlendSpace(loco, speed)});

StateDesc jumpDesc{"Jump", Motion::fromClip(jumpClip)};
jumpDesc.loop = false;
const i32 jumpState = ctrl->addState(base, jumpDesc);

ctrl->addTransition(base, kAnyState, jumpState, 0.1f).when(jump, ConditionOp::Triggered);
auto& back = ctrl->addTransition(base, jumpState, locoState, 0.2f);
back.hasExitTime = true; // уйти, когда прыжок доиграл
back.exitTime = 1.0f;

Animator animator(skel, ctrl); // рантайм-экземпляр на одного персонажа

// каждый кадр:
animator.setFloat(speed, currentSpeed);
if (jumpPressed) animator.setTrigger("jump"); // можно и по имени
animator.update(dt);
const Pose& pose = animator.pose();
```

Полный пример: `samples/guide_examples/10-animation/animator.cpp`.

**Параметры**: `Float`, `Int`, `Bool`, `Trigger`. Задаются по индексу (быстро) или по имени (неизвестное имя логируется один раз и игнорируется). Триггер «съедается» переходом, который он запустил.

**Переходы** (`TransitionDesc`):

| Поле | Смысл |
| --- | --- |
| `from` / `to` | Состояния; `from = kAnyState` — переход из любого состояния (проверяется первым и может прервать идущий кроссфейд). |
| `conditions` | Все должны выполняться; добавляются через `.when(param, op, threshold)`. Операции: `Greater`, `Less`, `Equal`, `NotEqual`, `IsTrue`, `IsFalse`, `Triggered`. |
| `hasExitTime`, `exitTime` | Ждать нормализованного времени источника; у зацикленных состояний `exitTime < 1` проверяется на каждом цикле. |
| `duration` | Длительность кроссфейда, секунды. |
| `destinationOffset` | С какого нормализованного времени начать целевое состояние. |
| `canTransitionToSelf` | Только для any-state переходов. |

За одно `update` на слое срабатывает не больше одного перехода. Прерванный кроссфейд «замораживает» последнюю выходную позу и смешивает уже от неё — без рывка. Из кода можно форсировать состояние: `play(layer, state)` или `crossFade(layer, state, duration)`. Для отладки и UI: `currentState`, `nextState`, `inTransition`, `normalizedTime`, `transitionProgress`.

**Состояния** (`StateDesc`): `motion` (клип или blend space), `speed`, необязательный `speedParam` (float-параметр — множитель скорости), `loop`. В состоянии с blend space все клипы синхронизированы по фазе: нормализованное время растёт на `dt / Σ wᵢ·durationᵢ`, поэтому шаги ходьбы и бега совпадают.

**Слои**: слой 0 — базовый. Остальные применяются по порядку: `LayerBlend::Override` (lerp с весом слоя × маска) или `LayerBlend::Additive`. Вес меняется на лету: `setLayerWeight`.

```cpp
// Слой «рука» поверх базы с весом 0.5, маска — только Wrist.
const u32 upper = ctrl->addLayer("Hand", LayerBlend::Override, 0.5f);
ctrl->addState(upper, {"Wave", Motion::fromClip(wave)});
ctrl->layer(upper).mask = JointMask::fromBranch(skel, skel.findJoint("Wrist"));
```

## Шаг 6. События и root motion

```cpp
walk->addEvent(0.5f, "footstep");
extractRootMotion(*walk, skel, {.rootJoint = 0}); // XZ + yaw -> walk->rootMotion, корень «на месте»

Transform owner; // трансформ персонажа в мире
animator.update(dt);
for (const FiredEvent& e : animator.events()) {
    if (e.event->name == "footstep") playFootstep(e.event->payload * e.weight);
}
applyRootMotion(owner, animator.rootMotionDelta());
```

Полный пример: `samples/guide_examples/10-animation/animator.cpp`.

`extractRootMotion` переносит движение корневого сустава (по умолчанию горизонталь XZ и поворот вокруг Y; `translationY` — по желанию) в `clip.rootMotion`, а сам корень делает неподвижным. `Animator` накапливает дельты базового слоя с учётом весов бленда и переходов через точку цикла; дельта выражена в локальном пространстве персонажа. `applyRootMotion` подходит, если персонаж двигается «как есть»; с физикой обычно поступают иначе — переводят дельту в скорость и отдают её контроллеру персонажа (глава 09).

События (`FiredEvent`) приходят для окна (prev, cur] с учётом зацикливания; событие в t = 0 срабатывает при входе в состояние. У событий из смешиваемых клипов есть `weight` — вес клипа в бленде (чтобы не играть шаги «бега» на 5 % веса).

## Шаг 7. Импорт из glTF/FBX

```cpp
ImportSettings settings;
settings.maxInfluences = 4; // 4 или 8 влияний на вершину
std::string error;
std::optional<AnimationImport> imported = importAnimationAsset("assets/hero.glb", settings, &error);
if (!imported) { OX_LOG_ERROR("anim", "{}", error); return; }

const Skeleton& skel = imported->skeleton;         // скелет
const auto& meshes = imported->meshes;             // SkinnedMeshData
const auto& clips = imported->clips;               // AnimationClip
```

Полный пример (glTF собирается в памяти и грузится через `importAnimationAssetFromMemory`): `samples/guide_examples/10-animation/import.cpp`.

| `ImportSettings` | По умолчанию | Смысл |
| --- | --- | --- |
| `sourceUpAxis` | `Auto` | `Auto` читает метаданные FBX (glTF — всегда Y-up); можно форсировать `Y`/`Z`/`X`. |
| `unitToMeters` | 0 (авто) | > 0 — переопределить перевод единиц (0.01 для сантиметров). |
| `scale` | 1 | Дополнительный множитель длин. |
| `maxInfluences` | 4 | 4 или 8 влияний на вершину. |
| `importMeshes`, `importAnimations` | `true` | Что импортировать. |
| `fixQuaternionHemispheres` | `true` | Выравнивание знаков кватернионов. |

Всё приводится к соглашениям движка: правая система, Y вверх, метры; bind-палитра после конвертации остаётся единичной. Суставами становятся все узлы, на которые ссылаются кости (плюс промежуточные узлы до общего предка); в файлах без костей — анимированные узлы. Влияния ограничиваются `maxInfluences`, дубликаты сливаются, веса сортируются и нормализуются. Ошибки (включая исключения assimp) возвращаются как `std::nullopt` + текст.

## Шаг 8. IK

Все решатели работают в модельном пространстве: принимают локальную позу и её модельные трансформы и обновляют **оба** — поэтому их можно выстраивать цепочкой (foot IK → look-at → прицеливание).

```cpp
Pose pose = animator.pose();
std::vector<Transform> model; // IK работает в модельном пространстве
localToModel(skel, pose, model);

TwoBoneIKSettings ik;
ik.root = skel.findJoint("Shoulder");
ik.mid = skel.findJoint("Elbow");
ik.end = skel.findJoint("Wrist");
ik.target = {1.0f, 1.0f, 0.5f};
ik.pole = {0.0f, 1.0f, 5.0f}; // локоть сгибается в сторону +Z
IKResult r = solveTwoBoneIK(skel, pose, model, ik);

AimIKSettings aim;
aim.joint = ik.end;
aim.aimAxis = {0, 1, 0}; // локальная ось кисти, которую наводим на цель
aim.upAxis = {0, 0, 0};  // без контроля скручивания
aim.target = {5.0f, 1.0f, 0.5f};
solveAimIK(skel, pose, model, aim);
```

Ноги по неровной земле — `solveFootIK`. Луч в мир даёт игра, обычно через `PhysicsWorld::raycast`:

```cpp
GroundRaycast ray = [&](const glm::vec3& origin, const glm::vec3& dir, f32 maxDist) -> std::optional<GroundHit> {
    if (auto hit = physicsWorld.raycast(origin, dir, maxDist)) return GroundHit{hit->point, hit->normal};
    return std::nullopt;
};
FootIKSettings settings;
settings.pelvis = pelvis;
settings.ownerWorld = characterTransform; // модель -> мир
const FootIKLeg legs[] = {{hipL, kneeL, ankleL, 0.08f}, {hipR, kneeR, ankleR, 0.08f}};
FootIKResult r = solveFootIK(skel, pose, model, legs, settings, ray);
```

Полный пример: `samples/guide_examples/10-animation/ik.cpp`.

| Решатель | Для чего | Особенности |
| --- | --- | --- |
| `solveTwoBoneIK` | Руки, ноги | Аналитический; `pole` задаёт плоскость сгиба; `allowStretch`/`maxStretch`; `endRotation` — ориентация кисти. |
| `solveAimIK` | Оружие, голова, прожектор | `aimAxis`, необязательный `upAxis` против скручивания, `maxAngle`. |
| `solveLookAtChain` | Взгляд | Поворот распределяется по цепочке (позвоночник → шея → голова) с весами. |
| `solveFABRIK` | Хвосты, щупальца, цепочки любой длины | `tolerance`, `maxIterations`. |
| `solveFootIK` | Ноги на ступеньках и склонах | Лучи вниз, опускание таза под нижнюю ногу, two-bone IK, наклон стопы по нормали (`maxFootAngle`). |

У всех есть `weight` для плавного включения/выключения. `IKResult` сообщает `reached`, оставшуюся ошибку и число итераций. Отдельный примитив `applyModelRotation` поворачивает сустав на заданное вращение в модельном пространстве.

## Шаг 9. Скиннинг

```cpp
SkinnedMeshData bar;               // обычно приходит из импортёра
bar.positions = ...; bar.normals = ...;
bar.setInfluences(influences, 4);  // <= 4 влияний, сортировка и нормализация

std::vector<glm::mat4> model, palette;
localToModel(skel, pose, model);
computeSkinningMatrices(skel, model, palette); // palette[j] = model[j] * inverseBind[j]

std::vector<glm::vec3> skinned, normals;
skinMesh(bar, palette, SkinningMethod::Linear, skinned, &normals);
skinMesh(bar, palette, SkinningMethod::DualQuaternion, skinned); // без «candy wrapper»

// Палитра для GPU-варианта на dual quaternion: 2 x vec4 на сустав.
std::vector<DualQuat> dqPalette;
computeDualQuatPalette(palette, dqPalette);
```

Полный пример: `samples/guide_examples/10-animation/skinning.cpp`.

`skinMesh` — эталонная CPU-реализация и запасной путь. Основной путь — compute-шейдер `engine/shaders/animation/skinning.comp`: он принимает исходные вершины, `SkinnedMeshData::joints` (u16 пары, загружаются как есть), веса и палитру (`mat4[]` или dual quaternion `vec4[2·J]`) через buffer device address. Подключение к рендереру делает интеграционный слой. Linear blend skinning (LBS) дешевле и поддерживает масштаб; dual quaternion skinning (DQS) сохраняет объём на сгибах (локти, плечи), но игнорирует масштаб.

## Шаг 10. Отладочная отрисовка

```cpp
debugDrawSkeleton(skel, model, ownerWorld,
                  [&](glm::vec3 a, glm::vec3 b, glm::vec4 c) { debugDraw.line(a, b, c); },
                  {1, 0.8f, 0.2f, 1}, /*axisLength*/ 0.05f);
```

Полный пример: `samples/guide_examples/10-animation/skinning.cpp`.

Рисует кости линиями «родитель → ребёнок» и маленькие оси на каждом суставе (`axisLength = 0` — без осей).

## Типичные ошибки и подводные камни

- **Родитель после ребёнка.** `addJoint` требует, чтобы родитель уже существовал (`parent < index`) — на этом держатся однопроходные алгоритмы.
- **Забытый `finalize()`** у построенного вручную скелета: inverse bind-матрицы не посчитаны, палитра неверна.
- **Число треков ≠ числу суставов.** `tracks` индексируется суставом; делайте `tracks.resize(skel.jointCount())`.
- **Время клипа вне диапазона.** `sample` зажимает время; цикл — ответственность `Animator` или ваша (`fmod`).
- **IK до `localToModel`.** Решатели ждут согласованные `pose` и `model`; после ручной правки локального трансформа обновите модель (`updateModelFrom`).
- **Неравномерный масштаб в IK.** Решатели рассчитаны на равномерный масштаб; two-bone IK ожидает, что mid и end — прямые дети (twist-кости между ними не компенсируются).
- **Root motion применён дважды:** и через `applyRootMotion`, и через позу корня. После `extractRootMotion` корень в клипе неподвижен — двигайте только владельца.
- **Root motion при прерванном кроссфейде:** замороженный источник не даёт root motion — короткий «провал» скорости возможен.
- **Один `Animator` на несколько персонажей.** Контроллер можно делить, `Animator` — нет: он хранит время, параметры и курсоры.
- **Скелет из импорта уничтожен раньше `Animator`.** `Animator` хранит ссылку на скелет — держите `AnimationImport` (или скопированный `Skeleton`) живым.
- **Кубические треки из glTF через assimp** приходят уже пересэмплированными (assimp не отдаёт тангенсы).

## API

| Заголовок | Содержимое |
| --- | --- |
| [`animation.hpp`](../../engine/animation/include/oxwald/animation/animation.hpp) | Зонтичный заголовок |
| [`transform.hpp`](../../engine/animation/include/oxwald/animation/transform.hpp) | `Transform`, `nlerpShortest`, `slerpShortest`, `rotationBetween` |
| [`skeleton.hpp`](../../engine/animation/include/oxwald/animation/skeleton.hpp) | `Skeleton`, `kNoJoint` |
| [`pose.hpp`](../../engine/animation/include/oxwald/animation/pose.hpp) | `Pose`, `JointMask`, `localToModel`, `blendPoses`, `PoseAccumulator`, аддитивы |
| [`clip.hpp`](../../engine/animation/include/oxwald/animation/clip.hpp) | `AnimationClip`, `Track`, `SamplingCursor`, `AnimEvent`, `extractRootMotion`, `makeAdditiveClip` |
| [`compact_clip.hpp`](../../engine/animation/include/oxwald/animation/compact_clip.hpp) | `CompactClip`, `PackedQuat` |
| [`blend_space.hpp`](../../engine/animation/include/oxwald/animation/blend_space.hpp) | `BlendSpace1D`, `BlendSpace2D`, `delaunayTriangulate` |
| [`animator.hpp`](../../engine/animation/include/oxwald/animation/animator.hpp) | `AnimatorController`, `Animator`, `applyRootMotion` |
| [`ik.hpp`](../../engine/animation/include/oxwald/animation/ik.hpp) | Two-bone, aim, look-at, FABRIK, foot IK |
| [`skinning.hpp`](../../engine/animation/include/oxwald/animation/skinning.hpp) | `SkinnedMeshData`, палитры, `DualQuat`, `skinMesh` |
| [`importer.hpp`](../../engine/animation/include/oxwald/animation/importer.hpp) | `importAnimationAsset`, `importAnimationAssetFromMemory`, `ImportSettings` |
| [`serialization.hpp`](../../engine/animation/include/oxwald/animation/serialization.hpp) | `ByteWriter`, `ByteReader`, `serialize`/`deserialize` |
| [`debug_draw.hpp`](../../engine/animation/include/oxwald/animation/debug_draw.hpp) | `debugDrawSkeleton` |
| [`skinning.comp`](../../engine/shaders/animation/skinning.comp) | Compute-шейдер скиннинга |

Заметки для разработчиков модуля: [`docs/dev/modules/animation.md`](../dev/modules/animation.md).

## Что дальше

- [09. Физика](09-physics.md) — лучи для foot IK и контроллер персонажа для root motion.
- [11. Сплайны](11-splines.md) — движение по пути для катсцен и NPC.
- [02. Рефлексия и сериализация](02-reflection-serialization.md) — общий механизм сохранения данных движка.
- [32. Компоненты ECS](32-gameplay-components.md) — компоненты аниматора и скиннинга на сцене.
- [Оглавление](README.md).
