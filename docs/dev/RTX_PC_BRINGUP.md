# План: запуск OxwaldEngine на ПК с NVIDIA RTX

Инструкция для агента (или человека), который впервые собирает движок на Windows/Linux-машине с видеокартой
NVIDIA RTX. До этого движок разрабатывался и тестировался **только на macOS (Apple M4 Pro, MoltenVK)**. Там нет
аппаратной трассировки лучей, DLSS и mesh-шейдеров, поэтому соответствующий код ни разу не исполнялся, а сборка под
Windows и Linux ни разу не запускалась.

Цели в порядке приоритета:
1. Собрать всё (движок, тесты, плеер, редактор, тестовый проект) и добиться зелёного `ctest`.
2. Проверить **ray tracing** (все эффекты, path tracer, ReSTIR, денойзер на реальных лучах).
3. Проверить **DLSS** (NGX): инициализацию, все режимы качества, DLAA, знак jitter.
4. Проверить mesh-шейдеры и `drawIndirectCount` (на macOS тоже недоступны).
5. Зафиксировать найденное: исправления, цифры производительности, документацию.

Перед началом прочитай:
- [docs/dev/ARCHITECTURE.md](ARCHITECTURE.md) — устройство и правила кода;
- [docs/dev/modules/render.md](modules/render.md) — §11 «Ray tracing» (в конце есть ручной чек-лист) и §13 GPU-driven;
- [docs/dev/modules/render_postprocess.md](modules/render_postprocess.md) — апскейлеры и DLSS;
- [docs/dev/BACKLOG.md](BACKLOG.md) — известные ограничения;
- [README.md](../../README.md) — быстрый старт.

---

## 0. Правила работы

- Ветка `oxwald-engine`. Коммить логическими шагами. Подпись коммитов — как в истории ветки.
- Не ломай macOS: всё платформенное — под `if(WIN32)` / `#if defined(_WIN32)` / `__linux__`. Код, общий для
  платформ, правь так, чтобы он оставался корректным и для MoltenVK.
- **Не перегенерируй golden-изображения вслепую.** На NVIDIA растровые картинки могут слегка отличаться от
  эталонов с Mac. Если тест падает по сравнению с эталоном, сначала открой оба PNG и сравни глазами. Если разница
  — допустимые различия железа (точность, фильтрация), ослабь допуск или заведи отдельный эталон для платформы.
  Если это баг — исправь его.
- Тесты, которым нужно отсутствующее железо, делают `GTEST_SKIP()`. На RTX-машине RT- и DLSS-тесты **должны
  перестать пропускаться**. Если какой-то из них всё ещё пропускается — выясни причину; это и есть основная работа.
- Все находки, которые не исправлены сразу, — в `docs/dev/BACKLOG.md` с точными шагами воспроизведения.

---

## 1. Окружение

### Windows (основной сценарий)

1. Драйвер NVIDIA: свежий Game Ready или Studio, с поддержкой Vulkan 1.3 и `VK_KHR_ray_query`.
   Проверка: `vulkaninfo --summary` (из LunarG Vulkan SDK, ставить его необязательно — loader и validation layers
   приходят из vcpkg) или NVIDIA Control Panel → System Information.
2. **Visual Studio 2022** (17.10+) с workload «Desktop development with C++», компонентами MSVC v143, Windows SDK,
   C++ CMake tools.
3. **CMake ≥ 3.25** и **Ninja** (они есть в VS; или `winget install Kitware.CMake Ninja-build.Ninja`).
4. **Git** и **Git LFS**: `git lfs install`. Это обязательно: overlay-порт `nvidia-dlss` клонирует
   `NVIDIA/DLSS` с бинарниками в LFS.
5. **vcpkg**: `git clone https://github.com/microsoft/vcpkg && .\vcpkg\bootstrap-vcpkg.bat`, затем задать
   переменную окружения `VCPKG_ROOT`. В `vcpkg.json` зафиксирован `builtin-baseline`
   `0b88aacde46a853151730fbe7d0b7ee45f4b6864`: клон vcpkg должен содержать этот коммит (свежий `git pull` подойдёт).
6. Python 3 в PATH (нужен некоторым портам) и PowerShell 7 желательно.
7. Все команды ниже запускать из **«x64 Native Tools Command Prompt for VS 2022»** или Developer PowerShell —
   Ninja должен видеть `cl.exe`.

### Linux (если машина на Linux)

