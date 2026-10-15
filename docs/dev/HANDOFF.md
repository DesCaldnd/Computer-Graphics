# Передача работы следующей сессии (состояние на 2026-10-14, ~19:30; таблица §2 и §3 обновлены 2026-10-15)

Сессия 2026-10-14 вела параллельно 11 задач силами агентов. Часть задач завершена, часть остановлена посередине из-за
лимита токенов. **Ничего не закоммичено**: все изменения лежат незакоммиченными в рабочем дереве ветки
`oxwald-engine` (последний коммит `22bb939`). Задача следующей сессии — доделать открытые пункты, провести финальную
сборку и полный прогон тестов, затем закоммитить по областям.

Подробные заметки агентов по каждой незавершённой области — в [`handoff/`](handoff/):
[android](handoff/handoff-android.md), [dlssrr](handoff/handoff-dlssrr.md), [latency](handoff/handoff-latency.md)
(Reflex + FSR 3), [neural](handoff/handoff-neural.md), [vrs](handoff/handoff-vrs.md). **Прочитайте заметку своей
области целиком перед началом работы** — там списки файлов, временные хаки и точные следующие шаги.

## 1. Окружение и правила (обязательно)

- **Всё большое — на `E:\Development`**, диск C: (SSD) почти заполнен. Новые папки сборки:
  `cmake --preset dev -B E:/Development/OxwaldEngine/build/<имя> -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_TARGET_TRIPLET=x64-windows`.
  Старые пути (`build/dev`, `build/debug`, `build/release`, `build/perf`, …) — junction-ссылки на `E:`; кэши vcpkg
  (`C:\Programming\vcpkg\{buildtrees,packages,downloads}`, `%LOCALAPPDATA%\vcpkg`) тоже junction на `E:\Development\vcpkg`.
- `F:\Development` (SSD) пользователь разрешил **только для временных файлов** сборки vcpkg под Android
  (`--x-buildtrees-root/--x-packages-root`); после сборки их **удалить**. `F:\Development\AI` не трогать — не наше.
  Сейчас F: очищен (временные файлы x64-android удалены; для arm64 их придётся создать заново и снова удалить).
- MSVC: `call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"` перед cmake.
- 8 ядер / 32 ГБ: при нескольких параллельных сборках — `-j 8` максимум каждой.
- Файлы, которые пользователь должен открыть сам (PDF, отчёты), кладите вне `build/` — его клиент не видит `build/`.
- Каждое изменение покрывать тестами; GPU-тесты с меткой `gpu`, `GTEST_SKIP()` без поддержки железа. Доки
  руководства — на русском. Смотреть каждую созданную эталонную картинку (Read).
- Железо: RTX 3080 (драйвер 617.42), i9-9900K (iGPU UHD 630 отключён в BIOS), монитор 5120×1440 @ 100 Гц.
- Без коммитов от агентов: коммитит координатор после проверки.

### Урок этой сессии
Агенты правили общее дерево одновременно, и недописанные файлы одного ломали сборку остальным (литеральные переводы
строк в `device_caps.cpp`, `neural/network.cpp`, `showcase_vrs_tests.cpp`). В следующей сессии:
доделывайте области **последовательно** или давайте агентам непересекающиеся файлы; правки общих файлов
(`renderer.cpp`, `device_caps.*`, `render_graph.*`, `runtime_renderer.cpp`, `settings.cpp`, `vcpkg.json`, корневой
`CMakeLists.txt`) — маленькими атомарными заменами, с повторным чтением файла перед правкой; не оставлять файл
некомпилируемым между вызовами инструментов.

## 2. Статус задач

