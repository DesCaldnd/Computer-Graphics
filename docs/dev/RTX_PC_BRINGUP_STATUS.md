# Запуск на RTX-ПК: состояние и план продолжения

Журнал выполнения [RTX_PC_BRINGUP.md](RTX_PC_BRINGUP.md). Сессия 1: 2026-10-08, прервана для переезда на другую
машину. Этот файл — точка входа для продолжения: что сделано, что сломано, что делать дальше.

Машина сессии 1: Windows 11 Pro 26200, NVIDIA GeForce RTX 3080 10 ГБ (драйвер 617.42, Vulkan 1.4), i9-9900K, 32 ГБ,
VS 2022 17.14 (MSVC 14.44.35207), CMake 3.30, Ninja 1.12.1, Git LFS 3.7.1, vcpkg 2025-12-16 (клон на коммите
`b21ff8f`, baseline манифеста в нём есть).

---

## 1. Как поднять окружение на новой машине (Windows)

1. Поставить: VS 2022 с «Desktop development with C++», CMake ≥ 3.25, Ninja, Git + Git LFS (`git lfs install`),
   Python 3, свежий драйвер NVIDIA.
2. vcpkg: `git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg`, `bootstrap-vcpkg.bat`,
   задать `VCPKG_ROOT`.
3. Клонировать репозиторий в короткий путь без пробелов. `.gitattributes` теперь фиксирует LF для текстовых файлов,
   дополнительных настроек `core.autocrlf` не нужно.
4. Зависимости (из «x64 Native Tools Command Prompt for VS 2022», из корня репозитория):

   ```bat
   %VCPKG_ROOT%\vcpkg install --x-install-root=vcpkg_installed --triplet x64-windows
   ```

   На i9-9900K заняло около 44 минут вместе с Qt (без бинарного кэша). Порт `nvidia-dlss` исправлен и ставится
   (тянет ~700 МБ через Git LFS).
5. Сборка и тесты:

   ```bat
   cmake --preset dev -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_TARGET_TRIPLET=x64-windows -DOX_WARNINGS_AS_ERRORS=OFF
   cmake --build --preset dev
   ctest --preset dev -j8 --output-on-failure
   ```

   `-DOX_WARNINGS_AS_ERRORS=OFF` пока обязателен: возврат `/WX` не сделан (раздел 4, пункт 3).
6. Замечания:
   - Не запускать тесты из-под администратора: loader игнорирует `VK_ADD_LAYER_PATH` в elevated-процессах.
   - Брандмауэр Windows спрашивает про сетевой доступ для тестовых exe (Tracy-клиент слушает порт). На тесты
     ответ не влияет.
   - В Git Bash MSVC пишет диагностику в локали системы; `VSLANG=1033` не помог. Коды ошибок (`C2146`, `LNK2019`)
     читаются в любом случае.
   - Пропуск `DeviceLifecycle.SurfaceDeviceAfterHeadlessDevice` — норма: драйвер NVIDIA на Windows не даёт
     `VK_EXT_headless_surface`.

---

## 2. Что сделано (всё в ветке `oxwald-engine`)

Раздел 1–2 плана (окружение, зависимости) — выполнены. Раздел 3 (сборка) — выполнен, кроме возврата `/WX`.
Раздел 4 (тесты) — в работе. Разделы 5–9 — не начаты, кроме автоматической части DLSS.

### Сборка под MSVC
- Всё собирается: движок, плеер, инструменты, тесты, примеры гайда, тестовый проект, редактор.
- `.gitattributes` (LF для текста, бинарные ассеты `-text`).
- Порт `nvidia-dlss`: фетч по SHA вместо тега (иначе падает `git lfs fetch`), явный LFS-remote, в список рантайма
  попадают только `nvngx_*.dll`, отдельный список dev-DLL для Debug.
