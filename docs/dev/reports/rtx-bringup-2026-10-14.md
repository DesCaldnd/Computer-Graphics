# Отчёт: запуск OxwaldEngine на ПК с NVIDIA RTX

Итог выполнения плана [RTX_PC_BRINGUP.md](../RTX_PC_BRINGUP.md). Работа шла 2026-10-08 … 2026-10-14 в ветке
`oxwald-engine` (37 коммитов после `ff1f6f1`).

**Результат.** Движок, плеер, инструменты, тестовый проект и редактор собираются под MSVC с включёнными
warnings-as-errors в пресетах `dev` и `debug`. `ctest`: 1249 тестов, 0 падений (`dev` — 3 пропуска, `debug` — 4). Ray
tracing и DLSS исполняются на реальном железе и проходят автоматические проверки без ошибок валидации. Не сделаны
интерактивные проверки в окне, Linux и перепроверка на macOS — см. раздел 7.

## 1. Железо и окружение

| | |
| --- | --- |
| GPU | NVIDIA GeForce RTX 3080, 10 ГБ, драйвер 617.42 (Vulkan 1.4, устройство создаётся как 1.3) |
| CPU / RAM | Intel Core i9-9900K (8 ядер, 16 потоков), 32 ГБ |
| ОС | Windows 11 Pro 10.0.26200 |
| Компилятор | Visual Studio 2022 17.14, MSVC 14.44.35207, Windows SDK 10.0.26100 |
| Сборка | CMake 3.30.0-rc4, Ninja 1.12.1, Git 2.53 + Git LFS 3.7.1 |
| vcpkg | 2025-12-16, клон на `b21ff8f`, baseline манифеста `0b88aac`; validation layers 1.4.341 |

Установка зависимостей (`vcpkg install … --triplet x64-windows`, без бинарного кэша): около 44 минут вместе с Qt.

## 2. Что сломалось при сборке и запуске

### Сборка и деплой
| Проблема | Исправление | Коммит |
| --- | --- | --- |
| Порт `nvidia-dlss`: `git lfs fetch <url> <tag>` падает, тег не становится локальным ref | фетч по SHA, явный LFS-remote; в список рантайма попадали все DLL из `bin/` | `b3de788` |
| Нет `.gitattributes`, системный `core.autocrlf=true` даёт CRLF в ассетах | LF для текста, бинарные ассеты `-text` | `b3de788` |
| Манифест validation layer с `./` в `library_path` (Windows-loader ищет в cwd) | `.\\` для DLL | `b624c82` |
| Jolt в RelWithDebInfo линкуется без `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED` | список конфигураций в `INTERFACE_COMPILE_DEFINITIONS` | `b624c82` |
| Нет `NOMINMAX`, `/Zc:preprocessor` (`__VA_OPT__`), `ws2_32`/`winmm` для enet | глобальные определения и флаги | `b624c82` |
| Редактор создаёт Vulkan-surface только для macOS | Win32 и xcb, деплой Qt-плагинов | `88e1293` |
| ~120 файлов без нужных стандартных include, POSIX-вызовы, `mmap` в pak | статический проход | `0feb65d` |
| LNK2019 `basisu::unpack_bc7`: внутренняя функция libktx, на Windows ktx — DLL | свой BC7-декодер, сверен на 40 млн блоков | `63bde8a` |
| `__VERSION__` в окне «О программе» | `_MSC_FULL_VER` | `4f9eb52` |
| Глобальные `/wd4244 /wd4267 …`, поставленные при порте | убраны; исправлены 23 предупреждения | `17a05a4`…`ead033e` |
| Пресет `debug`: C4702 в трёх местах | поиск первого элемента без `break` | `8a4ea91` |
| Debug: vcpkg кладёт `TracyClient.dll` в `debug/bin/Debug`, exe не стартуют | копирование DLL активной конфигурации | `71dc04f` |
| Debug: выход за границы вектора внутри glslang 16.2.0 (`GlslangToSpv.cpp:6201`), отладочные итераторы MSVC роняют каждый шейдер с вложенными buffer-блоками | overlay-порт glslang с проверкой границ | `faf25e1` |

Итоговый набор флагов MSVC: `/W4 /permissive- /utf-8 /wd4100 /wd4127 /wd4201 /wd4324` + `/WX` (вне `samples/`).

