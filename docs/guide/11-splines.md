# 11. Сплайны

> Модуль `spline` (таргет `Oxwald::spline`, пространство имён `ox::spline`). Только CPU, зависит лишь от `core` и glm. Компонент сплайна на сцене и следование сущностей по пути описаны в главе [32. Компоненты ECS](32-gameplay-components.md).

## Зачем

Сплайны (splines) — гладкие кривые, заданные несколькими контрольными точками. В игре это:

- **пути**: рельсы трамвая, маршрут патруля, пролёт камеры в катсцене, гоночная трасса;
- **геометрия**: дороги, трубы, провода, заборы — меш, «выдавленный» (extruded) вдоль кривой;
- **расстановка**: столбы каждые 10 м, деревья вдоль тропинки;
- **запросы**: «где ближайшая точка трассы к машине», «какой процент круга пройден».

Модуль даёт пять типов кривых, параметризацию по длине дуги (движение с постоянной скоростью), устойчивые кадры (frames) без «перекручивания», движение по пути с событиями, экструзию мешей и отладочную отрисовку.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `Spline` | Кривая: тип, контрольные точки, замкнутость, настройки, маркеры. Кеш вычислений строится лениво. |
| `ControlPoint` | Позиция, Bézier-хэндлы (`inHandle`/`outHandle` — **смещения** от точки), режим хэндлов, крен (`roll`), вес (NURBS), необязательный вектор `up`. |
| Параметр `t` | `t ∈ [0, segmentCount()]`; сегмент `i` — это `[i, i+1]`. У интерполирующих типов точка `i` лежит в `t = i`. |
| Дистанция | Длина дуги от начала кривой, `[0, length()]`. Перевод: `distanceToT` / `tToDistance`. |
| `SplineSample` | Результат `evaluate(t)`: позиция, кадр (tangent/normal/binormal), производные, кривизна. |
| `SplineMarker` | Именованная точка на кривой (по `t`), например `"station"`. |
| `PathFollower` | «Бегунок», движущийся по сплайну с постоянной мировой скоростью и стреляющий событиями. |

### Типы кривых

| `SplineType` | Проходит через точки | Гладкость | Для чего |
| --- | --- | --- | --- |
| `Linear` | да | ломаная | Рельсы из прямых, отладка, сетки. |
| `Bezier` | да | зависит от хэндлов | Ручное редактирование в редакторе (как в Blender/Illustrator). |
| `CatmullRom` | да | C1 (uniform) / G1 | Пути «по точкам» без хэндлов: патрули, камеры. |
| `BSpline` | нет (кроме концов открытой) | C2 | Очень гладкие траектории камеры, «сглаживание» шумных точек. |
| `Nurbs` | нет | степень 1–7, веса | Точные окружности и дуги, данные из CAD. |

Для Catmull-Rom важен `SplineSettings::catmullRomAlpha`: `kCatmullRomUniform` (0), `kCatmullRomCentripetal` (0.5, по умолчанию — без петель и острых выбросов на неравномерных точках), `kCatmullRomChordal` (1).

## Шаг 1. Кривая и её кадр

```cpp
#include <oxwald/spline/spline.hpp>
using namespace ox::spline;

Spline road(SplineType::CatmullRom);
road.addPoint({0, 0, 0});
road.addPoint({10, 0, -5});
road.addPoint({20, 1, 0});
road.addPoint({30, 0, -5});
SplineSettings s = road.settings();
s.catmullRomAlpha = kCatmullRomCentripetal; // 0 uniform, 0.5 centripetal (без петель), 1 chordal
road.setSettings(s);

// t ∈ [0, segmentCount()]; у интерполирующих типов точка i лежит в t = i.
glm::vec3 p1 = road.position(1.0f); // == (10, 0, -5)

SplineSample smp = road.evaluate(1.25f);
// Ортонормированный кадр: tangent — вперёд, normal — «вверх», binormal — «вправо».
glm::quat q = smp.rotation(); // −Z -> tangent, +Y -> normal (как у камеры)
```

Полный пример: `samples/guide_examples/11-splines/curves.cpp`.

Поля `SplineSample`: `position`, `tangent`, `normal`, `binormal` (= `cross(tangent, normal)`), `derivative` (dP/dt, не нормирован), `secondDerivative`, `curvature` (1/радиус — удобно, чтобы сбрасывать скорость в поворотах), `roll`. `rotation(localForward, localUp)` строит кватернион для объекта с другими осями; свободная функция `frameRotation(forward, up)` делает то же для произвольных векторов.

**Режимы кадров** (`SplineSettings::frameMode`):

