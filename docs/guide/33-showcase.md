# 33. Тестовый проект OxwaldShowcase

> Каталог [`samples/OxwaldShowcase/`](../../samples/OxwaldShowcase/) — это настоящий проект `.oxproj`, а не модуль движка. В нём три таргета: нативный игровой модуль `ox_showcase_game` (подключается к OxwaldPlayer и OxwaldEditor), генератор контента `oxshowcase_generate` (плюс таргет `oxshowcase_regenerate`) и тесты `ox_showcase_tests` (метка ctest `showcase`). Пространство имён C++ — `ox::showcase`. Проект собирается, только если сконфигурированы модули `runtime`, `gameplay`, `render`, `ui`, `assets`, `net`, `async`, `script` и `world`.

Это глава-экскурсия по проекту. Для каждой станции здесь указано, что на ней видно, какие компоненты и cvar'ы работают, в каких файлах всё сделано, и приведён короткий фрагмент кода из проекта. В конце показано, как добавить свою станцию. **Компилируемых примеров у главы нет**: весь код взят из `samples/OxwaldShowcase` (с сокращениями), и проверяют его тесты самого проекта ([раздел «Тесты»](#тесты)).

![Главное меню](images/showcase/main_menu.png)

## Зачем

| Задача | Что даёт OxwaldShowcase |
| --- | --- |
| Увидеть возможности движка вживую | Хаб с порталами и 14 станций, по одной на подсистему: от теней до сети и сохранений |
| Найти рабочий пример API | Каждая станция — короткий Lua-скрипт и функция генератора на C++, в которых используются нужные компоненты |
| Проверить сборку на своей машине | Скриншоты всех станций одной командой (`--tour --shots`), смоук-тест каждой сцены в ctest |
| Начать свою игру | Образец всего игрового «каркаса»: персонаж от третьего лица, меню RmlUi, настройки графики, переназначение клавиш, сохранения, нативный модуль игры |

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| Станция | Отдельная сцена `Assets/Scenes/Stations/NN_Имя.oxscene`, посвящённая одной подсистеме. Список станций хранится в `Assets/Data/stations.json` |
| Хаб | `Assets/Scenes/Hub.oxscene`: площадь, на которой по кругу стоят порталы на все станции |
| Инфо-стенд | Табличка на станции. Рядом с ней (и первые 9 секунд после прихода) в HUD показана панель с описанием, клавишами и ссылкой на главу руководства |
| Сущность `Game` | Есть в каждой сцене. На ней висит `Scripts/game.lua`, а в свойствах скрипта лежат тексты станции: `station`, `title`, `description`, `hints`, `guide` |
| Генератор | `oxshowcase_generate`. Строит все сцены, материалы, префабы, текстуры, звуки и манекен через API движка. Результат детерминирован |
| Игровой модуль `Showcase` | Нативный код проекта (`Source/`): Lua-таблица `showcase`, экскурсия, сетевое демо, автосохранение |
| Экскурсия (tour) | Камера по очереди облетает хаб и все станции и показывает подписи. Может снимать скриншоты |
| Фоторежим (photo) | Камера ставится в точку `TourCam.A` сцены. Так снимают одиночные кадры и прогоняют смоук-тесты |

Состав проекта:

```
OxwaldShowcase.oxproj   настройки проекта, ввод (действия Move/Look/Jump/Sprint/Interact/...), "modules": {"Showcase": true}
Assets/                 контент; всё, кроме Legacy/, Scripts/ и UI/, создаёт генератор
  Scenes/               MainMenu.oxscene, Hub.oxscene, Stations/01_Lighting.oxscene … 14_SaveGames.oxscene
  Scripts/              Lua: game.lua, player.lua, camera.lua, portal.lua, скрипты станций, lib/flow.lua …
  UI/                   RmlUi: hud.rml, main_menu.rml, pause.rml, settings.rml, saves.rml, dialogue.rml, side_panel.rml, *.rcss
  Data/stations.json    список станций (id, название, сцена, глава руководства)
  Materials/ Prefabs/ Chunks/ Textures/ Audio/ Models/ AI/    сгенерированные ассеты
  Legacy/               модели и текстуры из прошлых версий проекта (солдат, Suzanne, кожа, панорамы)
Source/                 игровой модуль: showcase_module.cpp, showcase_lua.cpp, tour.cpp, net_demo.cpp
Tools/                  генератор: generate_showcase.cpp, gen_common.cpp, gen_hub.cpp, gen_stations_*.cpp, gen_assets.cpp …
Tests/                  CMakeLists.txt (тесты сцен, экскурсии, генератора) и showcase_tests.cpp (gtest)
```

## Шаг 1. Сборка и запуск

```sh
cmake --preset dev -B build/showcase -DVCPKG_MANIFEST_INSTALL=OFF
cmake --build build/showcase --target OxwaldPlayer OxwaldEditor oxshowcase_generate
```

### Плеер

```sh
# игра в окне, начинается с главного меню (startupScene проекта)
build/showcase/bin/OxwaldPlayer --project samples/OxwaldShowcase

# сразу на нужную станцию
build/showcase/bin/OxwaldPlayer --project samples/OxwaldShowcase --scene project://Assets/Scenes/Stations/05_Water.oxscene

# экскурсия с гидом: камера облетает хаб и все станции с подписями
build/showcase/bin/OxwaldPlayer --project samples/OxwaldShowcase -- --tour

# экскурсия по одной станции
build/showcase/bin/OxwaldPlayer --project samples/OxwaldShowcase -- --tour --station water
```