### Неопределённое и неуточнённое поведение, которое скрывал clang/libc++
| Баг | Где | Коммит |
| --- | --- | --- |
| Порядок вычисления аргументов: `f(childRef(c, path), std::move(path))` — чтение полей компонентов из Lua давало nil | `gameplay/src/lua_bindings.cpp` | `63bde8a` |
| То же в генерации травы и шума огня, в сценах GPU-тестов | `vegetation_assets.cpp`, генератор, `gpu_driven_tests.cpp` | `4332b71`, `1d0b778`, `68d0bff` |
| Range-for по `.items()` временного json — бесконечный цикл при сохранении настроек проекта в редакторе | `editor/src/core/project.cpp` | `8e24515` |
| `std::uniform_*_distribution` различаются между STL — другая карта нормалей воды | `water_feature.cpp` | `4332b71` |
| `%` с отрицательным операндом в GLSL: шов в облаках, `oxDdgiMod` | `noise_gen.comp`, `ddgi.glsl` | `4332b71`, `3228c21` |
| Удаление и замена открытых файлов; общее имя `<path>.tmp` у параллельных процессов | генератор, `writeFileAtomic`, кэш SPIR-V, аудио-тест | `1d0b778`, `d43d5f2`, `63bde8a` |
| Хеш `InstanceKey` клал все сущности в одну корзину на MSVC STL (`updateInstances` 177 мс на 10k) | `gpu_scene.cpp` | `68d0bff` |

### Баги, найденные первым исполнением на NVIDIA
| Баг | Коммит |
| --- | --- |
| DLSS: устройство создавалось без расширений NGX; утечка объектов NGX и segfault при выходе | `7e6f08a`, `92696ed` |
| Async compute: барьеры на compute-очереди с графическими stage-масками; перекрытие с графикой было нулевым | `68d0bff` |
| RT: `activeThisFrame()` всегда false после кадра | `7cf2689` |
| RT: промахи лучей DDGI и отражений не учитывали `ambientIntensity × r.IBL.Intensity` (RT удваивал фон) | `2160547` |
| RT: шлейф от тени движущегося объекта на 32 кадра | `14e1131` |
| RT: SBT начиналась с невыровненного адреса | `7f28823` |
| RT: три теста с физически невозможными сценами | `c1083aa` |
| Плеер: `--cvar` затирался пользовательскими настройками, `--width/--height` не влияли на headless | `88f48b1` |
| Редактор: авто-определение качества игнорировало провайдер возможностей | `8e24515` |

Эталонные изображения не перегенерировались. Один допуск ослаблен обоснованно:
`GpuDrivenTest.TextureStreamingRespectsBudget` — 5 % плохих пикселей (измерено 3,66 %) из-за выбора LOD при
анизотропии 16× у NVIDIA. `oxshowcase_generate --check` сравнивает 26 бинарных ассетов с жёстким числовым допуском
(отличия в последнем бите от libm и FMA), сами ассеты не менялись.

## 3. Тесты

| Пресет | Всего | Прошло | Упало | Пропущено |
| --- | --- | --- | --- | --- |
| `dev` (RelWithDebInfo, `/WX`) | 1249 | 1246 | 0 | 3 |
| `debug` (`/WX`) | 1249 | 1245 | 0 | 4 |

Пресет `debug`: в последнем полном прогоне машина уходила в сон, 9 тестов упали по таймауту (один — по сравнению
изображений посреди сна); при повторном запуске все 9 прошли.

Пропуски:
- `RayTracingTest.RayTracingCheckboxIsInertWithoutSupport` — проверяет поведение без RT, на этой машине RT есть;
- `DeviceLifecycle.SurfaceDeviceAfterHeadlessDevice` — драйвер NVIDIA на Windows не даёт `VK_EXT_headless_surface`;
- `UiEngine.WindowedScreenshot` — headless-запуск;
- только в `debug`: `GpuDrivenTest.SteadyStateExtractAndDrawListsDoNotAllocate` — отладочные контейнеры MSVC
  выделяют прокси-объекты, счётчик аллокаций не имеет смысла.

Все 12 тестов из раздела 4 плана выполняются и проходят.

## 4. Ray tracing

Валидация включена, ошибок нет. Synchronization validation (`VK_LAYER_VALIDATE_SYNC=1`) по `RayTracingTest.*` — без
замечаний. Скриншоты: [docs/guide/images/rtx/](../../guide/images/rtx/).

