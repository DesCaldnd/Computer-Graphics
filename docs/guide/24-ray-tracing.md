# 24. Трассировка лучей (режим RTX)

> Модуль `render`, таргет `Oxwald::render`, пространство имён `ox::render::rt`. Заголовки — `<oxwald/render/features/raytracing/raytracing.hpp>` (настройки, статус, регистрация), `rt_api.hpp` (интеграция), `rt_scene.hpp` (BLAS/TLAS на CPU), `denoiser.hpp`, `ddgi.hpp`. Низкоуровневые BLAS/TLAS — [глава 17](17-rhi-vulkan.md).

## Зачем

«Режим RTX» — одна галочка `r.RayTracing`. Она заменяет растровые эффекты трассированными лучами:

| Растровый эффект | Трассированная замена | Что даёт |
| --- | --- | --- |
| Карты теней (CSM, spot, point) | RT-тени | Точные мягкие тени с contact hardening, дырявые тени листвы, цветные тени сквозь стекло, сотни теневых источников с ReSTIR |
| SSR + reflection probes | RT-отражения | Отражения объектов за кадром и за камерой |
| SSAO / GTAO | RTAO | Затенение без экранных ореолов и без исчезания у края кадра |
| Irradiance-зонды | DDGI | Динамическое диффузное GI: цвет «перетекает» со стен на пол |
| Экранное преломление | RT-преломление | Стекло видит объекты вне экрана, честное полное внутреннее отражение |
| Тени в объёмном тумане | RT-видимость фроксел | Лучи света от окклюдеров за кадром |
| — | Path tracer | Эталонная картинка для сверки освещения (не для игры) |

Галочку можно переключать на ходу. Граф кадра пересобирается каждый кадр, и на следующем кадре растровые фичи просто сменяются трассированными, без перезапуска и загрузки. На машине без RT-железа галочка ничего не делает: рендер остаётся растровым, кадр тот же.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| RT-железо | `DeviceCaps::rayTracingSupported()` = `VK_KHR_acceleration_structure` + `VK_KHR_ray_query`. Для path tracer'а в режиме 1 ещё нужен `VK_KHR_ray_tracing_pipeline` |
| Гейтинг | RT-эффект работает, только если включены `r.RayTracing`, `rayTracingSupported()` **и** cvar самого эффекта (`r.RayTracing.Shadows` и т. д.) |
| Эксклюзивная группа | RT-вариант и его растровый двойник состоят в одной группе (`Shadows`, `AO`, `Reflections`, `IndirectDiffuse`, `Translucency`). Побеждает фича с бóльшим приоритетом: у RT он 100, у растра 0 |
| `RtStatus` | Доступность каждого эффекта и человекочитаемая причина отказа — для подсказки у выключенной галочки |
| BLAS / TLAS | Ускоряющие структуры: BLAS на уникальный (submesh, LOD), один TLAS на кадр со всеми инстансами |
| SVGF-денойзер | Пространственно-временной фильтр: из 1 луча на пиксель делает гладкую картинку. Работает на любом GPU |
| DDGI | Окно зондов вокруг камеры, которое лучи обновляют каждый кадр |
| ReSTIR DI | Выбор источника света для теневого луча пропорционально его вкладу, с переиспользованием во времени и по соседям |

## Шаг 1. Требования к железу и сборке

| Что | Требование |
| --- | --- |
| GPU | NVIDIA RTX (Turing и новее), AMD RDNA2+, Intel Arc — любой GPU, у которого Vulkan-драйвер даёт `VK_KHR_acceleration_structure` и `VK_KHR_ray_query` |
| Path tracer, режим `Pipeline` | Дополнительно `VK_KHR_ray_tracing_pipeline` (режим `RayQuery` его не требует) |
| ОС | Windows или Linux с актуальным драйвером |
| macOS | Не поддерживается: MoltenVK транслирует Vulkan в Metal и не реализует Vulkan ray tracing. Шейдеры компилируются, CPU-логика и денойзер тестируются, сами лучи — нет |
| Сборка | Ничего особенного: RT-шейдеры компилируются в SPIR-V везде, а включаются по `DeviceCaps` во время работы |