| Режим | Как считается «верх» | Когда использовать |
| --- | --- | --- |
| `RotationMinimizing` (по умолчанию) | Параллельный перенос (метод двойного отражения), затравка — `settings.upVector`. На замкнутых петлях скрутка распределяется по длине — без скачка на шве. | Трубы, провода, американские горки, всё, что может идти вертикально. |
| `UpVector` | `ControlPoint::up` (интерполируется) или `settings.upVector`, сделанный перпендикулярным касательной. | Дороги и камеры, которые должны оставаться «горизонтальными». |

`ControlPoint::roll` (радианы, интерполируется линейно) добавляет крен поверх обоих режимов — виражи трассы.

## Шаг 2. Bézier и редактирование

```cpp
Spline path(SplineType::Bezier);
path.addPoint({0, 0, 0});
path.addPoint({5, 0, 0}); // новые точки — HandleMode::Auto
path.setHandleMode(0, HandleMode::Mirrored);
path.setOutHandle(0, {1, 2, 0}); // хэндлы — смещения ОТНОСИТЕЛЬНО точки; in станет {-1,-2,0}

const usize idx = path.insertPointAt(0.4f); // разбиение де Кастельжо — форма не меняется
path.addMarker("checkpoint", 1.5f);
u64 v = path.version(); // растёт при каждой правке — признак «пересобрать меш»
```

Полный пример: `samples/guide_examples/11-splines/curves.cpp`.

| `HandleMode` | Поведение |
| --- | --- |
| `Free` | Хэндлы независимы (излом). |
| `Aligned` | На одной прямой, длины независимы (G1). |
| `Mirrored` | На одной прямой и равной длины (C1). |
| `Auto` | Вычисляются из соседей (как Catmull-Rom, C1). Ручная правка хэндла переводит точку в `Aligned`. |

Изменение одного хэндла сразу применяет режим ко второму. `insertPointAt` у Bézier режет сегмент точно (форма сохраняется, маркеры в разрезанном сегменте пересчитываются); у остальных типов вставляет точку кривой в `t`, и форма слегка меняется. Остальные правки — `setPosition`, `setPoint`, `insertPoint`, `removePoint`, `setRoll`, `setWeight`, `setUp`, `setClosed`, `setType`.

## Шаг 3. B-сплайны и NURBS

```cpp
// B-spline аппроксимирует точки (кроме концов открытой кривой).
Spline smooth(SplineType::BSpline);
smooth.setPoints({{.position = {0, 0, 0}}, {.position = {2, 2, 0}}, {.position = {4, 0, 0}},
                  {.position = {6, 2, 0}}});

// Точная окружность NURBS: 9 точек, степень 2, веса 1, √½, 1, ...
const f32 r = 2.0f, w = std::sqrt(0.5f);
const glm::vec3 pts[9] = {{r, 0, 0},   {r, r, 0},   {0, r, 0}, {-r, r, 0}, {-r, 0, 0},
                          {-r, -r, 0}, {0, -r, 0}, {r, -r, 0}, {r, 0, 0}};
std::vector<ControlPoint> cps;
for (int i = 0; i < 9; ++i) cps.push_back({.position = pts[i], .weight = (i % 2) ? w : 1.0f});
Spline circle(SplineType::Nurbs);
SplineSettings s;
s.degree = 2;
s.knots = {0, 0, 0, 0.25f, 0.25f, 0.5f, 0.5f, 0.75f, 0.75f, 1, 1, 1};
circle.setSettings(s);
circle.setPoints(cps);
```

Полный пример: `samples/guide_examples/11-splines/curves.cpp`.

У B-сплайнов и NURBS сегменты — непустые интервалы узлового вектора, а не промежутки между точками, поэтому `segmentCount()` меньше числа точек (у 4-точечного кубического B-сплайна — один сегмент). Открытые кривые «зажаты» (clamped) и начинаются/заканчиваются в крайних точках; замкнутые — периодические. Свой узловой вектор (`settings.knots`, размер `pointCount + degree + 1`) поддерживается только для открытых NURBS.

Низкоуровневые функции для одного кубического сегмента — в `bezier.hpp`: `bezierPosition`, `bezierDerivative`, `bezierSecondDerivative`, `deCasteljau`, `splitBezier`, `hermiteToBezier`.

## Шаг 4. Длина дуги и запросы

Параметр `t` **не пропорционален расстоянию**: если точки расставлены неравномерно, движение «по t» будет то ускоряться, то замедляться. Для всего, что связано с метрами, используйте дистанцию:

```cpp
const f32 len = road.length();

const f32 tHalf = road.distanceToT(len * 0.5f); // середина дороги в метрах
const f32 d = road.tToDistance(tHalf);
SplineSample mid = road.evaluateAtDistance(len * 0.5f);

// Столбы каждые ~2 м: шаг подгоняется, чтобы последний попал точно в конец.
std::vector<SplineSample> posts = road.sampleByDistance(2.0f);

// Ближайшая точка кривой к игроку.
ClosestPointResult hit = road.closestPoint(playerPos); // {t, position, distance}

Bounds box = road.bounds();

// Перед чтением из нескольких потоков: собрать кеш заранее.
road.rebuild();
```

Полный пример: `samples/guide_examples/11-splines/arc_length.cpp`.

`uniformParameters(n)` возвращает `n + 1` значений `t` через равные расстояния — удобно для собственной расстановки. Прогресс по трассе для гонки — `road.tToDistance(road.closestPoint(carPos).t) / road.length()`. У замкнутых сплайнов `t` и дистанции «заворачиваются», у открытых — зажимаются.

## Шаг 5. Движение по пути: PathFollower

```cpp
#include <oxwald/spline/path_follower.hpp>

Spline rails(SplineType::Linear);
rails.addPoint({0, 0, 0});
rails.addPoint({100, 0, 0});
rails.addMarker("station", 0.5f); // маркер на сплайне (по t) = 50 м

PathFollower tram(FollowerSettings{.speed = 10.0f, .loopMode = LoopMode::PingPong});
tram.addEvent("horn", 80.0f); // событие самого follower'а (по дистанции)
tram.setEventCallback([&](const PathEvent& e) {
    // e.name, e.distance, e.direction (+1/-1), e.marker
});

// каждый кадр:
tram.advance(rails, dt);
FollowerPose pose = tram.pose(rails); // position, rotation, distance, t — в пространстве сплайна
```

Полный пример: `samples/guide_examples/11-splines/follower.cpp`.

| `FollowerSettings` | По умолчанию | Смысл |
| --- | --- | --- |
| `speed` | 1 | Единиц в секунду по длине дуги; отрицательная — назад. |
| `loopMode` | `Loop` | `Once` (стоп, `finished()`), `Loop` (на начало; бесшовно на замкнутых), `PingPong` (разворот на концах). |
| `orientToPath` | `true` | Поворачивать объект по кадру кривой. |
| `faceTravelDirection` | `true` | При движении назад разворачиваться «носом» по ходу. |
| `forwardAxis`, `upAxis` | −Z, +Y | Локальные оси объекта, совмещаемые с касательной и нормалью. |
| `fireMarkers` | `true` | Стрелять также маркерами сплайна. |

Скорость постоянна в мировых единицах, как бы ни были расставлены точки. События срабатывают ровно один раз за каждое пересечение, в порядке движения — даже если за один `advance` пересечено несколько, при заворачивании цикла и в обе стороны в ping-pong. Интервалы полуоткрытые: событие ровно в точке разворота сработает один раз, а событие ровно там, где бегунок остановился в этом кадре, — в следующем кадре, когда он его пересечёт.

`PathFollower` не хранит ссылку на сплайн — сплайн передаётся в каждый вызов, поэтому бегунок легко положить в компонент. Поза — в локальном пространстве сплайна: умножьте её на трансформ сущности, которой принадлежит сплайн. Управление: `reset(distance)`, `setDistance`, `setDirection`, `distance()`, `direction()`, `finished()`.

## Шаг 6. Меши вдоль сплайна

```cpp
#include <oxwald/spline/spline_mesh.hpp>

// Труба радиуса 0.3, 12 сегментов, кольца каждые 0.5 м, с крышками.
ExtrudedMesh pipe = extrude(s, makeCircleProfile(0.3f, 12), {.spacing = 0.5f, .capStart = true, .capEnd = true});

// Дорога шириной 6 м: плоская лента, нормали вверх, v растёт с дистанцией.
ExtrudedMesh road = extrude(s, makeStripProfile(6.0f), {.spacing = 1.0f, .vPerUnit = 0.1f});
// road.vertices: {position, normal, uv}; road.indices: список треугольников (CCW)
```

Полный пример: `samples/guide_examples/11-splines/mesh.cpp`.

Профиль (`ExtrusionProfile`) — 2D-сечение: x идёт вдоль binormal («вправо»), y — вдоль normal («вверх»). Готовые профили: `makeCircleProfile`, `makeRectangleProfile` (жёсткие рёбра), `makeStripProfile` (плоская лента лицом вверх). Свой профиль: замкнутые обходите против часовой стрелки (нормали наружу); продублируйте точку, чтобы получить жёсткое ребро; `u` задаёт текстурные координаты поперёк.