| Эффект | Статус | Чем проверено |
| --- | --- | --- |
| Тени солнца | работает | тест против shadow maps, станция 1 |
| Тени локальных источников | работает | `RtLocalLightShadowsWithoutRestir` |
| Alpha-test и цветные тени | работает | `RtShadowsOfAlphaTestedAndTintedCasters` |
| ReSTIR, 32 источника | работает | яркость в тени 0,206 против 0,203 у path tracer |
| AO | работает | `RtAmbientOcclusionDarkensContacts` |
| Отражения | работает | тест, станции 3 и 13 (объекты вне экрана) |
| GI (DDGI) | работает с ограничением | перенос цвета 0,052 против 0,078 у path tracer; детали мельче шага проб (~2 м) не разрешаются |
| Преломление | правдоподобно | тест, изображение |
| RT-волюметрика | работает | `RtVolumetricFogVisibility` |
| Path tracer, режимы 0 и 1 | работает | режимы совпадают: 0,67/255 при 512 spp |
| Refit BLAS для скиннинга | работает | `SkinningTest.RayTracedShadowFollowsAnimation` |
| TLAS с движущимися объектами | без утечек | 300 кадров, VRAM 584,3 → 584,3 МБ |
| Переключение `r.RayTracing` на лету | без утечек | `RuntimeToggleRebuildsGraphWithoutLeaks` |

Замеры (`OX_RT_PERF=1 OX_VULKAN_VALIDATION=0`, тест `RayTracingTest.PerfReportRayTracing`; 250 инстансов, солнце,
8 локальных источников с тенями, стекло, туман):

| Проход, мс | 1080p | 4K |
| --- | --- | --- |
| Растровый кадр | 1,40 | 3,28 |
| **Гибридный RT-кадр** | **9,08** | **31,0** |
| RT.BuildAS (обновление TLAS) | 0,014 | 0,014 |
| RT.Shadows (солнце + 4 локальных) | 1,02 | 3,94 |
| Денойз теней, солнце / локальные | 1,39 / 1,38 | 5,52 / 5,38 |
| RT.AO / денойз | 0,09 / 1,36 | 0,35 / 5,27 |
| RT.Reflections / денойз | 0,22 / 1,29 | 0,84 / 5,15 |
| DDGI | 1,31 | 1,98 |
| RT.Translucency | 0,20 | 0,76 |
| ReSTIR (initial + spatial) | 0,92 | 3,66 |
| RT.PathTrace, режим 0 / режим 1, 1 spp | 1,33 / 1,41 | 5,50 / 5,71 |
| Кадр path tracer | 10,3 | 37,0 |
| VRAM, гибрид / path tracer | 1,24 / 1,5 ГБ | 3,4 / 4,1 ГБ |

Четыре SVGF-денойзера занимают около 60 % времени RT при 1080p и 70 % при 4K.

Тестовый проект: станции 01, 02, 03, 04, 08, 13 отрендерены headless в 1920×1080 с RT и без, валидация включена,
ошибок нет. Замечен зернистый шум на шероховатых металлах и вокруг эмиссивных объектов.

## 5. DLSS

`PostProcessTest.DlssRendersOnRtx` выполняется и проходит. NGX SDK 310.9.1.

| Режим | Внутреннее разрешение (вывод 960×540) | Mip bias |
| --- | --- | --- |
| Ultra Performance | 320×180 | −2,585 |
| Performance | 480×270 | −2,000 |
| Balanced | 557×313 | −1,785 |
| Quality | 640×360 | −1,585 |
| DLAA | 960×540 | −1,000 |

**Решение по jitter (пункт бэклога закрыт): менять ничего не пришлось.** `RenderView::jitterPixels()` передаётся как
есть. PSNR против 16× SSAA на статичной сцене: как есть 40,58 дБ (TAAU 39,22); остальные знаки 25–31 дБ с мерцанием.
Векторы движения: `uvCur − uvPrev`, без jitter, `InMVScale = (−renderW, −renderH)` — 31,04 дБ против 28,6–28,8 при
перевёрнутой оси. `DepthInverted` и `IsHDR` — артефактов нет. Экспозиция: своя текстура + `InPreExposure` 40,56 дБ,
авто-экспозиция NGX 41,59 дБ. `InSharpness` в NGX устарел и игнорируется.

Dev-библиотека NGX в Debug подхватывается: она впечатывает в кадр баннер «…CONTACT NVIDIA TO OBTAIN DLLs FOR YOUR
TITLE». Отладочный оверлей горячими клавишами не проверялся (нужен интерактивный запуск).