- CMake: `NOMINMAX`/`WIN32_LEAN_AND_MEAN`, `/Zc:preprocessor` (нужен для `__VA_OPT__`), `/bigobj`; манифест
  validation layer с `.\\` в `library_path` (Windows-loader иначе ищет DLL в cwd); NGX DLL по конфигурации;
  define'ы Jolt для RelWithDebInfo (иначе `VerifyJoltVersionID()` падает); `ws2_32 winmm` для enet;
  `gtest_discover_tests` в режиме `PRE_TEST` на Windows.
- Редактор: Win32-surface (и xcb для Linux) в `vulkan_viewport.cpp`, деплой Qt-плагинов рядом с exe, пути через
  UTF-16 (`fsPath`/`qsPath`), строка компилятора в «О программе». Wayland-surface не реализован (fallback на
  программный viewport).
- Исходники: недостающие стандартные include (~120 файлов), POSIX-вызовы, отображение pak в память через Win32,
  запись сгенерированных файлов в бинарном режиме, `Clock::sleepUntil` на waitable timer (иначе ограничитель
  кадров квантуется по 15,6 мс).

### Баги, найденные на Windows/NVIDIA и исправленные
| Баг | Где | Суть |
| --- | --- | --- |
| LNK2019 `basisu::unpack_bc7` | `engine/assets/src/bc_codec.cpp` | вызывалась внутренняя функция libktx; на Windows ktx — DLL без этого экспорта. Написан свой BC7-декодер, сверен с эталоном на 40 млн блоков |
| Lua: чтение полей компонентов даёт nil | `engine/gameplay/src/lua_bindings.cpp` | `f(childRef(c, path), std::move(path))` — MSVC вычисляет аргументы справа налево |
| Вода расходится с эталоном | `water_feature.cpp` | `std::uniform_*_distribution` различаются между STL; алгоритм libc++ выписан явно |
| Трава расходится с эталоном | `vegetation_assets.cpp` | два вызова RNG в аргументах одного конструктора |
| Шов в облаках | `shaders/render/volumetrics/noise_gen.comp` | `%` с отрицательным операндом в GLSL — UB, NVIDIA считает иначе |
| `oxshowcase_generate --check` падает | `samples/OxwaldShowcase/Tools` | удаление ещё открытого файла; плюс порядок вычисления в генерации `fire_loop.wav` |
| Гонка при параллельном `ctest` | `writeFileAtomic`, кэш SPIR-V | фиксированное имя `<path>.tmp` у всех процессов |
| DLSS: ошибки валидации и `FAIL_PlatformError` | `render_fixture.cpp`, `dlss_ngx.cpp` | устройство создавалось без расширений NGX (`VK_NVX_binary_import`, `VK_NVX_image_view_handle`, `VK_KHR_push_descriptor`); теперь `dlss::probe` сначала проверяет включённые расширения |
| DLSS: утечка объектов NGX и segfault при выходе | `dlss_ngx.cpp`, `rhi::Device` | NGX гасился только из `DlssFeature::shutdown`; добавлен `Device::addShutdownCallback` |
| RT: `activeThisFrame()` всегда false после кадра | `rt_scene_gpu.cpp` | сравнение с номером кадра, который `endFrame()` уже увеличил |

`--check` генератора: 26 бинарных ассетов отличаются от закоммиченных на последний бит (libm, FMA). Ассеты не
перегенерированы; `--check` сравнивает PNG/WAV/сцены/меш манекена с жёстким числовым допуском и печатает
`0 differences, 293 files up to date (27 of them within the numeric tolerance)`.

### DLSS (раздел 6 плана) — автоматическая часть сделана
- `PostProcessTest.DlssRendersOnRtx` выполняется и проходит, плюс новые тесты: режимы качества, сходимость на
  статике, движение камеры, экспозиция, пересоздание рендерера и устройства.
- Внутреннее разрешение при выводе 960×540: UltraPerformance 320×180 (mip bias −2,585), Performance 480×270
  (−2,000), Balanced 557×313 (−1,785), Quality 640×360 (−1,585), DLAA 960×540 (−1,000).