Устройство включает RT-расширения само, если они есть. `DeviceDesc::enableRayTracing = false` позволяет *не* включать их, даже когда GPU их поддерживает. Это удобно, чтобы проверить растровые фолбэки на RTX-машине ([глава 17](17-rhi-vulkan.md)).

## Шаг 2. Галочка: редактор, проект, игрок, консоль

Галочка всегда одна и та же — cvar `r.RayTracing` (по умолчанию `false`). Где её можно поставить:

| Где | Как | Куда сохраняется |
| --- | --- | --- |
| Редактор: **Project Settings → Rendering → Hardware Ray Tracing** | Переключатель **Ray Tracing** + переключатели эффектов | `.oxproj` → `rendering.cvars` (значения проекта по умолчанию) |
| Редактор: **Preferences → Play → Game User Settings** | Переключатель **Ray Tracing** | `user://settings.json` → `graphics.rayTracing` (настройка игрока) |
| Игра: меню графики | `UserSettings::graphics.rayTracing` ([глава 04](04-cvars-quality.md)) | `user://settings.json` |
| Консоль / конфиг | `r.RayTracing 1` | — / `.ini` |

![Project Settings → Rendering на Mac: баннер с причиной, галочка выключена](images/editor/project_settings_rendering.png)

На машине без поддержки переключатель **Ray Tracing** неактивен. Сверху показан баннер с причиной и названием GPU, такая же подсказка всплывает над переключателем. Переключатели эффектов активны только при включённой галочке («Enable Ray Tracing first»). В строке состояния редактора рядом с названием GPU стоит «· RT», если трассировка доступна.

Свой экран настроек строится так же — по `rt::rayTracingStatus`:

```cpp
#include <oxwald/render/features/raytracing/raytracing.hpp>

const rt::RtStatus status = rt::rayTracingStatus(device.caps());
rtCheckbox.setEnabled(status.available);
rtCheckbox.setTooltip(status.reason);   // пусто, если доступно
for (const rt::RtEffectStatus& e : status.effects) {
    // e.name "Shadows", e.cvar "r.RayTracing.Shadows", e.available, e.reason
    addEffectToggle(e.name, e.cvar, e.available, e.reason);
}
```

Причины, которые увидит пользователь:

| Ситуация | `reason` |
| --- | --- |
| Mac (MoltenVK) | «Apple M4 Pro does not expose VK_KHR_acceleration_structure, VK_KHR_ray_query (MoltenVK translates Vulkan to Metal and does not implement Vulkan ray tracing yet)» |
| Старый GPU / драйвер | «<GPU> does not expose … (requires a ray tracing capable GPU and an up-to-date driver)» |
| Есть ray query, нет RT pipeline | только у эффекта «Path tracer (RT pipeline)»: «<GPU> does not expose VK_KHR_ray_tracing_pipeline (the ray query mode still works)» |

Эффекты в `RtStatus::effects` по порядку: Shadows, Reflections, Ambient occlusion, Global illumination, Translucency, Volumetrics, ReSTIR DI, Path tracer (ray query), Path tracer (RT pipeline).

Проверка «будут ли лучи в этом кадре»:

```cpp
RenderSettings s = RenderSettings::fromCVars();          // s.rayTracing = r.RayTracing
bool active = rt::rayTracingActive(s, device.caps());     // галочка && железо
```

Рендерер сам гасит `RenderSettings::rayTracing` на устройстве без поддержки. Поэтому `renderer->settings().rayTracing` показывает, что реально действует в кадре.

Полный пример: `samples/guide_examples/24-ray-tracing/rt_settings.cpp` (тесты `StatusAndReasonsForUi`, `GatingNeedsCheckboxAndHardware`).

## Шаг 3. Эффекты

У каждого эффекта свой cvar: выключенный эффект возвращает растровый вариант, остальные остаются трассированными.