Кадр 4K (3840×2160, плеер headless, качество High, RT выключен, валидация выключена), GPU-время кадра, мс:

| Режим | Станция 4 «Волюметрика» | Станция 6 «Мир» |
| --- | --- | --- |
| Off | 37–39 | 24,7 |
| TAA native | 38–41 | 26,1 |
| FSR1 Quality | 21,0–21,7 | нестабильно: 52,8 и 58,9 |
| TAAU Quality | 21,0 | нестабильно: 14,1 и 50,5 |
| DLSS Quality | 23,1–23,9 | 16,4 |
| DLSS Performance | 18,2–18,5 | 14,6 |

Станция 4 — три прогона, разброс указан. Станция 6 — мгновенное значение из оверлея последнего кадра; FSR1 и TAAU
при одних и тех же настройках дают то ~14 мс, то 50–59 мс. Причина не установлена (см. раздел 7).

## 6. GPU-driven, mesh-шейдеры, редактор

- Mesh-шейдеры и `drawIndirectCount` реально используются: счётчики `meshShaderDrawCalls` и
  `indirectCountDrawCalls` проверяются в тестах. Изображение mesh-пути совпадает с compute-meshlet путём.
- Стресс-сцена, 1080p, валидация включена, CPU был занят сборками (CPU-цифры шумные): 10k инстансов с HiZ — 2,26 мс
  GPU (M4 Pro: 8,04); 50k — 8,3 мс (M4 Pro: 24,03).
- `r.AsyncCompute` 0 → 1: 8,06 → 7,99 … 8,65 → 8,19 мс на 50k — небольшой стабильный выигрыш; `Auto` его уже
  включает вне MoltenVK. `r.ParallelRecording` — без убедительного выигрыша; эвристика `Auto` не менялась.
- Редактор: тесты 4 из 4 (три прогона подряд), открывает тестовый проект с Vulkan-viewport, закрывается без ошибок
  валидации.

## 7. Открытые проблемы

Не сделано из плана:
- интерактивные проверки в окне: переключение RT 10 раз из меню настроек, ресайз окна с DLSS, отладочный оверлей
  dev-DLL, тур `--tour` в окне (headless-тест `Showcase.Tour` проходит), перетаскивание ассетов в редактор;
- захват Nsight Graphics (число инстансов TLAS, размеры BLAS после компакции);
- Linux — код surface для xcb и ветки CMake написаны, ни разу не собирались;
- перепроверка на macOS: исправления детерминизма воспроизводят поведение clang/libc++ по построению, но три эталона
  (вода, облака, растительность), `oxshowcase_generate --check` и новые тесты планировщика графа на Mac не гонялись.

Найдено и не исправлено:
- станция 6 в 4K: FSR1 и TAAU то ~14 мс, то 50–59 мс. Воспроизведение: `C:\…\OxwaldPlayer --project
  samples\OxwaldShowcase --headless --width 3840 --height 2160 --scene
  project://Assets/Scenes/Stations/06_World.oxscene --frames 300 --cvar r.AntiAliasing=2 --cvar r.Upscaler=1 --cvar
  r.Upscaler.Quality=3 --screenshot out.png -- --photo`, несколько запусков;
- станция 4 в 4K без апскейлера — 37–39 мс GPU: волюметрика не масштабируется с разрешением, не разбиралось;
- GPU-assisted validation: «Descriptor index N references a resource that was destroyed» в `IBL.Generate`
  (`ibl/prefilter.comp`), эпизодически в `HiZ.Early` и `GpuCull.Late`. Причина не найдена;
- GPU-assisted validation: VUID 12281 при перестройке деформируемого BLAS — по результатам экспериментов дефект слоя
  1.4.341 (теряет последнюю созданную AS из таблицы адресов при уничтожении другой), не движка;
- первая сборка BLAS скиннингового меша, по чтению кода, может идти до compute-скиннинга этого кадра;
- денойзеры AO и отражений отстают при движении (обрезка истории применена только к теням);
- гибридные RT-проходы продолжают работать под path tracer (~8 мс при 1080p впустую);
- зернистый шум RT-отражений на шероховатых металлах;
- не-ASCII пути вне ANSI-кодировки: ~170 вызовов `path.string()`, `fopen`, stb, assimp;
- патч glslang — локальный overlay-порт; в upstream не отправлялся;
- остальное — в [BACKLOG.md](../BACKLOG.md), разделы «Found during the Windows/RTX bring-up» и «Ray tracing: open
  after the first hardware run».