| # | Задача | Статус | Что осталось |
|---|---|---|---|
| 1 | Скрипт PDF по всем докам (`tools/docs_pdf/`) | ✅ готово, 10 pytest | проверить ctest `docs_pdf.pytest` в полной сборке; перегенерировать PDF в конце (`docs/OxwaldEngine-docs.pdf`; по решению пользователя 2026-10-15 PDF **коммитится** и пушится вместе с доками) |
| 2 | Ориентированные гизмо (Local/World) | ✅ готово, `editor.GizmoTests` 12/12 | прогнать все тесты редактора в сборке **с модулем render** (`SettingsTests` падал только из-за отсутствия `r.*` cvar'ов в урезанной сборке) |
| 3 | Оптимизация редактора (13–15 → 112–117 fps) | ✅ готово | см. «Хвосты оптимизации» ниже; замер в пресете **Release** (пользователь мерил в Release) |
| 4 | Разрушаемость + станция 15 Showcase | ✅ готово, 26 тестов | `oxshowcase_generate --check` зелёный после всех областей; тесты редактора с `DestructibleComponent` |
| 5 | Сплит-рендеринг (`r.SplitRendering`) | ✅ готово, 31 тест | по умолчанию Off (на 3080 + UHD 630 невыгодно); замер на реальном iGPU — если пользователь включит его в BIOS (инструкция в guide 35) |
| 6 | DLSS Ray Reconstruction (`r.Denoiser`) | ✅ готово (2026-10-15) | — (замеры в `perf.md`, доки guide 24/25, `render_postprocess.md`, CHANGELOG) |
| 7 | Нейросжатие текстур (NTC) + инфраструктура MLP | ✅ готово (2026-10-15) | — (бенчмарки в `perf.md`, «Neural rendering») |
| 8 | Нейроденойзер (`r.Denoiser Neural`) | ✅ готово, экспериментальный | после сходимости истории размытее SVGF и ~3× дороже À-trous; в меню не показывается; следующий шаг — обучение на кадрах после временного прохода (BACKLOG) |
| 9 | VRS (`r.VRS*`) | ✅ готово (2026-10-15) | не замерен на GPU класса Low/Medium (BACKLOG) |
| 10 | Reflex (`r.Reflex`) + генерация кадров FSR 3 | ✅ готово, есть открытые пункты | замеры в `perf.md`, «Latency»; открыто (BACKLOG): JustInTime пересыпает при упоре в GPU, vsync без ограничения fps ~48 мс, защита темпа FSR 3 эмпирическая, строки Reflex/FG в редакторе не проверялись вручную; см. [handoff-latency](handoff/handoff-latency.md) |
| 11 | Android | 🟡 AVD проверен, arm64 собран | AVD: смоук-тест и LLDB пройдены (guide 37, «Проверено на AVD»); последний смоук (`build/android/smoke-final`) — код 3: не записался собственный скриншот плеера (`adb screencap` работает), перезапустить; APK arm64-v8a собран, на устройстве (OnePlus 12) не запускался |

### Хвосты оптимизации (готово, 2026-10-15)
- Гибридные RT-проходы под path tracer пропускают себя (10,3 → 1,7 мс при 1080p); растровая часть кадра ещё
  работает (BACKLOG).
- Станция 6 в 4K: «двухрежимное» время FSR1/TAAU — кванты других процессов на общем GPU, не дефект движка.
- Станция 4 в 4K: GTAO + SSR вполовину разрешения выше 1800 px, 12,9 → 8,4 мс.
- Вьюпорт редактора идёт с частотой экрана, на котором находится (144/165/240 Гц не ограничены 125 fps).

## 3. Известное состояние сборки и тестов

**2026-10-15, финальная проверка пройдена** (подробно — `handoff/out-verify.md`):
- `dev` (RelWithDebInfo): чистая сборка 0 ошибок / 0 предупреждений (/WX); `ctest`: 1422 теста, 0 падений,
  9 пропусков (замеры по переменной окружения, нет `VK_EXT_headless_surface`, RT-переключатель на машине с RT,
  Android без устройства). CPU-часть `ctest -LE "gpu|android" -j 6`, GPU-часть `ctest -L gpu -j 1` (метки
  исправлены, `ox_gtest_labels`). `oxshowcase_generate --check` — без расхождений.
- `debug`: сборка 0/0, GPU 279/279, тесты редактора 6/6 (замеры темпа вьюпорта в Debug пропускаются).
- `release`: сборка 0/0, плеер 300 кадров без ошибок, эталоны 13/13, редактор 100 fps на мониторе 100 Гц
  (`E:\Development\OxwaldEngine\build\release\bin`).
- Android: AVD x86_64 — 4 смоук-прогона по 300 кадров, код 0, LLDB работает; arm64 собран, на устройстве не
  запускался.
- Открытые пункты — в `BACKLOG.md`, раздел «Open after the 2026-10-15 session».

Ниже — состояние на 2026-10-14, до исправлений этой сессии (история).

- Последняя полная сборка `build/dev` (до исключения VRS-теста из CMakeLists): 478/480, упал только
  `showcase_vrs_tests.cpp`. После реконфигурации он выпадет из сборки.
- Последний полный ctest (из Windows-hostcheck агента Android, в момент, когда другие области были в работе) — упавшие
  тесты, требующие разбора:
  - эталоны: `GoldenTest.ManyPointLightsClustered`, `ReflectionsTest.SsrMirrorFloor`, `ReflectionsTest.GtaoCorner`,
    `TranslucencyTest.RefractiveSphereAbsorption`, `WorldSkinningTest.TerrainSplatLayersAndCsm` — вероятно, задеты
    изменениями в общем коде рендера (VRS-пайплайны, RR guide buffers, NTC в материалах, `renderer.cpp`). Найти
    виновника (бисекция: выключить фичу cvar'ом / откатить файл во временной копии), **не ослаблять допуски и не
    перегенерировать эталоны без обоснования**;
  - `GpuDrivenTest.SteadyStateExtractAndDrawListsDoNotAllocate`, `TextureStreamingRespectsBudget`,
    `AsyncPipelineCompileUsesPlaceholderUntilReady` — проверить связь с NTC-стримингом и новыми пайплайнами;
  - `NeuralTexturesTest.InferenceOnSampleMatchesRegularTextures`, `RayReconstructionTest.*` (2),
    `LowLatencyTest.*` (3) — в своих областях (см. таблицу);
  - `Input.RebindsPersistInUserSettings` — вероятно, задето новыми пользовательскими настройками (`settings.cpp`:
    Reflex/сплит/NTC).
- До этой сессии было: 1249 тестов, 0 падений (`dev`), 3 пропуска.

## 4. Финальная сборка и проверка (порядок)

1. Доделать открытые пункты таблицы (по одной области за раз или на непересекающихся файлах).
2. `cmake --preset dev -B build/dev …` → `cmake --build build/dev -j 12` — **0 ошибок с /WX**.
3. `ctest --test-dir build/dev --output-on-failure -j 4` (GPU-тесты лучше `-j 1..2`) — цель: 0 падений; пропуски
   объяснены. Отдельно: тесты редактора, `ox_showcase_tests`, `docs_pdf.pytest`.
4. `oxshowcase_generate --check` — зелёный (известная разница в `OxwaldShowcase.oxproj` — это **незакоммиченная
   правка пользователя**, сделанная до сессии; не откатывать, уточнить у пользователя).
5. Пресет `debug`: сборка + ctest (отладочные итераторы MSVC ловят выходы за границы).
6. Пресет `release`: сборка; замер fps редактора на дефолтной сцене Showcase (пользователь смотрел именно так).
7. Android: APK x86_64 на AVD (smoke-test, скриншот, LLDB), затем arm64.
8. Ручная проверка в окне: плеер с RT on/off, DLSS RR, VRS overlay, Reflex, FG, станция 15, гизмо в редакторе.
9. Обновить `docs/dev/perf.md` (все замеры), `CHANGELOG.md`, `README.md` (список возможностей), `docs/guide/README.md`
   (главы 34–37 и новые), перегенерировать PDF документации.
10. Коммиты по областям (сообщения — как в истории репозитория), без пуша, пока пользователь не попросит.

## 5. Новые главы и документы этой сессии
- `docs/guide/34-destruction.md`, `35-split-rendering.md`, `36-neural-rendering.md`, `37-android.md`
- `docs/dev/modules/android.md`, `render_vrs.md` (черновик), обновлены `rhi.md`, `render.md`, `runtime.md`,
  `editor.md`, `physics.md`, `gameplay.md`, `ARCHITECTURE.md`, `perf.md` (раздел «Frame pacing»)
- `tools/docs_pdf/` (скрипт PDF), `tools/android/` (скрипты сборки, эмулятора, отладки), `apps/android/` (Gradle)