- **Знак jitter (пункт бэклога): менять ничего не пришлось.** `RenderView::jitterPixels()` передаётся как есть.
  PSNR против 16× SSAA на статике: как есть 40,58 дБ (TAAU 39,22); любой другой знак 25–31 дБ и мерцание.
- Векторы движения: `uvCur − uvPrev`, без jitter, `MVLowRes`, `InMVScale = (−renderW, −renderH)`. Верный вариант
  31,04 дБ (TAAU 29,51), с перевёрнутой осью 28,6–28,8.
- Экспозиция: передаётся текстура `Exposure` + `InPreExposure`; `AutoExposure` только без текстуры. Пресеты L/M
  (UltraPerformance/Performance) входы экспозиции игнорируют.
- `InSharpness` в NGX устарел: `r.Upscaler.Sharpness` на DLSS не влияет.
- Время прохода DLSS при 1080p (с валидацией, GPU делили другие процессы — цифры грубые): Performance 6,2 мс,
  Quality 3,95 мс, DLAA 2,9 мс.

### Ray tracing (разделы 4–5 плана) — частично
- BLAS/TLAS и все ray-query проходы исполняются на RTX 3080 без ошибок обычной валидации.
- `RayTracingTest.*`: 11 проходят, 1 пропущен (по делу), 3 падают (ниже).
- На глаз по картинкам: тени, AO, отражения, преломление, path tracer (режим 0) — правдоподобны. GI и ReSTIR
  оценить нельзя, пока не исправлены сцены тестов.

---

## 3. Состояние тестов

Последний полный `ctest` (до вливания веток агентов): 1230 тестов, 1212 прошли, 18 упали, 3 пропущены.
Четыре ветки агентов влиты cherry-pick'ом без конфликтов, но **слитое дерево не пересобиралось и полный прогон не
повторялся** — это первый шаг на новой машине. Каждая ветка по отдельности собиралась и проходила свои тесты.

Ожидаемо должны остаться красными:

| Тест | Причина | Что делать |
| --- | --- | --- |
| `RayTracingTest.RtReflectionsSeeOffscreenObjects` | сцена теста неверна: красная сфера позади камеры, плоский пол её отразить не может | поставить сферу впереди и выше кадра, например `{0, 6.1, -6}` |
| `RayTracingTest.RtGlobalIlluminationBleedsColour` | солнце `(0.6,-0.7,-0.2)` освещает обратную сторону красной стены | развернуть x солнца на `-0.6`, посмотреть картинку |
| `RayTracingTest.RestirShadowsManyLights` | при EV 9 `environment(1,1)` выбеливает кадр | параметризовать `aoScene(sky)`, взять ~0,02; корректность ReSTIR пока не подтверждена |
| `editor.SettingsTests`, `editor.IntegrationTests` | зависали после создания Vulkan-устройства | перепроверить: вероятно, исправлено починкой времени жизни NGX |

---

## 4. План продолжения

1. **Пересобрать и прогнать полный `ctest`**, зафиксировать числа. Проверить, что редактор больше не падает при
   выходе: `OxwaldEditor --project samples\OxwaldShowcase --smoke-seconds 10`.
2. **GPU-driven и async compute** — исправлено (раздел 5), но влито одним WIP-коммитом и после слияния не
   пересобиралось. Перемерить CPU-цифры и `r.ParallelRecording` на простаивающей машине, решить по `Auto`.
3. **Вернуть `OX_WARNINGS_AS_ERRORS=ON`.** Сейчас в `ox_set_warnings` (`cmake/OxwaldHelpers.cmake`) и в
   `ox_editor_compile_options` (`editor/CMakeLists.txt`) стоят глобальные `/wd4100 /wd4127 /wd4201 /wd4324 /wd4244
   /wd4267 /wd4305 /wd4245 /wd4456–4459`. По плану подавлять можно только стороннее: убрать как минимум
   `/wd4244 /wd4267 /wd4305 /wd4245` и исправить сами места. Оставшиеся предупреждения при текущем наборе:
   C4702 ×5, C4033 ×2 (корутины в `async/tests`), C4723 (`core/math.hpp:54`), C4146 (`rhi/src/swapchain.cpp:137`).