| Эффект | CVar | Фича / заменяет | Как работает |
| --- | --- | --- | --- |
| Тени | `r.RayTracing.Shadows` | `ShadowsRT` / карты теней | `SamplesPerPixel` лучей на пиксель. Солнце — конус с угловым радиусом `LightComponent::sourceRadius` (градусы), локальные источники — сферы радиуса `sourceRadius` (м). Альфа-тест учитывается, сквозь `Transparent`/`Refractive` проходит цветной свет (`.Colored`). Явно трассируются до 4 локальных источников (`.MaxLocalLights`); с ReSTIR — все теневые |
| ReSTIR DI | `r.RayTracing.Shadows.ReSTIR` | — | Резервуары по всем теневым локальным источникам: 32 кандидата, временное переиспользование (M ≤ 20×), пространственное (4 соседа в 16 px). Видимость выбранного источника задаёт долю затенения для всех |
| Отражения | `r.RayTracing.Reflections` | `ReflectionsRT` / SSR + зонды | GGX (VNDF), 1 отскок с освещением в точке попадания (солнце + один случайный источник с теневыми лучами, emissive, DDGI). Вес плавно гаснет от 75 % до 100 % `MaxRoughness`; дальше работают зонды/IBL |
| AO | `r.RayTracing.AO` | `AmbientOcclusionRT` / SSAO, GTAO | Косинусные лучи длиной `AO.Radius` (1 м) |
| GI (DDGI) | `r.RayTracing.GI` | `GlobalIlluminationRT` / irradiance-зонды | Окно `ProbesXZ × ProbesY × ProbesXZ` с шагом `ProbeSpacing` (2 м) едет за камерой. `RaysPerProbe` лучей на зонд за кадр, гистерезис 0.97, бесконечные отскоки через прошлый кадр |
| Преломление | `r.RayTracing.Translucency` | `TranslucencyRT` / экранное преломление | Для `Refractive`-материалов: отражение по Френелю + путь преломления (Снеллиус, внутренние отскоки до `MaxBounces`, TIR, Бугер — Ламберт). Детерминированно, без шума и денойзера. `Transparent` остаются растровыми |
| Объёмы | `r.RayTracing.Volumetrics` | `VolumetricsRT` | Видимость на фроксел сетки тумана: солнце, доля локальных источников, небо. Работает, только если включён объёмный туман ([глава 22](22-volumetrics.md)) |
| Path tracer | `r.PathTracing` | `PathTracer` / весь кадр | Прогрессивное накопление (RGBA32F) до `MaxSamples`; сброс при любом изменении камеры, объектов, света, окружения или настроек. `r.PathTracing.Mode`: `RayQuery` (мегаядро) или `Pipeline` (SBT, только непрозрачные тени) |

```cpp
// Включить RTX, но AO оставить растровым (GTAO), а тени — с ReSTIR для сотни ламп.
CVarRegistry& cv = CVarRegistry::instance();
cv.set("r.RayTracing", "true");
cv.set("r.RayTracing.AO", "false");
cv.set("r.RayTracing.Shadows.ReSTIR", "true");
```

Сцена ускоряющих структур (`RayTracingScene`) строится, только если включён хотя бы один RT-эффект или path tracer.

- **BLAS** собирается на уникальный (submesh, LOD). По умолчанию берётся *самый грубый* LOD (`r.RayTracing.BLAS.LOD = -1`): теням, AO, GI и глянцевым отражениям упрощение не мешает. На Ultra используется LOD 0. Статические BLAS сжимаются (compaction). За кадр строится не больше `r.RayTracing.BLAS.BuildsPerFrame` (16) BLAS, крупные на экране — первыми. Неиспользуемые выгружаются.
- **TLAS** перестраивается раз в кадр. Если поменялись только трансформы, делается refit; при добавлении или удалении инстансов — rebuild.
- **Маски.** Материал определяет, какие лучи видят инстанс: непрозрачные и alpha-test видны теням, AO и GI; прозрачные — отражениям, преломлению и path tracer'у. `castShadows = false` в `MeshRendererComponent` делает объект невидимым для теневых лучей.
- **Скиннинг.** Анимированные меши трассируются в bind pose, пока зона мира/скиннинга не передаст позы через `RayTracingSceneApi::setDeformedGeometryProvider` ([глава 28](28-world-rendering.md)).