- Ubuntu 24.04 или аналог, проприетарный драйвер NVIDIA ≥ 550, `build-essential`, `clang` или `gcc` ≥ 13,
  `cmake`, `ninja-build`, `git-lfs`, `pkg-config`, `autoconf autoconf-archive automake libtool`, плюс dev-пакеты для
  Qt и GLFW: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
  libxkbcommon-x11-dev libwayland-dev libgl1-mesa-dev '^libxcb.*-dev'`.
- Триплет `x64-linux` (DLSS-рантайм для Linux в overlay-порте есть: `libnvsdk_ngx.a` и `libnvidia-ngx-dlss*.so`).

---

## 2. Установка зависимостей

`tools/bootstrap.sh` написан для macOS/Linux (bash). На Windows зависимости ставятся напрямую:

```bat
cd <repo>
%VCPKG_ROOT%\vcpkg install --x-install-root=vcpkg_installed --triplet x64-windows
```

На Linux: `tools/bootstrap.sh` (он вызовет vcpkg с триплетом по умолчанию).

Ожидаемые проблемы и что делать:

| Симптом | Что делать |
| --- | --- |
| `nvidia-dlss` падает при `vcpkg_from_git ... LFS` | Проверь `git lfs install`. Если версия vcpkg не поддерживает параметр `LFS` у `vcpkg_from_git`, переделай порт на скачивание нужных файлов (`lib/Windows_x86_64/x64/nvsdk_ngx_d*.lib`, `lib/Windows_x86_64/{rel,dev}/nvngx_dlss*.dll`) через `vcpkg_download_distfile` с `https://media.githubusercontent.com/media/NVIDIA/DLSS/v310.9.1/<path>` и SHA512. |
| В `vcpkg-overlays/ports/nvidia-dlss/portfile.cmake` указан `REF` — коммит тега `v310.9.1` | Если vcpkg ругается на REF или FETCH_REF, проверь коммит: `git ls-remote https://github.com/NVIDIA/DLSS refs/tags/v310.9.1^{}`. |
| `qtbase` собирается долго | Нормально, 20–40 минут. Фича `editor` в `vcpkg.json` включена по умолчанию. Для первой сборки движка без редактора: `--x-no-default-features`. |
| Overlay-порт `vulkan` или `moltenvk` | На Windows/Linux `moltenvk` не ставится (`"platform": "osx"`). Overlay-стаб `vulkan` ничего лишнего не тянет. |
| Порт падает из-за путей с пробелами или длинных путей | Клонируй репозиторий в короткий путь без пробелов (`C:\dev\ox`), включи `git config --system core.longpaths true`. |

Сохрани время и итог установки в отчёт.

---

## 3. Первая сборка

```bat
cmake --preset dev -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build --preset dev
```

Пресеты используют Ninja, toolchain vcpkg и общий `vcpkg_installed/`. В пресетах `dev`/`debug` включён
**`OX_WARNINGS_AS_ERRORS=ON`**. MSVC выдаст предупреждения, которых не было у clang, поэтому стратегия такая:

1. Первая сборка: `-DOX_WARNINGS_AS_ERRORS=OFF`. Добейся, чтобы всё собиралось и линковалось.
2. Затем исправь предупреждения MSVC (`/W4`) в нашем коде и верни `ON`. Подавлять разрешено только
   предупреждения из сторонних заголовков и только точечно.

Вероятные места проблем — проверь в первую очередь:

- **Специфика компилятора.** Код писался под AppleClang 17 / libc++. Ожидай мелочей: `std::format` с
  пользовательскими типами, неявные сужения (C4244/C4267), `[[nodiscard]]`, designated initializers в другом
  порядке, `constexpr`-различия, `__PRETTY_FUNCTION__`, `__builtin_*`, атрибуты `__attribute__`,
  `#pragma clang`, VLA, `ssize_t`, `<unistd.h>`, POSIX-функции (`setenv` vs `_putenv_s`, `fsync`, `rename` поверх
  существующего файла, который на Windows не перезаписывается, — проверь атомарную запись сохранений и
  `pipelineCache`).
- **`min`/`max` из `windows.h`.** Нужен `NOMINMAX` (определи его глобально для Windows в
  `cmake/OxwaldHelpers.cmake`, если где-то включается `windows.h`).