4. **Ray tracing:**
   - исправить три сцены тестов (таблица выше), посмотреть картинки GI и ReSTIR;
   - GPU-assisted validation ругается `VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-12281` в `RT.BuildAS`
     (ссылки TLAS на компактированные BLAS). Проверка: в `engine/rhi/src/acceleration_structure.cpp:109` временно
     отключить компакцию (`if (false && ...)`) и запустить `RayTracingTest.RtAmbient*` с
     `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT`. Либо пробел в слое, либо ошибка в
     последовательности компакции;
   - GPU-AV: «Descriptor index 7 references a resource that was destroyed» в `IBL.Generate`
     (`ibl/prefilter.comp:14`) — не RT, не разобрано;
   - sync validation: перезапустить с `VK_LAYER_VALIDATE_SYNC=1` (`VK_LAYER_ENABLES` устарел);
   - `shaders/render/raytracing/ddgi.glsl:36` `oxDdgiMod` — `%` с отрицательным операндом, то же UB, что в облаках;
   - path tracer режим 1 (RT-пайплайн, SBT) не покрыт ни одним тестом — добавить сравнение режимов 0 и 1;
   - `rt_restir_unshadowed.png`: диагональные полосы на полу, источник неизвестен;
   - не проверено вовсе: RT-тени локальных источников без ReSTIR, alpha-test и цветные тени, RT-волюметрика,
     refit BLAS для скиннинга, обновление TLAS на длинной дистанции;
   - затем ручной чек-лист render.md §11 на станции 13 тестового проекта и замеры 1080p/4K.
5. **Та же ошибка порядка вычисления в тестах** (`rng.range()` несколько раз в аргументах одного вызова):
   в `gpu_driven_tests.cpp` исправлено; осталось проверить `postprocess_gpu_tests.cpp:582,584`,
   `reflections_tests.cpp:415,417`, `renderer_tests.cpp:344,346`, `volumetrics_tests.cpp:301` — сцены на MSVC
   там могут отличаться от Mac.
6. **DLSS, ручная часть:** оконный запуск плеера и редактора, dev-DLL с отладочным оверлеем (сборка Debug),
   ресайз окна, замеры 4K на станциях 4 и 6 (Off / TAA / FSR1 / TAAU / DLSS Quality / Performance).
7. **Раздел 7 плана:** mesh-шейдеры и `drawIndirectCount` (подтвердить, что путь реально используется),
   `r.AsyncCompute` и `r.ParallelRecording` 0/1, стресс-сцены 10k/50k, тур `--tour` в окне, редактор.
8. **Раздел 8 плана — документация не тронута:** render.md §11, render_postprocess.md (решение по jitter, время
   жизни NGX через callback устройства), гайд 24/25/00, README, BACKLOG (закрыть DLSS jitter), perf.md,
   CHANGELOG 0.1.1, итоговый отчёт `docs/dev/reports/rtx-bringup-<дата>.md`.
9. **Проверка на macOS:** все исправления детерминизма воспроизводят поведение clang/libc++, но на Mac не
   проверялись — прогнать три эталона (вода, облака, растительность) и `oxshowcase_generate --check`.

Прочие известные риски: ~170 вызовов `path.string()` и `fopen`/stb/assimp идут через ANSI-кодировку — не-ASCII
пути вне неё сломаются (решение одним ходом: манифест `activeCodePage=UTF-8` на все exe); `std`-распределения в
`ai/nav_query.cpp:33` и сетевых транспортах дают разный геймплей между платформами; `veg_cull.comp` добавляет
записи через `atomicAdd`, порядок недетерминирован.

---

## 5. GPU-driven, async compute, mesh-шейдеры (раздел 7 плана) — автоматическая часть

Влито WIP-коммитом «WIP GPU-driven/rhi». До слияния на ветке агента: `ox_rhi_gpu_tests` 21 прошёл / 1 пропущен,
`GpuDrivenTest.*` 15/15, ошибок валидации нет.