```cpp
#include <oxwald/render/features/raytracing/rt_api.hpp>

if (auto* api = rt::RayTracingSceneApi::find(renderer->features())) {
    bool traced = api->activeThisFrame();          // строился ли TLAS в этом кадре
    u32 instances = api->tlasInstanceCount();
}
```

Полный пример: `samples/guide_examples/24-ray-tracing/rt_settings.cpp` (тест `BlasLodMasksAndAccumulation`) и `rt_gpu.cpp` (тест `CheckboxSwapsEffectsOrIsInert`: на Mac проверяет, что галочка инертна, на RTX — что TLAS построен).

## Шаг 4. Качество: Scalability::RayTracing

Все параметры привязаны к группе масштабируемости `RayTracing`. Значения по уровням Low / Medium / High / Ultra:

| CVar | По умолчанию | Low / Medium / High / Ultra |
| --- | --- | --- |
| `r.RayTracing.Shadows` / `.Reflections` / `.AO` | on | on,on,on,on / off,on,on,on / off,on,on,on |
| `r.RayTracing.GI` / `.Translucency` / `.Volumetrics` | on | off,off,on,on / off,on,on,on / off,off,on,on |
| `r.RayTracing.Shadows.SamplesPerPixel` / `.MaxLocalLights` | 1 / 4 | 1,1,1,2 / 1,2,4,4 |
| `r.RayTracing.Shadows.ReSTIR` / `.Colored` | off / on | off,off,off,on / off,on,on,on |
| `r.RayTracing.Shadows.ResolutionScale` | 100 | 50,100,100,100 (50 — половина + апсемпл) |
| `r.RayTracing.Reflections.MaxRoughness` / `.SamplesPerPixel` / `.ResolutionScale` | 0.6 / 1 / 100 | 0.3,0.4,0.6,0.8 / 1,1,1,2 / 50,50,100,100 |
| `r.RayTracing.AO.SamplesPerPixel` / `.ResolutionScale` | 1 / 100 | 1,1,2,4 / 50,50,100,100 |
| `r.RayTracing.GI.RaysPerProbe` / `.ProbesXZ` / `.ProbesY` / `.ResolutionScale` | 128 / 24 / 8 / 100 | 64,96,128,256 / 12,16,24,32 / 6,8,8,12 / 50,50,100,100 |
| `r.RayTracing.Translucency.MaxBounces` | 4 | 2,3,4,6 |
| `r.RayTracing.Volumetrics.LocalSamples` / `.SkyRays` | 2 / 1 | 1,1,2,4 / 0,1,1,2 |
| `r.RayTracing.Denoiser.Iterations` | 4 | 3,4,4,5 |
| `r.RayTracing.BLAS.LOD` | −1 (самый грубый) | −1,−1,−1,0 |

Без уровней: `r.RayTracing.Shadows/Reflections/AO.Denoiser` (on), `r.RayTracing.AO.Radius` 1, `r.RayTracing.GI.ProbeSpacing` 2, `r.RayTracing.GI.Hysteresis` 0.97, `r.RayTracing.Denoiser.MaxHistory` 32, `r.RayTracing.BLAS.BuildsPerFrame` 16, `r.PathTracing` off, `r.PathTracing.Mode` RayQuery, `.MaxBounces` 8, `.SamplesPerFrame` 1, `.MaxSamples` 4096.

```cpp
scalability::setGroup(Scalability::RayTracing, QualityLevel::Low);
rt::RtSettings s = rt::RtSettings::fromCVars();   // снимок всех r.RayTracing.* / r.PathTracing.*
// s.reflections == false, s.gi == false, s.shadowResolutionScale == 50
std::vector<std::string> all = rt::rayTracingCVarNames();   // для группы в UI настроек
```