- **Кодировка.** `/utf-8` уже стоит в `ox_set_warnings`; русские строки в Lua, RmlUi и редакторе должны работать.
  Проверь, что пути к файлам с не-ASCII символами открываются (`std::filesystem::path` → `.u8string()`).
- **Jolt.** Модуль физики собирается с `-fno-rtti` под clang. Для MSVC эквивалент — `/GR-`, но смешивать с
  RTTI-кодом осторожно: посмотри, как vcpkg собрал Jolt (`JPH_*` define'ы в `share/joltphysics`), и повтори логику из
  `engine/physics/CMakeLists.txt` для MSVC. `JPH::VerifyJoltVersionID()` должен проходить.
- **Деплой рантайма Vulkan.** `ox_deploy_vulkan_runtime()` в `engine/rhi/cmake/OxwaldVulkanRuntime.cmake` имеет
  ветку `WIN32`: проверь, что рядом с exe оказываются `vulkan-1.dll` из vcpkg, validation layer
  (`VkLayer_khronos_validation.dll` + json с правильным `library_path`) и DLL Qt (applocal-деплой vcpkg).
  NVIDIA ICD приходит из драйвера через реестр — его копировать не нужно.
- **NGX DLL.** Проверь, что `nvngx_dlss.dll` (rel для Release/RelWithDebInfo, dev для Debug) копируется рядом с
  exe (переменная `NVIDIA_DLSS_RUNTIME_FILES` в конфиге порта), а `dlss_ngx.cpp` передаёт каталог exe в NGX как
  feature path.
- **Редактор, Vulkan-viewport.** `editor/src/viewport/vulkan_viewport.cpp` создаёт surface **только для macOS**
  (Metal). Нужно реализовать Windows-ветку: `VK_KHR_win32_surface` в `requiredInstanceExtensions()`,
  `vkCreateWin32SurfaceKHR` с `HWND = reinterpret_cast<HWND>(window->winId())` и `GetModuleHandle(nullptr)`.
  Для Linux — `VK_KHR_xcb_surface`/`VK_KHR_wayland_surface` через `QNativeInterface`. Альтернатива — использовать
  `QVulkanInstance::surfaceForWindow()` (кроссплатформенно), передав Qt наш `VkInstance` через
  `QVulkanInstance::setVkInstance`. Выбери более простой путь и проверь ресайз и HiDPI.
- **Скрипты сборки на bash.** `tools/bootstrap.sh` и, возможно, отдельные custom-команды CMake могут вызывать
  POSIX-утилиты. Всё, что запускается из CMake, должно работать через `${CMAKE_COMMAND} -E`.
- **Codesign-шаги** в `editor/CMakeLists.txt` и rhi-деплое должны быть под `if(APPLE)` — проверь.
- **Тестовый проект и генератор** (`samples/OxwaldShowcase/Tools`): `--check` должен показывать
  `0 differences` и на Windows. Если отличаются переводы строк в сгенерированных JSON/Lua, проверь
  `.gitattributes`: для ассетов нужен `eol=lf`, добавь при необходимости.

Каждый класс исправлений — отдельный коммит: `Windows: fix MSVC build of <module>`, `Windows: Vulkan surface for
editor viewport` и т.д.

---

## 4. Тесты: общий прогон

```bat
ctest --preset dev -j8 --output-on-failure
```

Ожидается, что все тесты (на macOS их 1233) проходят. Разбери каждое падение: это реальный баг, различие
платформ или нестабильный тест. Затем проверь, что **пропусков стало меньше**:

```bat
ctest --preset dev -j8 2>&1 | findstr /C:"Skipped"
```

Эти тесты на RTX-машине обязаны **выполниться, а не пропуститься**:

| Тест | Что проверяет |
| --- | --- |
| `AccelerationStructures.BuildBlasAndTlasWhenSupported` | построение BLAS/TLAS в rhi |
| `GuideRhiDevice.RayTracingOnlyWithCaps` | пример из гайда, гл. 17 |
| `RayTracingTest.RtShadowsMatchShadowMapsLoosely` | RT-тени против shadow maps |
| `RayTracingTest.RtAmbientOcclusionDarkensContacts` | RTAO |
| `RayTracingTest.RtReflectionsSeeOffscreenObjects` | RT-отражения видят объекты за кадром |
| `RayTracingTest.RtGlobalIlluminationBleedsColour` | DDGI, перенос цвета |
| `RayTracingTest.RtRefractionThroughGlass` | преломление в стекле |
| `RayTracingTest.PathTracerConvergesAndResetsOnCameraChange` | path tracer |
| `RayTracingTest.RestirShadowsManyLights` | ReSTIR DI |
| `RayTracingTest.RuntimeToggleRebuildsGraphWithoutLeaks` | переключение `r.RayTracing` на лету без утечек |
| `PostProcessTest.DlssRendersOnRtx` | DLSS через NGX |
| `GpuDrivenTest.MeshShaderPathCompilesAndMatchesWhenSupported` | mesh/task-шейдеры |

Все остальные RT-тесты (денойзер, DDGI-математика, ReSTIR-резервуары, CPU-логика TLAS/BLAS) уже проходили на Mac —
они должны остаться зелёными.

Если тест падает **с ошибкой валидации**, это приоритет №1: RT-код не исполнялся нигде. Включи
`OX_VULKAN_VALIDATION=1`, при необходимости GPU-assisted validation (`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT`)
и synchronization validation.

Картинки RT-тестов пишутся в `%TEMP%\oxwald_render_out\` (например, `rt_shadows.png`). **Открой и посмотри каждую.**
Тест может пройти по свободному допуску, но всё равно выглядеть неправильно.

---

## 5. Ray tracing: ручная проверка

Выполни **чек-лист из 10 пунктов в [render.md §11](modules/render.md)** («Manual checklist on an NVIDIA RTX
machine»). Удобнее всего делать это на тестовом проекте:

```bat
build\dev\bin\OxwaldPlayer.exe --project samples\OxwaldShowcase --scene project://Assets/Scenes/Stations/13_RTX.oxscene
```

На станции 13 «RTX и апскейлеры» переключатели `[R]`, `[1]`–`[5]` должны быть **активны**, а справа — GPU-тайминги
RT-проходов. Также проверь:

- **Переключение на лету**: в редакторе (Project Settings → Rendering) и в игре (Esc → Настройки → Графика)
  галочка «Трассировка лучей» должна быть активна. Переключи её 10 раз подряд: без падений, VRAM стабильна (F1 →
  Stats), граф пересобирается за кадр.
- **Каждый эффект по отдельности** (`r.RayTracing.Shadows/Reflections/AO/GI/Translucency/Volumetrics`) против
  растровой версии: сделай пары скриншотов (`--screenshot`, см. `samples/OxwaldShowcase/README.md`) на станциях
  1 (свет), 2 (материалы, стекло), 3 (отражения), 4 (волюметрика).
- **Path tracer**: `r.PathTracing 1` в консоли (F1 или `~`), в обоих режимах `r.PathTracing.Mode 0/1`
  (ray query и RT-пайплайн с SBT). Изображение сходится и сбрасывается при движении.
- **Скиннинг и refit BLAS**: станция 8 «Анимация». Тень манекена должна следовать анимации.
- **Качество**: пресеты Low/Medium/High/Ultra группы `Ray Tracing` и кнопка Auto-Detect. На RTX Auto должен
  включать RT и DLSS там, где это задумано (`render::recommendedSettings`).
- **Nsight Graphics** (если установлен): захват кадра, число инстансов TLAS, размеры BLAS после компакции.

Для каждого эффекта запиши в отчёт: работает / баг (с описанием и скриншотом) / время прохода в мс при 1080p и 4K.

---

## 6. DLSS: проверка

Код — `engine/render/src/features/postprocess/upscalers/dlss_ngx.cpp`. API доступности —
`render::upscalerAvailability()`.

1. **Доступность**: в редакторе и в меню игры DLSS должен быть активен (не серым). В логе при старте должны быть
   строки инициализации NGX без ошибок. Если NGX не находит DLL — проверь деплой (раздел 3) и feature path.
2. **Тест** `PostProcessTest.DlssRendersOnRtx` проходит, картинка в `%TEMP%\oxwald_render_out\` выглядит правильно.
3. **Режимы качества**: Ultra Performance / Performance / Balanced / Quality / **DLAA**. Проверь, что внутреннее
   разрешение соответствует режиму (F1 → Stats, строка «Рендер / вывод» на станции 13) и что mip bias меняется.
4. **Знак jitter — важно** (открытый пункт бэклога). Собери в Debug, чтобы подхватились **dev-DLL** NGX: они
   показывают отладочный оверлей DLSS. Включи визуализацию jitter и motion vectors горячими клавишами
   dev-библиотеки (см. «DLSS Programming Guide» в `doc/` SDK, раздел о debug overlay). Признаки неверного знака
   jitter или векторов движения: дрожание или «плавание» статичной картинки, размазывание при движении камеры.
   Исправь флаги и знаки в `dlss_ngx.cpp` и задокументируй решение.
5. **Reversed-Z и HDR**: флаги `DepthInverted` и `IsHDR` уже выставлены — проверь отсутствие артефактов на
   границах объектов и на ярких источниках. Auto-exposure: сравни режим, где DLSS считает экспозицию сам, с режимом,
   где мы передаём свою (`Exposure`).
6. **Ресайз окна и смена режима на лету**: фича NGX пересоздаётся без утечек и падений.
7. **Производительность**: замерь кадр 4K на станциях 4 (волюметрика) и 6 (мир) для Off / TAA native / FSR1 /
   TAAU / DLSS Quality / DLSS Performance. Запиши в `docs/dev/perf.md` новым разделом «NVIDIA RTX <модель>».

---

## 7. GPU-driven, mesh-шейдеры, остальное

- `r.GpuDriven.Meshlets 1` и mesh-шейдерный путь (если GPU поддерживает `VK_EXT_mesh_shader`): изображение
  совпадает с instanced-путём, тест `MeshShaderPathCompilesAndMatchesWhenSupported` проходит.
- `drawIndirectCount` теперь доступен: проверь, что GPU-driven путь его использует, а не путь с паддингом
  (см. render.md §13), и сравни время.
- **Async compute и параллельная запись команд**: на MoltenVK режим `Auto` их выключает, потому что там нет
  выигрыша. На NVIDIA замерь `r.AsyncCompute 0/1` и `r.ParallelRecording 0/1` и поправь эвристику `Auto`, если
  выигрыш есть.
- Стресс-сцены из `docs/dev/perf.md` (10k / 50k инстансов): добавь цифры для RTX.
- Пройди весь тестовый проект в окне: `OxwaldPlayer --project samples\OxwaldShowcase -- --tour`. Это тур по всем
  станциям; смотри, нет ли падений, ошибок валидации или визуальных артефактов.
- Редактор: открой тестовый проект, Play / Stop на нескольких станциях, импорт ассета перетаскиванием,
  Project Settings → Scalability → Auto-Detect.

---

## 8. Документация и отчёт

Обнови по результатам:
- `docs/dev/modules/render.md` §11: отметь, что проверено на RTX и на какой карте и драйвере, исправленные баги,
  цифры по проходам.
- `docs/dev/modules/render_postprocess.md`: решение по знаку jitter, цифры DLSS.
- `docs/guide/24-ray-tracing.md` и `docs/guide/25-upscalers-postprocess.md`: убрать оговорки «не проверено на
  железе», если всё подтвердилось; добавить скриншоты RT и DLSS в `docs/guide/images/` (например,
  `rtx/rt_on_off_*.png`).
- `docs/guide/00-getting-started.md` и `README.md`: раздел про сборку на Windows и Linux с реальными шагами
  (что пришлось поставить, сколько заняло).
- `docs/dev/BACKLOG.md`: закрыть подтверждённые пункты (DLSS jitter и т.п.), добавить новые находки.
- `docs/dev/perf.md`: раздел про RTX-машину.
- `CHANGELOG.md`: запись «0.1.1 — Windows/RTX bring-up».

Итоговый отчёт — новым файлом `docs/dev/reports/rtx-bringup-<дата>.md`:
- железо и ОС (GPU, драйвер, CPU, версия Windows или дистрибутив), версии VS/CMake/vcpkg;
- что сломалось при сборке и как исправлено (по пунктам, со ссылками на коммиты);
- результат `ctest`: сколько прошло, упало, пропущено, и почему пропущенные пропускаются;
- таблица RT-эффектов: статус, мс @1080p и @4K, ссылки на скриншоты;
- таблица DLSS: режимы, внутреннее разрешение, мс, качество на глаз, решение по jitter;
- открытые проблемы.

---

## 9. Коммит и пуш

```bat
git status
git add -A
git commit -m "Windows/RTX bring-up: <кратко>"
git push origin oxwald-engine
```

Коммить по ходу работы (раздел 3 — по классам исправлений, затем RT, DLSS, документация), а не одним
коммитом в конце. Перед финальным пушем — ещё раз полный `ctest` и короткий запуск тестового проекта в окне.