- **Барьеры async compute.** Планировщик графа (`render_graph.cpp`, шаг 7) добавлял графические стадии прошлого
  кадра в первый барьер compute-прохода. Теперь пропускает это при разных семействах очередей, `sanitizeStages`
  фильтрует стадии по семейству. Перекрытие было нулевым ещё по двум причинам (батчи не резались на
  read-after-read между очередями; compute-батч отправлялся раньше, чем CPU записывал следующий графический) —
  исправлено, перекрытие теперь реальное: 0,075 из 0,076 мс в стресс-сцене.
- **`MatchesCpuPathWithShadows`** — баг теста: порядок вычисления аргументов на MSVC строил другую сцену. CPU- и
  GPU-пути всегда совпадали между собой, `drawIndirectCount` был исправен.
- **`TextureStreamingRespectsBudget`** — допустимое различие железа: резидентность та же (512 / 64 px), разница в
  выборе LOD при анизотропии 16× у NVIDIA. Допуск ослаблен до 5 % плохих пикселей (измерено 3,66 %), эталон не
  перегенерирован.
- **Аллокации.** Временный `std::unordered_set` в `GpuScene::updateInstances` аллоцирует на MSVC STL даже
  пустым; заменён сортированным scratch-вектором. Попутно: хеш `InstanceKey` `(entity<<16)^submesh` клал все
  сущности в одну корзину на MSVC, `updateInstances` был квадратичным (177 мс на 10k) — заменён, стало 2–3 мс.
- **Mesh-шейдеры и `drawIndirectCount` подтверждены** новыми счётчиками `meshShaderDrawCalls` /
  `indirectCountDrawCalls` (проверяются в тестах): «16 drawMeshTasksIndirect calls of 24», «indirect: 16 calls
  (16 with GPU draw count)». Картинка mesh-пути совпадает с compute-meshlet путём. Padded- и count-варианты на
  NVIDIA стоят одинаково.

Замеры RTX 3080, 1080p, валидация включена, CPU был загружен другими сборками на ~90 % (CPU-цифры шумные):

| Стресс-сцена | GPU кадр, мс | CPU рендерер, мс |
| --- | --- | --- |
| 10k, CPU-путь | 2,34 | 7,1 |
| 10k, GPU culling | 2,55 | 3,2 |
| 10k, + HiZ | 2,26 | 3,1 |
| 10k, + meshlets | 2,35 | 3,2 |
| 50k, CPU-путь | 9,8 | 29 |
| 50k, GPU culling | 10,8 | 13 |
| 50k, + HiZ | 8,3 | 12,5 |
| 50k, + meshlets | 8,5 | 12,6 |

Для сравнения M4 Pro, конфигурация по умолчанию (+ HiZ): 8,04 мс GPU на 10k, 24,03 мс на 50k.

- `r.AsyncCompute` выкл → вкл, GPU wall: 10k 2,22 → 2,19 мс; 50k 8,06 → 7,99, 8,31 → 8,19, 8,65 → 8,19 мс.
  Небольшой стабильный выигрыш; `Auto` уже включает его вне MoltenVK, эвристика не менялась.
- `r.ParallelRecording` выкл → вкл, CPU: без валидации 2,07 → 2,14 мс (выигрыша нет), с валидацией 8,0 → 5,4 мс.
  Под нагрузкой неубедительно, `Auto` не менялся.
- `updateInstances` на 50k: 12,8 мс под нагрузкой против 2,0 мс на M4 — горячая точка CPU на Windows
  (`unordered_map`), не оптимизировано.
- Воспроизведение: `GpuDrivenTest.StressSceneTimings` и новый `StressSceneAsyncComputeAndParallelRecording`
  (`OX_RENDER_PERF=<n>`).

На macOS не проверено: планировщик теперь режет больше батчей при включённом async (на MoltenVK `Auto` выключен,
умолчания не меняются), новые тесты планировщика гонялись только на Windows.