Автоопределение качества ставит группу `RayTracing` на Low, если поддержки нет ([глава 26](26-quality-settings.md)).

Полный пример: `samples/guide_examples/24-ray-tracing/rt_settings.cpp` (тест `SettingsAndScalability`).

## Шаг 5. Денойзер SVGF

Все шумные RT-эффекты (тени, отражения, AO, GI) пускают мало лучей (1 на пиксель) и чистят результат общим денойзером `rt::SvgfDenoiser` (Schied et al. 2017). Он написан на обычных compute-шейдерах и работает на любом GPU, в том числе на Mac.

1. **Временной шаг.** История репроецируется по `Velocity`. Каждый из 2×2 билинейных тапов проверяется по прошлой глубине и нормали; при разрыве история начинается заново. Вес нового кадра — `max(1/(len+1), 1/maxHistory)`.
2. **Дисперсия.** Считается из моментов яркости во времени, а пока история короче 4 кадров — пространственно, окном 7×7.
3. **À-trous.** `iterations` проходов ядра 5×5 с шагом 1, 2, 4, 8, 16. Края держатся по градиенту глубины, нормали и яркости (с учётом дисперсии). Выход первой итерации становится историей.
4. Для эффектов в половинном разрешении — joint-bilateral апсемпл по глубине и нормалям.

| `DenoiserSettings` | По умолчанию | Смысл |
| --- | --- | --- |
| `enabled` | true | false — вход проходит насквозь (только конверсия формата) |
| `iterations` | 4 | Проходы À-trous (0..5); cvar `r.RayTracing.Denoiser.Iterations` |
| `maxHistory` | 32 | Минимальный вес нового кадра 1/32; cvar `r.RayTracing.Denoiser.MaxHistory` |
| `phiColor`, `phiNormal`, `phiDepth` | 4, 128, 1 | Жёсткость краёв по яркости, нормали, глубине |
| `lumaWeights` | (0.2126, 0.7152, 0.0722, 0) | Какой сигнал направляет фильтр: цвет, скалярный AO, 4 канала теней |
| `output` | `RGBA16F` | `RGBA16F`, `RGBA8`, `R8` |
| `temporal` | true | false — только пространственный фильтр |

Измерено на M4 Pro: шумный AO с 1 spp против эталона в 256 spp даёт RMSE 0.188 до денойзера и 0.026 после. После резкого сдвига камеры «призраков» нет. Цена на 1080p RGBA16F: временной шаг 0.56 мс, дисперсия 0.29 мс, À-trous 1.2 мс за итерацию — около 5.7 мс при 4 итерациях; в половинном разрешении примерно вчетверо меньше. Эффекты с короткой историей: отражения используют `maxHistory` 12, иначе зеркала смазываются при движении.

Свою фичу с шумным экранным сигналом можно чистить тем же денойзером: `SvgfDenoiser::denoise(ctx, "MyEffect", inputs, settings)`. Имя задаёт историю в виде, поэтому у каждого эффекта оно своё. Как писать фичи — в [главе 18](18-rendering-overview.md).

## Шаг 6. Что проверено на Mac, что — только на RTX

**Проверено на Mac (MoltenVK, без RT)** — `ctest -L render`:

- все RT-шейдеры и их варианты дефайнов компилируются в оптимизированный SPIR-V: compute с ray query и все стадии RT pipeline, push-блоки ≤ 128 байт;
- CPU-логика: маски TLAS, смещения SBT, упаковка трансформов, выбор refit или rebuild; планировщик BLAS (бюджет, приоритеты, жизненный цикл compaction, refit/rebuild деформируемых, выгрузка) против асинхронного мока; политика LOD; сбросы накопления; адресация, скроллинг и атлас DDGI; таблицы масштабируемости; причины недоступности; гейтинг эксклюзивных групп с RT-железом и без;
- GPU на обычных compute-шейдерах: денойзер (цифры выше), смешивание и применение DDGI на синтетических лучах, резервуары ReSTIR выбирают источники пропорционально вкладу (0.784 для пары 4:1). И главное: `r.RayTracing` на устройстве без поддержки инертен — те же проходы, та же картинка, ни одного RT-пайплайна.