Флаги до `--` разбирает движок ([глава 05](05-runtime.md#шаг-7-oxwaldplayer)). Всё, что идёт после `--`, попадает в `EngineConfig::gameArgs` и достаётся игровому модулю:

| Аргумент игры | Смысл |
| --- | --- |
| `--tour` | Экскурсия: хаб, затем станции в порядке `stations.json` |
| `--photo` | Фоторежим: камера `TourCam.A`, инфо-панель открыта, управление выключено |
| `--station <id>` | В экскурсии показать только станцию `<id>` (`lighting`, `water`, `rtx`, …) |
| `--shots <каталог>` | В экскурсии сохранить `<каталог>/<id>.png` для каждой станции (до показа подписи) |
| `--quit` / `--stay` | Выйти или остаться после экскурсии. В headless по умолчанию `--quit`, в окне — переход в хаб |

Экскурсию можно запустить и из главного меню: «Экскурсия с гидом (тур)».

### Скриншоты

Скриншоты в этой главе сняты так:

```sh
# все станции и хаб: docs/guide/images/showcase/<id>.png
build/showcase/bin/OxwaldPlayer --project samples/OxwaldShowcase --headless --frames 60000 \
    -- --tour --quit --shots docs/guide/images/showcase

# один кадр одной сцены (главное меню в фоторежиме показывает само меню)
build/showcase/bin/OxwaldPlayer --project samples/OxwaldShowcase --headless \
    --scene project://Assets/Scenes/MainMenu.oxscene --frames 150 --screenshot main_menu.png -- --photo
```

`--shots` работает только с headless-рендером: в окне экскурсия напишет `tour screenshots need the headless Vulkan renderer`. `--screenshot` — флаг самого плеера: он сохраняет последний из `--frames N` кадров. `--frames 60000` — просто верхняя граница, плеер выйдет раньше, когда экскурсия закончится (`--quit`).

### Редактор

```sh
build/showcase/bin/OxwaldEditor.app/Contents/MacOS/OxwaldEditor --project samples/OxwaldShowcase
```

![Хаб в OxwaldEditor](images/showcase/editor_hub.png)

Редактор открывает хаб: в `.oxproj` указано `editor.maps.editorStartupMap = "Scenes/Hub.oxscene"`. Игра же стартует с `startupScene` — главного меню. Модуль `Showcase` слинкован и в редактор, поэтому Play запускает те же Lua-скрипты, что и плеер. Не путайте этот проект с шаблоном **Showcase** в Project Browser ([глава 30](30-editor.md#шаг-1-запуск-и-проекты)) — шаблон создаёт один небольшой демо-уровень.

> Сцены, материалы и префабы проекта создаёт генератор. Если сохранить сгенерированную сцену из редактора, следующий запуск `oxshowcase_regenerate` перезапишет правку, а `Showcase.GeneratorCheck` до этого будет падать. Меняйте код генератора, а в редакторе только смотрите и экспериментируйте (или храните свои сцены отдельно).

## Шаг 2. Управление

| Действие | Клавиатура / мышь | Геймпад |
| --- | --- | --- |
| Ходьба, бег, прыжок | WASD, Shift, Space | левый стик, нажатие левого стика, A |
| Камера, приближение | мышь, колесо | правый стик |
| Действие (`Interact`) | E | X |
| Выстрел (станция «Физика») | ЛКМ | RT |
| Меню / пауза | Esc | Start |
| Быстрое сохранение / загрузка | F5 / F9 | — |
| Инфо-панель станции | I | Back |
| Отладочный оверлей ImGui | F1 или ~ | — |

Это действия контекста `OnFoot` из `OxwaldShowcase.oxproj` ([глава 06](06-input.md)). Клавиши станций (T, L, P, R, …) скрипты читают напрямую через `input.keyPressed` и в настройках не переназначаются. Остальные клавиши меняются в «Настройки → Управление» и сохраняются в `user://settings.json`.

## Шаг 3. Главное меню

![Главное меню](images/showcase/main_menu.png)

**Что показывает.** Сцена `MainMenu.oxscene`: полированный пол, кольцо сфер (хром, золото, стекло, медь, автоэмаль, эмиссия), колонны с тёплым светом, проба отражений и камера, которая медленно облетает сцену (`menu_camera.lua`). Поверх сцены — документ RmlUi `main_menu.rml` с кнопками «Продолжить», «Новая экскурсия», «Экскурсия с гидом (тур)», «Загрузить», «Настройки», «Выход».

**Движок.** `ReflectionProbeComponent` (`update = OnEnable`), `TimeOfDayComponent` на паузе (вечер, 18:36), `AudioSourceComponent` на шине `Music` с `fadeInSeconds`, модели данных RmlUi, модель `settings` движка.

**Файлы.** `Tools/gen_hub.cpp` (`buildMainMenu`), `Scripts/game.lua` (свойство `menu = true`), `Scripts/lib/flow.lua`, `Scripts/lib/settings_menu.lua`, `UI/main_menu.rml`, `UI/settings.rml`.

Кнопки меню — обычные обработчики `ui.on` в `lib/flow.lua`:

```lua
ui.on(flow.docs.menu, "continue", "click", function()
    local s = flow.newestSave()
    if s then flow.closeAll(); flow.inMainMenu = false; showcase.load(s.slot) end
end)
ui.on(flow.docs.menu, "guided", "click", function()
    flow.closeAll(); flow.inMainMenu = false
    showcase.startTour() -- C++ coroutines fly through every station (Source/tour.cpp)
end)
```

Экран настроек почти целиком привязан к модели данных `settings`, которую предоставляет движок (`UiSystem` вызывает `GameUI::bindSettingsMenu` над `Engine::settings()`):

```html
<body class="menu" data-model="settings">
    <button class="choice quality" data-for="q : qualities" data-attr-id="'quality-' + q"
            data-class-selected="q == quality" data-event-click="set_quality(q)">{{q}}</button>
    <input id="raytracing" type="checkbox" data-checked="rayTracing" data-attrif-disabled="!rtAvailable"/>
    <div class="row hint" data-if="!rtAvailable">{{rtReason}}</div>
```

Подробнее: [29. UI](29-ui.md#шаг-8-меню-настроек-графики-модель-settings), [26. Настройки качества графики](26-quality-settings.md).

## Шаг 4. Хаб

![Хаб](images/showcase/hub.png)

**Что показывает.** Площадь радиусом 32 м. По кругу стоят 14 порталов (у каждого светящаяся полоса и плита цвета своей станции), между ними фонари с тенями через один. В центре монумент: хромовая сфера в золотом кольце под пробой отражений. Здесь же персонаж от третьего лица (модель солдата из `Legacy/`) с камерой, которая не проходит сквозь стены, фоновые ветер и музыка.

**Движок.** `CharacterControllerComponent` + `player.lua` (движение относительно камеры, шаги), `camera.lua` (`physics.raycast` от точки опоры, чтобы камера не уходила за стены), портал — сенсорный `ColliderComponent` + `TriggerComponent` с `requiredTag = "Player"`, асинхронная смена уровня с автосохранением покидаемого уровня.

**Файлы.** `Tools/gen_hub.cpp` (`buildHub`), `Tools/gen_common.cpp` (`SceneBuilder::player`, `SceneBuilder::portal`), `Scripts/player.lua`, `Scripts/camera.lua`, `Scripts/portal.lua`, `Scripts/game.lua`.

Порталы генератор расставляет по списку станций. Поэтому новая станция сама получает портал в хабе (см. [шаг 7](#шаг-7-добавляем-свою-станцию)):

```cpp
const auto& st = stations();
const f32 radius = 24.0f;
for (usize i = 0; i < st.size(); ++i) {
    const f32 a = (f32(i) + 0.5f) / f32(st.size()) * 2 * kPi;
    const glm::vec3 pos(std::sin(a) * radius, 0, std::cos(a) * radius);
    const f32 yaw = glm::degrees(a) + 180.0f;
    Entity p = sb.portal("Portal." + st[i].id, st[i].uri(), st[i].id, st[i].color, pos, yaw);
    sb.prim("Plinth", Primitive::Cube, "Accent." + st[i].id, {0, 0.05f, 1.6f}, {2.6f, 0.1f, 1.2f}, {}, p);
}
```

Сам переход — `onTriggerEnter` в `portal.lua`:

```lua
function onTriggerEnter(self, other)
    if self.cooldown > 0 or showcase.mode() ~= "play" or showcase.loading() then return end
    if other.name ~= "Player" then return end
    flow.prompt("")
    flow.toast("Переход: " .. title(self), 2)
    audio.play("project://Assets/Audio/chime.wav", nil, 0.6)
    showcase.loadLevel(self.target)
end
```

`cooldown = 1.5` (секунды) не даёт сразу уйти обратно, если игрок появился рядом с порталом. `showcase.loadLevel` не меняет уровень прямо из скрипта: модуль ставит смену в очередь и выполняет её в начале следующего кадра (см. [«Игровой модуль»](#игровой-модуль-showcase)).

Подробнее: [05. Runtime и игровой цикл](05-runtime.md), [06. Ввод](06-input.md), [09. Физика](09-physics.md) (персонаж и триггеры), [15. Скрипты на Lua](15-scripting-lua.md).

## Шаг 5. Станции

На каждой станции есть игрок, инфо-стенд, портал обратно в хаб и две камеры экскурсии (`TourCam.A`, `TourCam.B`). Всё это создаёт один вызов `SceneBuilder::stationBasics` + `tourCameras`. Ниже описано только то, что отличает станции друг от друга.

### 1. Свет и тени

![Свет и тени](images/showcase/lighting.png)

**Что показывает.** Вечернее солнце с каскадными тенями: полосатая тень от перголы. Ниша со статуей Suzanne под двумя прожекторами (тени из атласа). Фонарь в клетке с точечным светом (кубические тени сквозь прутья). Игла и парящая сфера для PCSS: у основания тень резкая, дальше от заслоняющего объекта размывается. «Гирлянда» из 16×16 = 256 цветных точечных огней.

**Клавиши.** `[T]` (удерживать) — промотка времени суток, `[N]` — день/ночь, `[L]` — волна по гирлянде вкл/выкл, `[P]` — PCSS вкл/выкл.

**Движок.** `LightComponent` (Directional/Spot/Point, `castShadows`, `sourceRadius`), `TimeOfDayComponent` + `SkyComponent`, кластерный forward+. Cvar: `r.Shadows.PCSS`. Попробуйте `r.DebugView LightComplexity` в консоли — увидите тепловую карту огней по кластерам.

**Файлы.** `Tools/gen_stations_render.cpp` (`buildLighting`), `Scripts/light_garden.lua`, `Scripts/time_of_day.lua`.

Гирлянда в генераторе — обычный двойной цикл. Огни сделаны дочерними сущностями `LightGarden`, на которой висит скрипт:

```cpp
Entity garden = sb.create("LightGarden");
garden.setPosition({-1.5f, 0, -3.0f});
sb.script(garden, "Scripts/light_garden.lua");
for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x) {
        // ... hue → colour c
        const glm::vec3 p(-5.6f + f32(x) * 0.75f, 0.3f, -4.2f + f32(z) * 0.55f);
        Entity l = sb.pointLight("GardenLight", p, c, 320.0f, 2.4f, false, 0.02f, garden);
        sb.prim("Bulb", Primitive::Sphere, "Bulb." + std::to_string((x * 7 + z * 3) % 16), {0, 0, 0}, glm::vec3(0.1f), {}, l);
    }
```

Скрипт собирает дочерние `Light` и каждый кадр меняет их `intensity`. Тени переключаются через cvar:

```lua
if input.keyPressed("P") then
    local on = showcase.cvar("r.Shadows.PCSS") == "true"
    showcase.setCVar("r.Shadows.PCSS", not on)
    flow.toast(on and "PCSS выключен: жёсткие тени" or "PCSS включён: контактное упрочнение теней")
end
```

Подробнее: [20. Освещение и тени](20-lighting-shadows.md).

### 2. Материалы

![Материалы](images/showcase/materials.png)

**Что показывает.** Сетка PBR-сфер 7×5: шероховатость по горизонтали, металличность по вертикали. Кожаный диван и пуф (текстуры из `Legacy/`), автоэмаль, статуя солдата (текстурированный OBJ). Стекло с преломлением и поглощением по Беру–Ламберту (прозрачная и янтарная сферы, синий куб), матовое стекло перед неоновыми полосами. Alpha-test листва из перекрещенных карточек, светящиеся тотемы и картина на мольберте. Фон — LDR-кубмапа `park.oxcube`.

**Движок.** Материалы `.oxmat` (Opaque, AlphaTest, Refractive, emissive), `EnvironmentComponent::skybox` + `ldrSkyLuminance`, импорт OBJ с подассетами `#Mesh/0`, `#Material/N`. Клавиша `[V]` (скрипт `Scripts/material_debug.lua`) перебирает отладочные каналы `r.DebugView`: итог → альбедо → нормали → шероховатость → металличность; при уходе со станции режим сбрасывается. Те же каналы доступны из консоли: `r.DebugView Albedo`.

**Файлы.** `Tools/gen_stations_render.cpp` (`buildMaterials`), `Tools/gen_assets.cpp` (`generateMaterials`: `PBR.r*.m*`, `Glass*`, `FrostedGlass`, `Leather*`, …), `Assets/Materials/*.oxmat`, `Assets/Textures/Sky/park.oxcube`.

```cpp
// PBR grid: 7 roughness columns × 5 metallic rows on a dark stand.
for (int r = 0; r < 7; ++r)
    for (int m = 0; m < 5; ++m)
        sb.prim("PBRSphere", Primitive::Sphere, "PBR.r" + std::to_string(r) + ".m" + std::to_string(m),
                {-3.6f + f32(r) * 1.2f, 0.6f + f32(m) * 1.1f, -6.0f}, glm::vec3(0.9f));
// Glass: refractive spheres with Beer–Lambert absorption, frosted glass pane in front of neon stripes.
sb.prim("GlassSphere", Primitive::Sphere, "Glass", {3.0f, 0.75f, 1.2f}, glm::vec3(1.5f));
sb.prim("AmberSphere", Primitive::Sphere, "Glass.Amber", {4.8f, 0.55f, 3.2f}, glm::vec3(1.1f));
sb.prim("FrostedPane", Primitive::Cube, "FrostedGlass", {8.5f, 1.3f, 2.2f}, {3.2f, 2.4f, 0.08f});
```

Подробнее: [19. Материалы](19-materials.md), [31. Ассеты](31-assets.md) (импорт OBJ и подассеты).

### 3. Отражения и GI

![Отражения и GI](images/showcase/reflections.png)

**Что показывает.** Зал с чёрным полированным полом: в нём SSR отражают колонны, хромовый и золотой шары и светящиеся кубы. Проба отражений зала с box projection. Плоское зеркало на задней стене. GTAO в углу с ящиками. Справа — «Cornell box»: через объём освещённости красная и зелёная стены окрашивают белый пол и кубы.

**Клавиши.** `[R]` — SSR вкл/выкл, `[G]` — GTAO вкл/выкл.

**Движок.** `ReflectionProbeComponent` (`boxProjection`, `resolution`, `update = OnEnable`), `PlanarReflectorComponent`, `IrradianceVolumeComponent`. Cvar'ы: `r.SSR`, `r.AO.Method` (0 — выкл, 2 — GTAO).

**Файлы.** `Tools/gen_stations_render.cpp` (`buildReflections`), `Scripts/render_toggles.lua`.

```cpp
Entity reflector = sb.create("MirrorPlane");
reflector.setPosition({2.0f, 2.6f, -16.86f});
reflector.setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0)));
auto& pr = reflector.add<render::PlanarReflectorComponent>();
pr.size = {3.5f, 2.0f};
pr.maxRoughness = 0.1f;
// ...
auto& iv = vol.add<render::IrradianceVolumeComponent>();   // Cornell box
iv.extents = {4.0f, 2.5f, 4.0f};
iv.probeCount = {6, 5, 6};
```

```lua
if input.keyPressed("G") then
    local m = tonumber(showcase.cvar("r.AO.Method") or "2")
    showcase.setCVar("r.AO.Method", m == 0 and 2 or 0)
    flow.toast(m == 0 and "GTAO включён" or "AO выключен")
end
```

Подробнее: [21. Отражения и GI](21-reflections-gi.md).

### 4. Волюметрика

![Волюметрика](images/showcase/volumetrics.png)

**Что показывает.** Неф собора. В западной стене узкие высокие окна смотрят на вечернее солнце, а восточная стена и крыша глухие, поэтому в дымке видны отдельные лучи света. Высотный froxel-туман, туман у пола, две светящиеся «духовные сферы» (локальные объёмы с эмиссией), свечи у алтаря. Снаружи объёмные облака.

**Клавиши.** `[T]` (удерживать) — промотка времени, `[N]` — день/ночь, `[C]` — непрерывный цикл дня и ночи (у этой станции у скрипта включено свойство `dayNight`).

**Движок.** `EnvironmentComponent` (`fogEnabled`, `fogDensity`, `fogHeightFalloff`), `VolumetricFogComponent` (`anisotropy`, `directionalIntensity`), `FogVolumeComponent` (Box/Sphere, шум, эмиссия), `CloudLayerComponent`, `TimeOfDayComponent`.

**Файлы.** `Tools/gen_stations_render.cpp` (`buildVolumetrics`), `Scripts/time_of_day.lua`.

```cpp
Entity vf = sb.create("VolumetricFog");
auto& v = vf.add<render::VolumetricFogComponent>();
v.anisotropy = 0.6f;
v.directionalIntensity = 8.0f;
v.ambientIntensity = 0.05f; // the sky's ambient is not occluded inside the nave: keep it low so shafts dominate
// ...
fog("SpiritOrb", {-2.5f, 2.2f, -18.0f}, {1.4f, 1.4f, 1.4f}, render::FogVolumeShape::Sphere, 0.8f, {0.3f, 0.6f, 1.0f}, {0.05f, 0.25f, 0.6f}, 0.5f);
```

Обратите внимание на `ambientIntensity = 0.05`: освещение от неба внутри нефа ничем не затеняется. Если его не приглушить, туман засветится равномерно, и лучей не будет видно.

Подробнее: [22. Волюметрика](22-volumetrics.md), [28. Рендеринг открытого мира](28-world-rendering.md) (небо и смена дня и ночи).

### 5. Вода и частицы

![Вода и частицы](images/showcase/water.png)

**Что показывает.** Бухта с волнами Герстнера и каустикой на песчаном дне. Если зайти в воду по пандусу, включается подводный режим. На волнах качаются физические ящики и бочка. На берегу костёр: аддитивный огонь, освещённый дым с сортировкой и искры (растянутые billboard'ы, отскакивают по буферу глубины). Свет костра мерцает (`flicker.lua`).

**Клавиши.** `[B]` — сбросить в воду ещё один ящик.

**Движок.** `gameplay::WaterComponent` (физика волн) + `render::WaterSurfaceComponent` (рендер, `causticsIntensity`), `BuoyancyComponent`, `RigidBodyComponent`, `ParticleEmitterComponent` (`blend`, `renderMode`, `collision`, `sizeOverLife`, `colorOverLife`), префаб `Prefabs/FloatingCrate.oxprefab`, `scene.spawn` из Lua.

**Файлы.** `Tools/gen_stations_render.cpp` (`buildWater`), `Tools/gen_assets.cpp` (`generatePrefabs`), `Scripts/crate_spawner.lua`, `Scripts/flicker.lua`.

```cpp
auto& w = water.add<gameplay::WaterComponent>();
w.size = {1600.0f, 1600.0f};
w.windDirection = {0.7f, -0.7f};
w.windSpeed = 5.5f;
w.waveCount = 6;
w.steepness = 0.45f;
auto& ws = water.add<render::WaterSurfaceComponent>();
ws.size = {1600.0f, 1600.0f};
ws.causticsIntensity = 1.4f;
// Floating crates (buoyancy through gameplay::BuoyancyComponent + physics).
Entity crate = sb.body("FloatingCrate", Primitive::Cube, "Crate", p, glm::vec3(1.0f + 0.2f * f32(i % 2)), 180.0f, yawRotation(f32(i) * 23));
crate.add<gameplay::BuoyancyComponent>().halfExtents = glm::vec3(0.5f);
```

```lua
if input.keyPressed("B") then
    local p = self.entity.transform.worldPosition + vec3(math.random() * 6 - 3, 0, math.random() * 6 - 3)
    local crate = scene.spawn("FloatingCrate", p, quat.fromEuler(vec3(math.random(), math.random(), math.random())))
    if crate then flow.toast("Ящик сброшен — плавучесть считает погружённый объём") end
end
```

Подробнее: [23. Прозрачность, вода и частицы](23-transparency-water-particles.md), [09. Физика](09-physics.md).

### 6. Открытый мир

![Открытый мир](images/showcase/world.png)

**Что показывает.** Процедурный ландшафт 768×768 м (ridged simplex с искажением координат и гидравлической эрозией) с четырьмя слоями сплатов: трава, скалы на склонах, песок в низинах, снег на вершинах. Сосны с импосторами вдали, кусты и трава колышутся на ветру. Вокруг игрока по сетке 8×8 подгружаются чанки-префабы `Chunks/chunk_x_z.oxprefab`; в каждом стоит высокий светящийся маяк, поэтому подгрузку видно издалека. Инфо-стенд и портал ставятся на поверхность ландшафта скриптом `snap_to_terrain.lua`.

**Клавиши.** `[F]` — отладочный оверлей (выполняет консольную команду `ui.debug`). Отладку стриминга и LOD ландшафта смотрите там.

**Движок.** `TerrainComponent` (`Procedural`, `noise`, `hydraulicErosion`, `splatRules`, `lod`) + `TerrainRenderComponent`, `VegetationComponent` (слои Tree/Detail/Grass, `impostorDistance`, `cullDistance`), `WindComponent`, `WorldStreamingComponent` (`prefabPattern`) + `StreamingSourceComponent` на игроке, `world.terrainHeight` в Lua.

**Файлы.** `Tools/gen_stations_gameplay.cpp` (`buildWorld`), `Scripts/world_station.lua`, `Scripts/snap_to_terrain.lua`, `Scripts/player.lua` (свойство `snapToTerrain`).

```cpp
t.hydraulicErosion = true;
t.hydraulic.droplets = 60000;
t.layers = {gen.mat("Terrain.grass"), gen.mat("Terrain.rock"), gen.mat("Terrain.sand"), gen.mat("Terrain.snow")};
t.splatRules = {
    {.layer = 2, .maxHeight = -8.0f, .heightBlend = 4.0f},
    {.layer = 1, .minSlopeDeg = 32.0f, .maxSlopeDeg = 90.0f, .slopeBlendDeg = 6.0f, .noiseAmount = 0.3f},
    {.layer = 3, .minHeight = 55.0f, .heightBlend = 10.0f, .maxSlopeDeg = 40.0f, .noiseAmount = 0.4f},
};
// Streaming: survey markers in chunk prefabs (Assets/Chunks/chunk_x_z.oxprefab) load around streaming sources.
auto& ws = streaming.add<gameplay::WorldStreamingComponent>();
ws.settings.chunkSize = 96.0f;
ws.settings.loadRadius = 150.0f;
ws.settings.unloadRadius = 200.0f;
ws.prefabPattern = "Chunks/chunk_{x}_{z}";
```

Подробнее: [16. Открытый мир](16-world.md), [28. Рендеринг открытого мира](28-world-rendering.md).

### 7. Физика

![Физика](images/showcase/physics.png)

**Что показывает.** Три башни из брусков, кладка «крест-накрест». Россыпь мячей. Три цепи из капсул на шарнирах, на средней висит тяжёлый шар. Ворота на петле с мотором, качели с ограничением угла. Зелёная площадка-триггер подбрасывает всё, что на неё попадает. Мишени для лучевого ружья.

**Клавиши.** `[ЛКМ]` или `[E]` — выстрел, `[R]` — собрать башни заново.

**Движок.** `RigidBodyComponent`, `ColliderComponent`, `JointComponent` (`Point`, `Hinge`, `motorMode`, `limitsEnabled`), `TriggerComponent` + `onTriggerEnter`, `physics.raycast`, `entity.body:addImpulse`, `body:teleport`. Бруски башен помечены `SaveGameComponent`, поэтому F5/F9 сохраняют и их положение.

**Файлы.** `Tools/gen_stations_gameplay.cpp` (`buildPhysics`), `Scripts/raygun.lua`, `Scripts/launch_pad.lua`, `Scripts/physics_station.lua`.

Лучевое ружьё целиком:

```lua
function onUpdate(self, dt)
    if showcase.mode() ~= "play" or flow.blocking() or not self.cam then return end
    if input.triggered("Fire") or input.triggered("Interact") then
        local t = self.cam.transform
        local origin = t.worldPosition
        local hit = physics.raycast(origin, t.forward, self.range, scene.find("Player"))
        audio.play("project://Assets/Audio/raygun.wav", origin, 0.7)
        if hit and hit.entity and hit.entity:has("RigidBody") then
            local rb = hit.entity:get("RigidBody")
            if rb.motionType == "Dynamic" then
                local m = rb.mass > 0 and rb.mass or 20
                hit.entity.body:addImpulse(t.forward * (self.impulse * m), hit.point)
            end
        end
    end
end
```

Ворота с мотором в генераторе:

```cpp
auto& hinge = gate.add<gameplay::JointComponent>();
hinge.type = physics::ConstraintType::Hinge;
hinge.target = post.ref();
hinge.anchor = {-0.5f, 0, 0};
hinge.targetAnchor = {0.5f, 0, 0};
hinge.axis = {0, 1, 0};
hinge.motorMode = physics::MotorMode::Velocity;
hinge.motorTarget = 0.8f;
hinge.motorMaxForce = 400.0f;
```

Подробнее: [09. Физика](09-physics.md).

### 8. Анимация

![Анимация](images/showcase/animation.png)

**Что показывает.** Скиннинговый манекен. Генератор создаёт его процедурно и пишет в `Models/Mannequin.gltf` вместе со скелетом и клипами Idle/Walk/Run. Один манекен ходит по кругу: его скорость управляет 1D blend space. Второй бегает по большому овалу. Синий манекен стоит на ступенях: two-bone IK ставит стопы на разные ступени, aim IK поворачивает голову к игроку. Трое «наблюдателей» тоже провожают игрока взглядом.

**Клавиши.** `[1]` — стоять, `[2]` — шаг, `[3]` — бег (для манекена на кругу).

**Движок.** `AnimatorComponent` (встроенный контроллер, `blendParameter`, `blendSamples`, `animateInEditMode`), `SkinnedMeshComponent`, `IKComponent` (`TwoBone`, `Aim`), `SplineFollowerComponent`, `entity.animator:setFloat` в Lua.

**Файлы.** `Tools/gen_stations_gameplay.cpp` (`buildAnimation`, `mannequin`), `Tools/gen_mannequin.cpp`, `Scripts/locomotion.lua`.

```cpp
auto& an = root.add<gameplay::AnimatorComponent>();
an.skeleton = a.skeleton;
auto& c = an.inlineController;
c.parameters = {{"speed", anim::ParamType::Float, speedParam}};
// One 1D blend space state: idle (0 m/s) → walk (1.4 m/s) → run (4.5 m/s) weighted by "speed".
gameplay::AnimatorStateDesc loco;
loco.name = "Locomotion";
loco.blendParameter = "speed";
loco.blendSamples = {{a.idle, 0.0f}, {a.walk, 1.4f}, {a.run, 4.5f}};
c.states = {loco};
c.defaultState = "Locomotion";
// ...
ik.chains.push_back({.type = gameplay::IKChainType::TwoBone, .rootJoint = "UpperLeg.L", .midJoint = "LowerLeg.L", .endJoint = "Foot.L",
                     .target = footL.ref(), .poleOffset = {0, 0, -1}});
```

```lua
self.speed = math.lerp(self.speed, self.target, math.min(1, dt * 1.5))
self.follower.speed = self.speed
self.follower.playing = self.speed > 0.05
if self.entity.animator then self.entity.animator:setFloat("speed", self.speed) end
```

Подробнее: [10. Анимация](10-animation.md).

### 9. ИИ

![ИИ](images/showcase/ai.png)

**Что показывает.** Арена с укрытиями и навмешем, который строится при старте. Три охранника с деревом поведения ходят по маршрутам. Заметив игрока, они бегут за ним; потеряв из виду, идут к последней известной точке, осматриваются и возвращаются к патрулю. Состояние видно по цвету «глаза»: зелёный — патруль, красный — погоня, жёлтый — поиск. Полупрозрачный конус показывает поле зрения. В экскурсии и фоторежиме включена отладочная отрисовка ИИ (на скриншоте видны навмеш и сектора восприятия).

**Клавиши.** `[F]` — отладка навмеша, путей и восприятия.

**Движок.** `NavMeshSurfaceComponent` (`bakeOnStart`, `onlyChildren`), `NavAgentComponent`, `BehaviorTreeComponent` + `AI/guard.oxbt`, `PerceptionComponent` (`team`, `sight.range`, `fovDegrees`, `forgetAfter`), узлы `ScriptAction`, колбэки `onTargetSensed` / `onTargetLost`, `ai.blackboardSet`.

**Файлы.** `Tools/gen_stations_gameplay.cpp` (`buildAI`), `Tools/gen_assets.cpp` (`generateData` — дерево `guard.oxbt`), `Scripts/guard.lua`, `Scripts/ai_station.lua`.

Ветка поиска в `guard.oxbt` (условие на ключ blackboard'а с прерыванием `Both`):

```json
{
  "type": "BlackboardCondition", "name": "Searching", "key": "searching", "op": "Equals", "value": true,
  "abort": "Both",
  "child": {
    "type": "Sequence", "name": "Search",
    "children": [
      { "type": "ScriptAction", "function": "onSearch" },
      { "type": "MoveTo", "key": "lastKnown", "acceptance": 1.0 },
      { "type": "ScriptAction", "function": "lookAround" },
      { "type": "Wait", "seconds": 2.5 },
      { "type": "ScriptAction", "function": "giveUp" }
    ]
  }
}
```

Lua включает поиск, когда восприятие теряет цель. Узлы `ScriptAction` вызывают функции скрипта по имени:

```lua
function onTargetLost(self, source, sense)
    if self.lastSeen then
        ai.blackboardSet(self.entity, "lastKnown", self.lastSeen)
        ai.blackboardSet(self.entity, "searching", true)
    end
end

function giveUp(self)
    ai.blackboardSet(self.entity, "searching", false)
    return true
end
```

Навмеш строится только из детей сущности `NavMesh` (`onlyChildren = true`). Бесконечная «дальняя земля» под станцией иначе раздула бы границы навмеша.

Подробнее: [13. ИИ](13-ai.md).

### 10. Сплайны и корутины

![Сплайны и корутины](images/showcase/splines.png)

**Что показывает.** Поезд из локомотива и двух вагонов едет по замкнутому Catmull-Rom сплайну с постоянной скоростью. Шпалы и рельсы генератор расставил, сэмплируя ту же кривую. У платформы на маркере `Station` поезд тормозит, гудит и трогается. Дверь хранилища, лифт и диалог со смотрителем написаны корутинами на Lua: сценарий читается сверху вниз, без флагов состояния.

**Клавиши.** `[E]` у пульта — открыть хранилище, поднять/опустить лифт, поговорить со смотрителем. В диалоге варианты выбираются щелчком или `[1]–[3]`.

**Движок.** `SplineComponent` (`CatmullRom`, `closed`, `markers`), `SplineFollowerComponent` (`speed`, `startDistance`, `forwardAxis`), колбэк `onSplineEvent`, `spline::Spline::evaluateAtDistance` в генераторе, `spawn`/`wait`/`await(scene.delay(...))`/`await(scene.nextFrame())`, кинематические тела (`MotionType::Kinematic`).

**Файлы.** `Tools/gen_stations_gameplay.cpp` (`buildSplines`), `Scripts/train.lua`, `Scripts/door_sequence.lua`, `Scripts/elevator.lua`, `Scripts/dialogue.lua`, `Scripts/lib/interact.lua`, `UI/dialogue.rml`.

Дверь хранилища — одна корутина:

```lua
local function sequence(self)
    self.busy = true
    flow.toast("Корутина: тревога → разблокировка → дверь")
    for i = 1, 6 do
        self.alarm.intensity = (i % 2 == 1) and 4000 or 0
        await(scene.delay(0.25))
    end
    self.alarm.intensity = 0
    audio.play("project://Assets/Audio/door.wav", self.closed, 0.9)
    tween(self.door, self.closed, self.closed + vec3(0, 3.1, 0), 2.0)
    await(scene.delay(4.0))
    audio.play("project://Assets/Audio/door.wav", self.closed, 0.9)
    tween(self.door, self.closed + vec3(0, 3.1, 0), self.closed, 1.6)
    self.busy = false
end

function onUpdate(self, dt)
    if self.busy then return end
    interact.update(self, 2.2, "Открыть хранилище", function() spawn(function() sequence(self) end) end)
end
```

Остановка поезда — корутина, которую запускает событие маркера сплайна:

```lua
function onSplineEvent(self, name, distance)
    if name ~= "Station" or self.stopping then return end
    self.stopping = true
    spawn(function()
        for i = 1, 30 do setSpeed(self, self.cruise * (1 - i / 30)); wait(0.05) end
        setSpeed(self, 0)
        audio.play("project://Assets/Audio/chime.wav", self.entity.transform.worldPosition, 0.8)
        wait(3.0)
        for i = 1, 40 do setSpeed(self, self.cruise * i / 40); wait(0.05) end
        self.stopping = false
    end)
end
```

Подробнее: [11. Сплайны](11-splines.md), [08. Корутины](08-coroutines.md), [15. Скрипты на Lua](15-scripting-lua.md#шаг-6-корутины-и-таймеры-скрипта).

### 11. Звук

![Звук](images/showcase/audio.png)

**Что показывает.** Гудящая машина за толстой стеной: если обойти стену, слышно, как работает окклюзия и фильтр по расстоянию. Бетонный тоннель с зоной реверберации: внутри капель на шине `Cave` плавно появляется эхо. Башня с колонками играет музыку, кольца пульсируют по уровню шины `Music`. Каждые 20 секунд (или по `[E]` у жёлтой кнопки) звучит объявление на шине `Voice`, и музыка приглушается (ducking). Вокруг площади летает пищащий дрон, у него слышен эффект Доплера. В экскурсии включена отладка звука: окружности min/max дистанций и линии до слушателя.

**Клавиши.** `[E]` у кнопки — объявление.

**Движок.** `AudioSourceComponent` (`bus`, `spatial`, `occlusion`, `distanceLowPass`, `dopplerFactor`, `minDistance`/`maxDistance`), `AudioListenerComponent` на камере игрока, шины и эффекты `audio::AudioEngine` (`ReverbEffect`, `addDucking`) через Lua-таблицу `showcase.audio`, триггер `onTriggerEnter`/`onTriggerExit`.

**Файлы.** `Tools/gen_stations_gameplay.cpp` (`buildAudio`), `Tools/gen_textures_audio.cpp` (синтезированные WAV), `Scripts/audio_station.lua`, `Scripts/reverb_zone.lua`, `Source/showcase_lua.cpp` (`showcase.audio.*`).

```lua
function onStart(self)
    showcase.audio.reverb("Cave", 0.0, 0.9)      -- created dry; the tunnel trigger raises the wet level
    showcase.audio.duck("Voice", "Music", 0.2)
    -- ...
end
```

```lua
-- reverb_zone.lua
function onTriggerEnter(self, other)
    if other.name == "Player" then self.target = self.wet; flow.toast("Зона реверберации: шина Cave") end
end

function onUpdate(self, dt)
    if math.abs(self.current - self.target) > 0.01 then
        self.current = math.lerp(self.current, self.target, math.min(1, dt * 3))
        showcase.audio.reverb("Cave", self.current, 0.9)
    end
end
```

На стороне C++ `showcase.audio.duck` — это одна строка поверх `AudioEngine`:

```cpp
engine->addDucking({.sidechainBus = sidechain, .targetBus = target, .threshold = 0.01f,
                    .duckVolume = duckVolume.value_or(0.25f)});
```

Подробнее: [12. Звук](12-audio.md).

### 12. Сеть

![Сеть](images/showcase/network.png)

**Что показывает.** Listen-сервер прямо в игре. Мир станции — это сервер. Внутри того же процесса к нему подключается бот-клиент со своим `World` через транспорт в памяти с задержкой, джиттером и потерями пакетов. Реплицируются игрок, бот (бегает «восьмёркой») и шесть физических ящиков. Полупрозрачные голубые «призраки» — то, что видит клиент после репликации и интерполяции. На панели справа: RTT, тикрейт, размер снапшота, трафик, отставание призрака в сантиметрах.

**Клавиши.** `[=]`/`[−]` (или `+`/`−` на цифровом блоке) — задержка ±40 мс, `[L]` — потери пакетов 0 → 5 → … → 20 %.

**Движок.** `NetworkIdentityComponent` (`netType` = имя префаба на клиенте: `NetAvatar`, `NetBot`, `NetCrate`), `NetworkTransformComponent`, `gameplay::NetworkRuntime` (`startServer`, `connect`), `net::MemoryNetwork` + `LinkConditions`, второй `SystemScheduler` с урезанным `GameplayConfig` для клиента.

**Файлы.** `Tools/gen_stations_gameplay.cpp` (`buildNetwork`), `Tools/gen_assets.cpp` (префабы `NetAvatar`, `NetBot`, `NetCrate`), `Source/net_demo.cpp`, `Scripts/net_panel.lua`, `Scripts/bot.lua`, `UI/side_panel.rml`.

Запуск демо в `net_demo.cpp`: клиенту хватает сетевой системы, физика, анимация и скрипты ему не нужны.

```cpp
gameplay::GameplayConfig cfg;
cfg.physics = false;
cfg.animation = false;
cfg.scripting = false;
// ... (splines, audio, ai, coroutines, world, prediction — тоже false)
cfg.createEventBus = true;
m_client->assets.registerIn(m_client->services);
addGameplaySystems(m_client->scheduler, m_client->services, cfg);
m_client->scheduler.attach(m_client->world, m_client->services);
m_client->scheduler.setPlaying(true);

net::NetServerConfig sc;
sc.tickRate = 30.f;
if (!serverRt->startServer(m_client->network.createTransport(), sc)) return false;
auto& clientRt = m_client->services.get<gameplay::NetworkRuntime>();
if (!clientRt.connect(m_client->network.createTransport(), "memory", serverRt->server()->port())) return false;
```

Демо стартует только в сценах, где есть сущность `NetDemo`, через несколько кадров после смены уровня (к этому моменту gameplay-системы уже подключены к новому миру). При выгрузке мира оно останавливается в `onWorldUnloading`.

Подробнее: [14. Сеть](14-networking.md).

### 13. RTX и апскейлеры

![RTX и апскейлеры](images/showcase/rtx.png)

**Что показывает.** Шоурум: машинка из блоков автоэмали на поворотном диске, стеклянная и зеркальная сферы, зеркальная и красная стены, неоновые полосы. Справа панель. На ней переключатели трассировки лучей; если эффект недоступен на этом GPU, он показан серым с причиной. Ниже выбор апскейлера Off/FSR1/TAAU/DLSS с доступностью, внутреннее и выходное разрешение и самые дорогие проходы render graph по GPU-времени. На скриншоте (Apple M4 Pro, MoltenVK) трассировка лучей и DLSS недоступны, причины выведены прямо на панели.

**Клавиши.** `[Tab]` — панель, `[R]` — трассировка лучей целиком, `[1]–[5]` — RT-тени, RT-отражения, RT GI (DDGI), RT AO, path tracer, `[U]` — следующий доступный апскейлер, `[`/`]` — масштаб рендера 50/67/75/100 %.

**Движок.** Cvar'ы `r.RayTracing`, `r.RayTracing.Shadows`, `r.RayTracing.Reflections`, `r.RayTracing.GI`, `r.RayTracing.AO`, `r.PathTracing`, `r.Upscaler`, `r.ScreenPercentage`. `render::rt::rayTracingStatus(caps)` возвращает доступность и причину по каждому эффекту, `RenderStats::passes` — GPU-тайминги проходов. Всё это передаётся в Lua через `showcase.renderInfo()` и `showcase.gpuPasses(n)`.

**Файлы.** `Tools/gen_stations_render.cpp` (`buildRtx`), `Scripts/rtx_panel.lua`, `Source/showcase_lua.cpp` (`renderInfo`, `gpuPasses`), `UI/side_panel.rml`.

Переключатель проверяет доступность, прежде чем менять cvar, и объясняет отказ:

```lua
for _, e in ipairs(effects) do
    if input.keyPressed(e.key) then
        local ok, why = availability(info, e.rt)
        if ok then
            local on = showcase.cvar(e.cvar)
            showcase.setCVar(e.cvar, on ~= "true" and on ~= "1")
        else
            flow.toast(e.label .. " недоступно: " .. (why or ""), 4)
        end
    end
end
```

`showcase.setCVar` выполняет строку `имя значение` через консоль движка, а не пишет значение в реестр напрямую. Так движок реагирует ровно как на команду, набранную руками, и настройки графики применяются сразу. На RTX-видеокарте переключатели работают на лету, без перезапуска.

Подробнее: [24. Трассировка лучей](24-ray-tracing.md), [25. Апскейлеры и постобработка](25-upscalers-postprocess.md), [27. GPU-driven рендеринг и производительность](27-gpu-driven-performance.md).

### 14. Сохранения

![Сохранения](images/showcase/saves.png)

**Что показывает.** Две точки сохранения (кристаллы на постаментах), шесть ящиков, которые можно растолкать, три фонаря с выключателями (средний сначала выключен) и терминал с браузером слотов. Сохраните игру, растолкайте ящики и переключите лампы, затем загрузитесь: позиции ящиков и состояние ламп вернутся. В HUD на пару секунд появляется индикатор «Сохранено» / «Автосохранение…».

**Клавиши.** `[F5]` — быстрое сохранение, `[F9]` — быстрая загрузка, `[E]` у точки — сохранить в её слот (`savepoint_west` / `savepoint_east`), `[E]` у выключателя — лампа, `[E]` у терминала — браузер слотов (загрузка, удаление, новое ручное сохранение).

**Движок.** `SaveGameComponent` (сущность сохраняется целиком), `SaveGameSystem` (`listSlots`, `exists`, `deleteSlot`, `autosave`, `playTime`), `Engine::saveGame` / `loadGame`, `SaveGameConfig::autosaveOnLevelChange`.

**Файлы.** `Tools/gen_stations_gameplay.cpp` (`buildSaves`), `Scripts/save_point.lua`, `Scripts/switch.lua`, `Scripts/terminal.lua`, `Scripts/lib/flow.lua` (браузер слотов, F5/F9), `Source/showcase_module.cpp` (отложенные сохранения и автосохранение), `UI/saves.rml`.

Что попадает в сохранение, решает генератор:

```cpp
for (int i = 0; i < 6; ++i) {
    Entity c = sb.body("SavedCrate", Primitive::Cube, i % 2 ? "Crate" : "Orange", {-3.0f + f32(i % 3) * 1.5f, 0.5f + f32(i / 3) * 1.05f, -2.0f}, glm::vec3(1.0f), 20.0f);
    c.add<SaveGameComponent>();
}
// ...
Entity lamp = sb.pointLight("SavedLamp", p + glm::vec3(0, 3.1f, 0), {1.0f, 0.75f, 0.4f}, i == 1 ? 0.0f : 2500.0f, 10.0f, false);
lamp.add<SaveGameComponent>();
Entity sw = sb.block("Switch", "Hazard", p + glm::vec3(1.2f, 0.6f, 1.0f), {0.4f, 1.2f, 0.4f});
sb.script(sw, "Scripts/switch.lua");
sb.prop(sw, "lamp", lamp.uuid().toString());
```

Выключатель находит лампу по UUID (`scene.find(self.lamp)`). Генератор строит UUID детерминированно (см. [шаг 6](#генератор)), поэтому после загрузки связь не рвётся.

```lua
-- save_point.lua
function onUpdate(self, dt)
    interact.update(self, 2.2, "Сохранить в точке «" .. self.slot .. "»", function()
        showcase.save(self.slot, "Точка сохранения · " .. (self.slot:find("east") and "восток" or "запад"))
        audio.play("project://Assets/Audio/chime.wav", self.entity.transform.worldPosition, 0.7)
        flow.toast("Сохранено в слот " .. self.slot)
    end)
end
```

Подробнее: [07. Сохранения](07-savegames.md).

## Шаг 6. Как устроен проект

### Генератор

`Tools/generate_showcase.cpp` и файлы `gen_*.cpp` строят весь контент через API движка: `World`, `Entity`, компоненты, `serializeWorld`, `createPrefab`, `assets::materialToJson`. Порядок такой:

1. Процедурные текстуры, синтезированные звуки (WAV) и скиннинговый манекен в glTF (`gen_textures_audio.cpp`, `gen_mannequin.cpp`).
2. `.meta` для всех импортируемых файлов без метаданных (`scanAndMetaEverything`).
3. Материалы `Assets/Materials/*.oxmat`, файл проекта `OxwaldShowcase.oxproj`, префабы, `Data/stations.json` и дерево поведения `AI/guard.oxbt`.
4. Сцены: главное меню, хаб и 14 станций.

**Стабильные UUID.** Все идентификаторы выводятся из имён через `Uuid::fromName`, поэтому повторный запуск даёт побайтно те же файлы:

```cpp
static Uuid materialId(std::string_view name) { return Uuid::fromName("showcase.material." + std::string(name)); }
static Uuid assetId(std::string_view relPath) { return Uuid::fromName("showcase.asset." + std::string(relPath)); }

Entity SceneBuilder::create(std::string_view name, Entity parent) {
    const Uuid id = Uuid::fromName("showcase.entity." + sceneName + "." + std::to_string(m_counter++) + "." + std::string(name));
    return world.createWithId(id, name, parent);
}
```

В UUID сущности входят имя сцены, порядковый номер создания и имя. Если вставить сущность в середину функции, UUID всех следующих сущностей этой сцены изменятся. Для генератора это не страшно, но старые сохранения этой сцены перестанут совпадать с новой версией. Для префабов (`stablePrefab`) идентификаторы внутри документа тоже заменяются на детерминированные, потому что `createPrefab` выдаёт случайные. Существующие `.meta` генератор не трогает (их UUID сохраняются) и никогда не удаляет «осиротевшие» метаданные.

**Файл пишется только при изменении.** `Gen::writeAsset` сравнивает новое содержимое с файлом на диске. Отсюда режим проверки:

```sh
cmake --build build/showcase --target oxshowcase_regenerate          # = oxshowcase_generate --project samples/OxwaldShowcase
build/showcase/bin/oxshowcase_generate --check                       # ничего не пишет; код 1 и список "differs: …", если есть расхождения
build/showcase/bin/oxshowcase_generate --only water                  # только одна сцена: menu, hub или id станции
```

После `--check` выводится `oxshowcase_generate --check: 0 differences, N files up to date`. Это и проверяет тест `Showcase.GeneratorCheck`: файлы в репозитории должны совпадать с тем, что выдаёт генератор.

**`SceneBuilder`** (`Tools/gen.hpp`) убирает повторяющийся код. Основные методы: `prim`/`mesh` (меш с материалом), `block` (куб со статическим коллайдером), `body` (динамическое тело), `sun`/`pointLight`/`spotLight`, `environment`, `postProcess` (общая экспозиция, bloom и грейдинг проекта), `farGround` (бесконечный луг до горизонта), `script`/`prop`/`tag`. Есть и методы «каркаса» станции: `game`, `player`, `infoBoard`, `portal`, `tourCameras`, `stationBasics`.

### Игровой модуль `Showcase`

Нативный код проекта лежит в `Source/`. Это `IEngineModule`, который регистрирует себя макросом из [`runtime/game_module.hpp`](../../engine/runtime/include/oxwald/runtime/game_module.hpp):

```cpp
OX_GAME_MODULE("Showcase", ShowcaseModule);
```

Библиотека `ox_showcase_game` линкуется в OxwaldPlayer и OxwaldEditor с `WHOLE_ARCHIVE`, иначе линковщик выбросил бы статический регистратор. Включается модуль только в проектах, которые назвали его явно: `"modules": {"Showcase": true}` в `.oxproj`. В других проектах плеер его не создаёт.

Что делает модуль:

| Часть | Файл | Что делает |
| --- | --- | --- |
| `ShowcaseModule` | `showcase_module.cpp` | Разбирает `gameArgs`, читает `stations.json`, добавляет `Assets/Scripts` в пути поиска Lua, автосохраняет станцию каждые 180 с игрового времени, выполняет отложенные сохранения, загрузки и смену уровня |
| Lua-таблица `showcase` | `showcase_lua.cpp` | Режим запуска, cvar'ы и консоль, уровни, сохранения, статистика и сведения о GPU/RT/апскейлерах, GPU-тайминги, шины звука, сетевое демо, скриншоты, отладочная отрисовка, переназначение клавиш |
| Экскурсия | `tour.cpp` | C++-корутины на `CoroutineScheduler` |
| Сетевое демо | `net_demo.cpp` | Listen-сервер + бот-клиент (см. станцию 12) |

**Почему сохранения и смена уровня отложены.** Скрипт, который вызвал `showcase.load(...)`, работает внутри мира, который загрузка уничтожит. Поэтому `showcase.save`/`load`/`loadLevel` только ставят запрос в очередь, а `ShowcaseModule::preUpdate` выполняет его в начале следующего кадра:

```cpp
void ShowcaseModule::preUpdate(Engine& engine, const FrameTime& time) {
    m_realTime += time.realDt;
    applyPending();   // queued saves, then a queued load, then a queued level change
    // ... autosave timer (stations only)
    m_tour->preUpdate(time);
    m_net->preUpdate(time);
}
```

Результат загрузки возвращается в Lua событием: `showcase.loaded` или `showcase.loadFailed` (`ShowcaseModule::publish` → `events.subscribe` в `flow.lua`).

**Экскурсия.** На каждый уровень запускается своя корутина `Tour::stationShow`. Смена уровня отменяет корутины старого мира, так что «хвосты» прошлой станции не остаются. Корутина делает камеру `TourCam.A` основной и публикует событие `showcase.caption` (подпись в HUD). Затем она прогревает кадр: минимум 100 кадров и 2,5 с, пока не догрузятся ассеты, пробы, история TAA/SSR и автоэкспозиция. После этого при `--shots` снимает скриншот и 5 секунд ведёт камеру от `TourCam.A` к `TourCam.B`. В headless полёт идёт по кадрам (90 кадров), чтобы прогоны были детерминированными.

```cpp
for (u32 i = 0; t < duration && (!headless || i < frames); ++i) {
    co_await nextFrame();
    t += headless ? duration / frames : m_module.engine().frameTime().realDt;
    const f32 k = smooth(f32(t / duration));
    Transform cur = from;
    cur.position = glm::mix(from.position, to.position, k);
    cur.rotation = glm::slerp(from.rotation, to.rotation, k);
    a.setWorldTransform(cur);
}
requestStation(nextIndex(index));
```

### Lua: `game.lua` и `lib/flow.lua`

В каждой сцене есть сущность `Game` со скриптом `game.lua`. Генератор записывает тексты станции в свойства скрипта, а `game.lua` переносит их в модель данных HUD:

```lua
hud:set("stationNum", n and string.format("СТАНЦИЯ %02d / %02d", n, #showcase.stations()) or "OXWALD SHOWCASE")
hud:set("stationTitle", self.title)
hud:set("infoTitle", self.title)
hud:set("infoDesc", self.description)
hud:set("infoHints", self.hints)
hud:set("infoGuide", self.guide)
```

Общая логика игры — HUD, главное меню и пауза, настройки, браузер сохранений, всплывающие сообщения, F5/F9, Esc — вынесена в модуль `lib/flow.lua`. VM кэширует модули: `require("lib.flow")` в любой сцене возвращает одну и ту же таблицу. Поэтому документы RmlUi, модели данных и обработчики `ui.on` создаются один раз, в первом `flow.init()`, и переживают смену уровней:

```lua
function flow.init(stationFn)
    onStation = stationFn
    if initialized then return end
    initialized = true
    hud = ui.createModel("hud", {
        badgeVisible = true, stationNum = "", stationTitle = "",
        infoVisible = false, infoTitle = "", infoDesc = "", infoHints = "", infoGuide = "",
        prompt = "", toast = "", saving = "", fps = "", crosshair = false,
        captionVisible = false, captionTitle = "", captionText = "", captionStep = "",
    })
    menuModel = ui.createModel("menu", { canContinue = false, version = "OxwaldShowcase 1.0 · OxwaldEngine" })
    ui.load(flow.docs.hud, true)
    ui.load(flow.docs.menu)
    -- ... pause, settings, saves, dialogue, side panel
    require("lib.settings_menu").init(flow)
```

Меню устроены как стек: `flow.push(doc)` показывает документ, ставит игру на паузу и освобождает курсор, `flow.pop()` возвращает предыдущий экран. `flow.blocking()` (стек не пуст) проверяют все скрипты станций, чтобы клавиши не срабатывали поверх меню. «Нажми E» для любых интерактивных объектов — это `lib/interact.lua`.

### Меню RmlUi и настройки

Документы лежат в `Assets/UI/`. `hud.rml` привязан к модели `hud` из `flow.lua`, `main_menu.rml` — к модели `menu`. `settings.rml` привязан к модели `settings`, которую предоставляет сам движок: качество Low/Medium/High/Ultra/Auto с результатом автоопределения, окно, разрешение, VSync, трассировка лучей (неактивна, если недоступна, с причиной), апскейлер с доступностью, громкости, кнопки «Сохранить»/«Отменить». Остальные строки настроек дописывает `lib/settings_menu.lua`: уровни по группам масштабируемости (`sg.Shadows`, `sg.Reflections`, …), режим апскейла `r.Upscaler.Quality`, масштаб рендера `r.ScreenPercentage` и переназначение клавиш через `showcase.input.capture`:

```lua
ui.on(doc, "groups", "click", function(ev)
    local g, l = (ev.target or ""):match("^g%-(%a+)%-(%d)$")
    if g then
        showcase.setCVar("sg." .. g, l)
        M.refresh()
    end
end)
```

Кнопки, которые создаются в разметке динамически, получают `id` вида `g-Shadows-2`, а один обработчик на контейнере разбирает `ev.target`. Так же сделаны слоты в браузере сохранений и варианты ответов в диалоге.

### Станция сохранений изнутри

Сохранение проходит три слоя:

1. **Данные.** Генератор вешает `SaveGameComponent` на всё, что должно пережить загрузку (ящики, лампы, бруски башен на станции «Физика»).
2. **Lua.** Скрипты вызывают `showcase.save(slot, name)`, `showcase.quickSave()`, `showcase.load(slot)`. Браузер слотов в `flow.lua` строит список из `showcase.saves()`: имя, уровень, дата, время в игре, тип (автосохранение/быстрое/ручное), число сущностей.
3. **C++.** `ShowcaseModule::applyPending` вызывает `Engine::saveGame`/`loadGame` в начале кадра и запоминает время и тип последнего сохранения для индикатора `showcase.saveIndicator()`.

Встроенный таймер автосохранения движка модуль выключает (`setAutosaveInterval(0.0)`) и автосохраняет сам: так он знает, когда показать индикатор. Уход со станции через портал тоже автосохраняет покидаемый уровень (`SaveGameConfig::autosaveOnLevelChange`). «Продолжить» в главном меню загружает самое свежее сохранение, пропуская автосохранения главного меню. Слоты лежат в `user://` (каталог задаётся `--user-dir`).

## Шаг 7. Добавляем свою станцию

Добавим станцию 15 «Поворотный стол»: на диске стоят несколько фигур, `[K]` запускает и останавливает вращение. Новая станция сама получит портал в хабе, место в экскурсии, номер в HUD, материалы `Glow.*` / `Accent.*` своего цвета и смоук-тест.

### 1. Описание станции

Добавьте запись в конец списка `stations()` в [`Tools/gen_common.cpp`](../../samples/OxwaldShowcase/Tools/gen_common.cpp). Поля по порядку: `id`, имя файла сцены, название, описание для инфо-стенда и подписи экскурсии, клавиши, глава «Подробнее», цвет портала и стенда.

```cpp
{"turntable", "15_Turntable", "Поворотный стол",
 "Пример своей станции из главы 33: сцена собрана генератором, а стол вращает Lua-скрипт со свойством speed.",
 "[K] — вращение вкл/выкл", "docs/guide/33-showcase.md", {0.9f, 0.6f, 0.9f}},
```

Порядок в `stations()` — это порядок порталов в хабе, номер станции в HUD и порядок экскурсии.

### 2. Сцена в генераторе

Создайте `Tools/gen_stations_mine.cpp`. CMake собирает `Tools/*.cpp` через `GLOB CONFIGURE_DEPENDS`, так что новый файл подхватится сам:

```cpp
// Station 15: turntable (guide chapter 33, "Добавляем свою станцию").
#include "gen.hpp"

#include <oxwald/gameplay/world/components.hpp>

namespace ox::showcase::gen {

void buildTurntable(Gen& gen) {
    const Station& s = station("turntable");
    SceneBuilder sb(gen, s.id);
    Entity sun = sb.sun({0.4f, 0.6f, 0.3f}, 40000.0f);
    sb.create("Sky").add<gameplay::SkyComponent>().sunDirection = glm::normalize(glm::vec3(0.4f, 0.6f, 0.3f));
    sb.environment(sun);
    sb.postProcess();
    sb.farGround();
    sb.block("Floor", "Grid.x16", {0, -0.25f, 0}, {40, 0.5f, 40});
    // Player, info board, hub portal, Game entity with the station texts.
    sb.stationBasics(s, {0, 0.05f, 12}, 0.0f, {-3.5f, 0, 9}, 30.0f, {0, 0, 16.5f}, 180.0f);

    // The script rotates this (unscaled) root; the disc and the figures are its children.
    Entity table = sb.create("Turntable");
    table.setPosition({0, 0.1f, 0});
    sb.script(table, "Scripts/turntable.lua");
    sb.prop(table, "speed", 0.8);
    sb.prim("Disc", Primitive::Cylinder, "BrushedSteel", {0, 0, 0}, {6.0f, 0.2f, 6.0f}, {}, table);
    sb.prim("Ring", Primitive::Torus, "Gold", {0, 1.3f, 0}, glm::vec3(1.6f), eulerDeg(90, 0, 0), table);
    sb.prim("Ball", Primitive::Sphere, "Chrome", {2.0f, 0.6f, 0}, glm::vec3(1.0f), {}, table);
    sb.prim("Box", Primitive::Cube, "Glass.Blue", {-2.0f, 0.5f, 0}, glm::vec3(0.9f), yawRotation(30), table);
    sb.spotLight("TableSpot", {0, 6.0f, 4.0f}, {0, 0.8f, 0}, {1.0f, 0.95f, 0.9f}, 8000.0f, 14.0f, 18.0f, 32.0f);

    sb.tourCameras({6.0f, 3.0f, 7.0f}, {0, 1.0f, 0}, {-5.0f, 1.6f, 5.0f}, {0, 1.0f, 0}, 55.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

} // namespace ox::showcase::gen
```

Объявите функцию в [`Tools/gen.hpp`](../../samples/OxwaldShowcase/Tools/gen.hpp) рядом с остальными (`void buildTurntable(Gen& gen);`) и добавьте её в таблицу `builds[]` в [`Tools/generate_showcase.cpp`](../../samples/OxwaldShowcase/Tools/generate_showcase.cpp):

```cpp
                            {"rtx", buildRtx},             {"saves", buildSaves},
                            {"turntable", buildTurntable}};
```

Материалы `BrushedSteel`, `Gold`, `Chrome`, `Glass.Blue`, `Grid.x16` уже есть в `generateMaterials`. Для своих материалов добавьте вызов `gen.material(...)` туда же. Не забудьте `tourCameras`: без `TourCam.A` экскурсия напишет `tour: no TourCam.A` и не сможет показать станцию.

### 3. Lua-скрипт

`Assets/Scripts/turntable.lua`:

```lua
-- Поворотный стол: [K] запускает и останавливает вращение.
local flow = require("lib.flow")

properties = {
    speed = { type = "float", default = 0.8, min = 0, max = 5, tooltip = "Угловая скорость, рад/с" },
}

function onStart(self)
    self.spinning = true
end

function onUpdate(self, dt)
    if showcase.mode() == "play" and not flow.blocking() and input.keyPressed("K") then
        self.spinning = not self.spinning
        flow.toast(self.spinning and "Стол вращается" or "Стол остановлен")
    end
    if self.spinning then self.entity.transform:rotate(vec3(0, 1, 0), self.speed * dt) end
end
```

Это шаблон для любого скрипта станции. Клавиши обрабатываются только в режиме `play` и только когда не открыто меню (`flow.blocking()`); сообщения выводятся через `flow.toast`. В экскурсии и фоторежиме ввод игнорируется, а сама анимация идёт.

### 4. Инфо-стенд и портал в хабе

Отдельно делать ничего не нужно:

- **Инфо-стенд** ставит `stationBasics`. Его тексты берутся из полей `Station` (`description`, `hints`, `guide`) и попадают в свойства `game.lua`, а оттуда в HUD.
- **Портал в хабе** создаёт `buildHub`, который обходит `stations()`. Порталы равномерно распределены по кругу, поэтому с 15-й станцией сдвинутся все. `Hub.oxscene` перезапишется целиком, это нормально.
- **`stations.json`** пишет `generateData`. Его читают игровой модуль (экскурсия) и Lua (`showcase.stations()`: номер в HUD, названия порталов, названия уровней в браузере сохранений).
- **Материалы** `Glow.turntable` и `Accent.turntable` создаются в цикле по `stations()` в `generateMaterials`.

### 5. Генерация и проверка

```sh
cmake --build build/showcase --target oxshowcase_regenerate
build/showcase/bin/oxshowcase_generate --check            # 0 differences
build/showcase/bin/OxwaldPlayer --project samples/OxwaldShowcase \
    --scene project://Assets/Scenes/Stations/15_Turntable.oxscene
build/showcase/bin/OxwaldPlayer --project samples/OxwaldShowcase -- --tour --station turntable
```

Генератор создаст `.meta` для нового `turntable.lua` с детерминированным UUID (`scanAndMetaEverything`). Скрипт должен уже лежать на диске к моменту запуска генератора. Затем закоммитьте сгенерированные файлы вместе с кодом, иначе `Showcase.GeneratorCheck` упадёт у остальных.

### 6. Тест

Смоук-тесты сцен строятся по `file(GLOB ... Stations/*.oxscene)` при конфигурации, поэтому после генерации перезапустите CMake:

```sh
cmake --preset dev -B build/showcase -DVCPKG_MANIFEST_INSTALL=OFF
ctest --test-dir build/showcase -R "Showcase.Scene.15_Turntable|ShowcaseScripts|Showcase.GeneratorCheck" --output-on-failure
```

`Showcase.Scene.15_Turntable` прогонит сцену 120 кадров в headless-плеере в фоторежиме. `ShowcaseScripts.AllCompile` проверит, что `turntable.lua` компилируется. Если у станции есть состояние, которое нужно проверить (как у сохранений), допишите gtest в `Tests/showcase_tests.cpp` по образцу `ShowcaseSaves.RoundTripInSaveStation`: структура `ShowcaseEngine` поднимает headless-движок с UI и модулем `Showcase` на нужной сцене.

## Тесты

```sh
cmake --build build/showcase --target oxshowcase_generate OxwaldPlayer ox_showcase_tests
ctest --test-dir build/showcase -L showcase
```

| Тест | Что проверяет |
| --- | --- |
| `Showcase.GeneratorCheck` | `oxshowcase_generate --check`: файлы в репозитории совпадают с тем, что выдаёт генератор |
| `Showcase.Scene.<имя>` | Каждая сцена (`MainMenu`, `Hub`, все `Stations/*`) отрабатывает 120 кадров в headless-плеере (`--quality high`, `-- --photo`). Тест падает, если в логе есть `[error]`, `[fatal]`, `Validation Error`, `VUID-` или `assertion` |
| `Showcase.Tour` | Полная экскурсия в headless (`--quality medium`, `-- --tour --quit`) доходит до `Tour finished` |
| `ShowcaseScripts.AllCompile` | Все `Assets/Scripts/**/*.lua` компилируются (их больше 20) |
| `ShowcaseSaves.RoundTripInSaveStation` | Станция сохранений: сохранить → сдвинуть ящик и переключить лампу → загрузить → позиция и яркость вернулись, слот есть в `listSlots`, ошибок Lua нет |

Тесты сцен и экскурсии помечены и меткой `gpu`: им нужен Vulkan-рендер, пусть и без окна. Каждый тест получает свой `--user-dir`, поэтому сохранения и настройки разработчика они не трогают.

## Типичные ошибки и подводные камни

- **Модуль `Showcase` не загрузился** (в Lua `showcase` равен `nil`, в логе нет `Showcase module ready`). Проверьте `"modules": {"Showcase": true}` в `.oxproj` и что плеер собран в том же build-каталоге, где сконфигурирован `samples/OxwaldShowcase` (нужны все модули из шапки главы).
- **`--shots` не пишет файлы.** Скриншоты экскурсии снимаются только при `--headless`.
- **Правка сцены в редакторе пропала.** Сцены создаёт генератор, см. [шаг 1](#редактор).
- **После добавления сущности в генератор перестали грузиться старые сохранения.** UUID сущностей зависят от порядка их создания; добавляйте новые сущности в конец функции сцены.
- **Клавиша станции срабатывает поверх меню.** Проверяйте `flow.blocking()` и `showcase.mode() == "play"`, как в остальных скриптах.
- **На macOS серые переключатели RTX и DLSS.** Это ожидаемо: MoltenVK не поддерживает трассировку лучей Vulkan, а DLSS работает только на NVIDIA под Windows/Linux. Причина показана прямо на панели станции 13.

## API

| Файл | Что внутри |
| --- | --- |
| [`Source/showcase.hpp`](../../samples/OxwaldShowcase/Source/showcase.hpp) | `ShowcaseModule`, `LaunchArgs`, `StationInfo`, `activeModule()` |
| [`Source/showcase_lua.cpp`](../../samples/OxwaldShowcase/Source/showcase_lua.cpp) | Вся Lua-таблица `showcase` |
| [`Source/tour.cpp`](../../samples/OxwaldShowcase/Source/tour.cpp) | Экскурсия и фоторежим |
| [`Source/net_demo.cpp`](../../samples/OxwaldShowcase/Source/net_demo.cpp) | Listen-сервер и бот-клиент |
| [`Tools/gen.hpp`](../../samples/OxwaldShowcase/Tools/gen.hpp) | `Station`, `Gen`, `SceneBuilder`, список функций сцен |
| [`Tools/gen_common.cpp`](../../samples/OxwaldShowcase/Tools/gen_common.cpp) | Список станций, запись файлов и `.meta`, «каркас» станции |
| [`Assets/Scripts/lib/flow.lua`](../../samples/OxwaldShowcase/Assets/Scripts/lib/flow.lua) | HUD, меню, браузер сохранений |
| [`Tests/CMakeLists.txt`](../../samples/OxwaldShowcase/Tests/CMakeLists.txt) | Тесты сцен, экскурсии и генератора |
| [`engine/runtime/include/oxwald/runtime/game_module.hpp`](../../engine/runtime/include/oxwald/runtime/game_module.hpp) | `OX_GAME_MODULE`, `registerGameModule` |

Краткая справка по проекту: [`samples/OxwaldShowcase/README.md`](../../samples/OxwaldShowcase/README.md).

## Что дальше

- [05. Runtime и игровой цикл](05-runtime.md) — `.oxproj`, модули движка и OxwaldPlayer.
- [15. Скрипты на Lua](15-scripting-lua.md) — свойства, корутины и `bindApi`, на которых построена таблица `showcase`.
- [29. UI](29-ui.md) — RmlUi, модели данных и меню настроек.
- [30. Редактор](30-editor.md) — работа с проектом в OxwaldEditor.
- [31. Ассеты](31-assets.md) — сборка проекта в `.oxpak`.
- [32. Компоненты ECS](32-gameplay-components.md) — компоненты, из которых собраны станции.
- [Оглавление](README.md).