| `ExtrusionSettings` | По умолчанию | Смысл |
| --- | --- | --- |
| `spacing` | 1 | Шаг колец по длине (подгоняется, чтобы уложиться точно). |
| `vPerUnit` | 1 | Текстурная `v = distance * vPerUnit`. |
| `capStart`, `capEnd` | `false` | Крышки (только замкнутые выпуклые профили на открытых сплайнах). |

Размеры результата предсказуемы: колец `round(length / spacing) + 1`, столбцов — число точек профиля (+1 на шов UV у замкнутых). Пересобирайте меш, когда меняется `spline.version()`.

## Шаг 7. Отладочная отрисовка

```cpp
#include <oxwald/spline/spline_debug.hpp>

drawSpline(s, [&](glm::vec3 a, glm::vec3 b, glm::vec4 c) { debugDraw.line(a, b, c); },
           {.samplesPerSegment = 16, .frameSpacing = 2.0f});
```

Полный пример: `samples/guide_examples/11-splines/mesh.cpp`.

`drawSpline` рисует кривую, контрольные точки, хэндлы (для B-сплайнов/NURBS — контрольный многоугольник) и кадры: tangent синим, normal зелёным, binormal красным. Части доступны по отдельности: `drawCurve`, `drawControlPoints`, `drawHandles`, `drawFrames`.

## Типичные ошибки и подводные камни

- **Движение «по t».** `t += speed * dt` даёт неравномерную скорость. Используйте дистанцию (`distanceToT`, `evaluateAtDistance`) или `PathFollower`.
- **Хэндлы как абсолютные координаты.** `inHandle`/`outHandle` — смещения от точки; `setOutHandle(i, target - point.position)`.
- **Ожидание, что B-сплайн проходит через точки.** Он аппроксимирует; для прохождения через точки — Catmull-Rom или Bézier.
- **Чтение из нескольких потоков «грязного» сплайна.** Константные запросы лениво перестраивают кеш и не потокобезопасны после правки; вызовите `rebuild()` перед тем, как делиться сплайном.
- **Перекручивание труб и проводов** в режиме `UpVector` на вертикальных участках — переключитесь на `RotationMinimizing`.
- **Uniform Catmull-Rom на неравномерных точках** даёт петли и выбросы — оставьте centripetal (по умолчанию).
- **Маркеры после правки.** Маркеры хранятся по `t`; пересчитывает их только `insertPointAt` у Bézier, после других правок проверьте их положение.
- **Изломы у `Linear`.** На углах ломаной кадр меняется скачком, и экструзия даёт «перегибы» — для мешей берите гладкие типы.
- **Крышки у невыпуклого профиля** триангулируются веером из центра и будут неверны.
- **Поза follower'а в мировых координатах.** Она в пространстве сплайна; не забудьте трансформ владельца.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`spline.hpp`](../../engine/spline/include/oxwald/spline/spline.hpp) | `Spline`, `ControlPoint`, `SplineSettings`, `SplineSample`, `SplineMarker`, `ClosestPointResult`, `Bounds`, `frameRotation` |
| [`path_follower.hpp`](../../engine/spline/include/oxwald/spline/path_follower.hpp) | `PathFollower`, `FollowerSettings`, `LoopMode`, `PathEvent`, `FollowerPose` |
| [`spline_mesh.hpp`](../../engine/spline/include/oxwald/spline/spline_mesh.hpp) | `extrude`, `ExtrusionProfile`, `ExtrusionSettings`, `ExtrudedMesh`, `make*Profile` |
| [`spline_debug.hpp`](../../engine/spline/include/oxwald/spline/spline_debug.hpp) | `drawSpline`, `drawCurve`, `drawControlPoints`, `drawHandles`, `drawFrames`, `DebugDrawOptions` |
| [`bezier.hpp`](../../engine/spline/include/oxwald/spline/bezier.hpp) | `CubicBezier`, `bezierPosition`, `deCasteljau`, `splitBezier`, `hermiteToBezier` |

Заметки для разработчиков модуля (алгоритмы, точность, ограничения): [`docs/dev/modules/spline.md`](../dev/modules/spline.md).

## Что дальше

- [09. Физика](09-physics.md) — кинематические платформы, которые двигает `PathFollower` (через `moveKinematic`).
- [10. Анимация](10-animation.md) — персонажи, идущие по пути.
- [13. ИИ](13-ai.md) — патрули и маршруты NPC.
- [16. Открытый мир](16-world.md) — дороги и реки поверх ландшафта.
- [32. Компоненты ECS](32-gameplay-components.md) — компонент сплайна и следования по пути на сцене.
- [Оглавление](README.md).