**Требует RTX-класса** (тесты написаны и на Mac делают `GTEST_SKIP`): сборка и refit BLAS/TLAS, все dispatch'и ray query и RT pipeline, картинки эффектов (RT-тени против карт теней, затемнение RTAO, отражения объектов за кадром, перетекание цвета в GI, преломление, ReSTIR с 32 источниками, сходимость и сброс path tracer'а), переключение на ходу без утечек, число инстансов TLAS равно числу живых инстансов.

### Чек-лист на машине с NVIDIA RTX

1. `ox_render_gpu_tests --gtest_filter=RayTracingTest.*`: ничего не пропущено, нет ошибок validation. Посмотрите `<temp>/oxwald_render_out/rt_shadows.png`.
2. Плеер или редактор: несколько раз переключите `r.RayTracing` на ходу. Не должно быть рывков, кроме сборки BLAS; VRAM стабильна (`r.GpuTimings`, оверлей статистики); видны все RT-проходы (`RT.BuildAS`, `RT.Shadows`, …).
3. Тени: contact hardening при `sourceRadius` солнца 0.5° и 3°; в тенях листвы с alpha-test есть дыры; стекло даёт тонированную тень; с `r.RayTracing.Shadows.ReSTIR` и 100+ теневыми точечными источниками тени стабильны.
4. Отражения: зеркальный пол показывает объекты за камерой; при roughness 0 → 1 отражение плавно переходит в зонды; на солнце нет светлячков; при движении камеры смазывание на зеркалах приемлемое.
5. AO/GI: затемнение в контактах без экранных ореолов; красная стена «красит» белый пол; при проходе по уровню окно DDGI скроллится без скачков (новые слои сходятся примерно за 1 с); свет не протекает сквозь тонкие стены.
6. Стеклянная сфера: преломление переворачивает фон, у краёв кольцо полного внутреннего отражения, цветное поглощение по `absorptionDistance`, отражения объектов за кадром.
7. Объёмный туман с RT: лучи света от окклюдеров за кадром, световые столбы от локальных источников.
8. `r.PathTracing 1` (оба `r.PathTracing.Mode`, 0 и 1): картинка сходится без шума примерно за 1000 spp, сбрасывается при любом движении камеры или объекта, в целом совпадает с растровой (у растра нет GI и мягких теней).
9. Скиннинговый персонаж с `DeformedGeometryProvider`: тень следует за анимацией (refit), «застывшей» bind pose нет.
10. Захват в Nsight Graphics: число инстансов TLAS равно числу видимых инстансов, сжатые BLAS занимают около 50 % от несжатых.

Плюс быстрые проверки из этой главы: `ctest -L guide -R GuideRayTracing` — на RTX тест `CheckboxSwapsEffectsOrIsInert` идёт по ветке «TLAS построен», а `StatusOfThisDevice` печатает статус каждого эффекта.

## Типичные ошибки и подводные камни

- **«Включил RTX, ничего не изменилось».** Сначала проверьте `rt::rayTracingStatus(caps).reason`. На Mac и старых GPU галочка инертна по замыслу. На RTX проверьте cvar самого эффекта и `r.Feature.ShadowsRT` и т. п.
- **`rendering.rayTracingIfSupported` в `.oxproj` ничего не включает.** Поле читается, но рантайм его пока не применяет. Значение проекта по умолчанию задавайте через `"rendering": {"cvars": {"r.RayTracing": "true"}}`.
- **Переключатели RT AO и RT GI в Project Settings не действуют.** Редактор пишет cvar'ы `r.RayTracing.AmbientOcclusion` и `r.RayTracing.GlobalIllumination`, а рендерер читает `r.RayTracing.AO` и `r.RayTracing.GI`. До исправления включайте эти эффекты через консоль или `rendering.cvars`.
- **Тени пропали у части ламп.** Без ReSTIR трассируются только 4 локальных источника на вид (`r.RayTracing.Shadows.MaxLocalLights`), остальные остаются без теней. Включите `r.RayTracing.Shadows.ReSTIR`.
- **Прозрачные объекты освещены «без теней».** Потребители не в экранном пространстве (прозрачность, частицы) в RT-режиме получают солнце и локальные источники без затенения.
- **Отражения отличаются от модели.** Для BLAS берётся грубый LOD. Если в зеркальном полу видно упрощение, поставьте `r.RayTracing.BLAS.LOD 0`.
- **Персонаж отбрасывает тень в T-позе.** Не подключён `setDeformedGeometryProvider` — меш трассируется в bind pose.
- **Шум или «плывущие» тени.** Мало истории (резкие движения) или выключен денойзер (`r.RayTracing.*.Denoiser`). Добавьте `SamplesPerPixel` или итераций денойзера, но учтите цену.
- **Path tracer «не сходится».** Накопление сбрасывается от любого изменения: анимированные объекты, ветер, время суток. Для эталонного кадра остановите сцену.
- **Path tracer и производительность.** Растровые проходы под path tracer'ом продолжают выполняться: его картинка заменяет кадр перед постобработкой. Это инструмент сверки, а не игровой режим.
- **GI только вокруг камеры.** DDGI — одно окно зондов, которое едет за камерой. Дальше окна работает растровый фолбэк, переносов и классификации зондов нет.
- **Матовое стекло в RT.** RT-преломление детерминированное и гладкое; матовое (rough) стекло в RT-режиме выглядит гладким.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`raytracing.hpp`](../../engine/render/include/oxwald/render/features/raytracing/raytracing.hpp) | `RtSettings` (+`fromCVars`), `rayTracingActive`, `rayTracedRefractionActive`, `RtStatus`/`RtEffectStatus`, `rayTracingStatus`, `rayTracingCVarNames`, `registerRayTracingCVars`, `registerRayTracingFeatures`, группы `kGroup*`, `kRtPriority` |
| [`rt_api.hpp`](../../engine/render/include/oxwald/render/features/raytracing/rt_api.hpp) | `RayTracingSceneApi`: `find`, `setDeformedGeometryProvider`, `activeThisFrame`, `sceneHeaderAddress`, `tlasInstanceCount`, `blasStats` |
| [`rt_scene.hpp`](../../engine/render/include/oxwald/render/features/raytracing/rt_scene.hpp) | Маски `kMask*`, `instanceMask`, `selectBlasLod`, `TlasInstanceTable`, `BlasScheduler`, `IBlasBackend`, `DeformedGeometry`, `AccumulationTracker` |
| [`denoiser.hpp`](../../engine/render/include/oxwald/render/features/raytracing/denoiser.hpp) | `SvgfDenoiser`, `DenoiserSettings`, `DenoiserInputs/Outputs`, `DenoiseOutput` |
| [`ddgi.hpp`](../../engine/render/include/oxwald/render/features/raytracing/ddgi.hpp) | Раскладка DDGI: `DdgiVolumeDesc`, адресация зондов, атлас, направления лучей |
| [`device_caps.hpp`](../../engine/rhi/include/oxwald/rhi/device_caps.hpp) | `DeviceCaps::rayTracingSupported`, `rayTracingPipeline`, `whyRayTracingUnavailable` |

Шейдеры: `engine/shaders/render/raytracing/`. Подробности для разработчиков модуля: [`docs/dev/modules/render.md`](../dev/modules/render.md), раздел 11.

## Что дальше

- [17. RHI и Vulkan](17-rhi-vulkan.md) — `DeviceCaps`, BLAS/TLAS на уровне RHI.
- [20. Свет и тени](20-lighting-shadows.md) — растровые тени, которые заменяет RTX.
- [21. Отражения и GI](21-reflections-gi.md) — растровые SSR, зонды, GTAO.
- [25. Апскейлеры и постобработка](25-upscalers-postprocess.md) — DLSS на тех же RTX-картах.
- [26. Настройки качества](26-quality-settings.md) — группа `RayTracing` и автоопределение.
- [Оглавление](README.md).
