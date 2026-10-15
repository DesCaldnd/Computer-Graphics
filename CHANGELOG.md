# История изменений

Формат основан на [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/), версии — по
[Semantic Versioning](https://semver.org/lang/ru/).

## [Unreleased]

### Разрушаемость
- physics: `voronoiFracture` раскалывает замкнутый меш на куски Вороного: семена внутри меша (winding number,
  собственный splitmix64), отсечение биссектрисами с крышками разрезов (свой материал и UV), выпуклые оболочки,
  связи соседних кусков. Результат детерминирован по seed (побитово), сериализуется (`OXFR`, CRC). Тонкие объекты
  режутся на призмы сквозь толщину.
- gameplay: `DestructibleComponent` (рефлексия, сериализация, инспектор) и `DestructionRuntime`: фрактура при входе в
  play mode с кэшем, урон (`health`, пороги урона и импульса контакта), подмена целого объекта кусками с
  унаследованной скоростью и импульсом удара, структурный режим (замороженные куски, опоры `Bottom`/`Static`,
  обрушение отрезанных кусков, балка рушится над сломанной колонной), растворение обломков по времени и бюджету
  (`destruction.MaxActiveDebris`, `destruction.DebrisLifetimeScale` в группе Effects), `DestructionEvent` (Signal,
  EventBus, Lua `onEvent`), звук разрушения, Lua-таблица `destruction`, репликация флага `broken`.
- render: мост `DestructionMeshes` регистрирует меши кусков в `GpuResourceCache` (одна строка в
  `registerBuiltinFeatures`).
- OxwaldShowcase: станция 15 «Разрушаемость» (колоннада, стена, башня, ящики, ядра и взрывы, демо в экскурсии),
  тест `ShowcaseDestruction.StationDemoBreaksAndCollapses`. Руководство: глава [34](docs/guide/34-destruction.md).
- Тесты: 10 `Fracture.*` (physics), 16 `GameplayDestruction.*` (gameplay).

### Split rendering: лёгкие пассы на втором GPU
- `r.SplitRendering` (`Off` по умолчанию, `On`, `Auto`) переносит *переносимые* compute-пассы на второй GPU
  (встроенную графику), остальной кадр и вывод остаются на основном. Глава
  [35](docs/guide/35-split-rendering.md): когда это окупается, как включить iGPU в BIOS (MSI MEG Z390 ACE и
  другие платы), замеры.
- rhi: `SecondaryDevice` — второй compute-only `VkDevice` на другом адаптере (свой `VolkDeviceTable`, bindless-набор
  той же раскладки, тот же SPIR-V, калибровка скорости), `CrossDeviceChannel` с тремя путями передачи: общая
  external memory (только один физический GPU), `VK_EXT_external_memory_host` (основной путь для дискретной +
  встроенной), staging с копиями на CPU. Синхронизация — рабочий поток и timeline-семафоры, результат используется
  через `r.SplitRendering.Latency` кадров (2). Потеря второго устройства отключает перенос, кадр остаётся целым.
- Планировщик `chooseSplit` (только CPU): переносит набор сегментов с наибольшей экономией основного GPU, если второй
  успевает за кадром; измеренные времена заменяют оценки, гистерезис против «дребезга».
- render: `addPortableSegment`, `PortableSegmentDesc`; `LdrPost` (FXAA, LUT, зерно) переведён на переносимый
  сегмент. `RenderStats::split`, раздел Split rendering в окне Stats и cvar'ы в панели Quality оверлея.
  `RenderGraph::textureDesc()`.
- Вывод по этой машине (RTX 3080 + UHD 630): экранные пассы не окупаются — FXAA стоит 0.04 мс, а копии 1080p туда и
  обратно по PCIe ~1.5 мс. Механизм проверен на одном GPU (второе устройство на той же RTX 3080): картинка совпадает
  с обычной для всех путей передачи, переключение без утечек и ошибок validation, откат при потере устройства.

### Редактор: гизмо в локальных осях
- Гизмо перемещения и вращения работают в мировых или локальных осях. Переключение — кнопка на панели
  инструментов или **Ctrl+`**. Выбор сохраняется в настройках редактора (`viewport.gizmoSpace`, переключатель
  Preferences → Viewport → Local Gizmo Axes). По умолчанию оси локальные. Масштаб всегда идёт по локальным осям,
  как в Unreal и Unity.
- Оси гизмо берутся из полной мировой матрицы объекта, с учётом всей иерархии родителей. Под родителем с
  неравномерным масштабом матрица скошена; тогда ось X гизмо совпадает с видимой осью X объекта, а Y и Z
  ортогонализуются. Гизмо обновляется каждый кадр, поэтому поворот из инспектора, undo или скрипта сразу виден.
- Выбор ручек, перетаскивание по осям и плоскостям и привязка работают в повёрнутых осях: шаг перемещения
  отсчитывается вдоль локальных осей, вращение идёт вокруг локальной оси. При множественном выделении опорная
  точка и оси берутся от основного объекта.
- Перетаскивание пересчитывает только меняющиеся компоненты локального трансформа. Раньше мировой TRS
  раскладывался обратно через `Transform::fromMatrix`, и под неравномерно масштабированным родителем вращение и
  масштаб искажались. Undo/redo, как и раньше, — одна команда на перетаскивание.
- Математика вынесена в `editor/src/viewport/gizmo_math` без зависимости от Qt. Новый набор `editor.GizmoTests`
  (12 тестов) проверяет её отдельно и через вьюпорт: мышью, с undo/redo и сохранением настройки.

### Редактор: оптимизация вьюпорта (13–15 → 112–117 кадров/с)
- Вьюпорт редактора на RTX 3080 показывал 13–15 кадров/с (у пользователя ~40) при любом состоянии трассировки
  лучей, хотя сам кадр вьюпорта занимал 0,8–1,0 мс CPU. Причина: в режиме редактирования каждый тик движка на
  UI-потоке вызывал `AssetRegistry::poll()`, а его `FileWatcher` синхронно обходил `Assets/` с двумя лишними
  вызовами stat на файл — 44–62 мс на тик для 751 файла Showcase. Теперь на сцене Hub 112–117 кадров/с (лимит 120),
  с `r.RayTracing 1` — 109.
- assets: `FileWatcher::scan` берёт размер и время изменения из закэшированного `directory_entry`;
  `AssetRegistry::Options::watchScanInterval` (рантайм задаёт 250 мс): сканирование идёт в своём потоке, а `poll()`
  только раздаёт накопленные изменения. Служба `FileWatcher` движка при `fileWatching` тоже работает в фоне.
  rhi: опрос горячей перезагрузки шейдеров делает один запрос `directory_entry` на шейдер вместо двух stat.
- Вьюпорт планирует кадры по дедлайнам от лимита кадров (`FramePacer`).
- Диагностика: `OX_EDITOR_VIEWPORT_STATS=1` (посекундная статистика вьюпорта и самые дорогие проходы),
  `OxwaldPlayer --stats` / `OX_FRAME_STATS=1`; `IRenderer::stats()` отдаёт `lastFrameWaitMs`, `lastAcquireMs`,
  `lastCpuRenderMs`, `lastPresentMs`, `lastGpuFrameMs`, `vsync`; строка состояния и оверлей статистики показывают
  `FPS · интервал кадра · CPU · GPU`. Тест `ViewportPacingTests`. Замеры — `docs/dev/perf.md`, раздел «Frame pacing».

### DLSS Ray Reconstruction (главы 24 и 25, `docs/dev/modules/render_postprocess.md`)
- `r.Denoiser` = `SVGF` (по умолчанию в коде) / `DLSSRR` (`RR`) / `Auto` / `Neural`: на NVIDIA RTX шумные RT-эффекты
  (тени, AO, отражения) может чистить NVIDIA DLSS Ray Reconstruction. Фича `DLSSRR` занимает слот апскейлера
  (приоритет выше DLSS/TAAU/FSR 1), разрешение рендера — по `r.Upscaler.Quality`, при `r.Upscaler Off` — DLAA; SVGF-
  проходы эффектов в такие кадры не выполняются. Пресет NGX E (последняя трансформерная модель).
- Направляющие буферы: проход `RT.RRGuides` (`rr_guides.comp`, диффузное альбедо и F0·A+B), расстояние до попадания
  отражённых лучей из `reflections.comp`, нормали с шероховатостью в w, матрицы камеры.
- Переключение на ходу без утечек: фича NGX освобождается сразу (`DLSSRRRelease`); `ngxLiveFeatureCount()` для
  диагностики. Без RR (не RTX, нет `nvngx_dlssd`, path tracer) остаются SVGF и выбранный апскейлер.
- Меню: Preferences → Play → Game User Settings → Ray Tracing Denoiser, строка «RT denoiser» в меню игры,
  `UserSettings::graphics.denoiser` (по умолчанию `Auto`); автоопределение качества выбирает `Auto`.
- Замеры на RTX 3080: качество против path tracer'а и сошедшегося гибридного кадра, время при 1080p/4K
  (`docs/dev/perf.md`). На RTX 3080 RR дороже SVGF + DLSS везде, кроме режима Performance.
- Тесты: `RayReconstructionCpu.*` (5), `RayReconstructionTest.*` (6, GPU).

### Нейронный рендеринг (глава 36, `docs/dev/modules/render_neural.md`)
- `ox::nn` (assets): MLP на CPU — инференс, обучение (backprop + Adam), FP16-упаковка; результат детерминирован при
  любом числе потоков. `render::neural`: `NeuralNetwork`, `NeuralInference`, `NtcGpuDecoder`, GLSL-слои FP32 / FP16 /
  `VK_NV_cooperative_vector` / `VK_KHR_cooperative_matrix` (`mlp.glsl`, `mlp_coopmat.glsl`).
- rhi: `DeviceCaps::cooperativeMatrix*`, `cooperativeVector*`, `storageBuffer16BitAccess`,
  `shaderBufferInt64Atomics`; устройство включает оба расширения, если они есть; формат `B4G4R4A4_UNORM_PACK16`.
- Нейросжатие текстур (NTC): `AssetType::NeuralTextureSet` (`.oxntc`), энкодер/декодер/метрики
  (`assets/neural/ntc.hpp`), настройка импорта материалов `neuralCompression` (Off/On/Replace, пресеты, битрейт),
  `oximport --ntc` и `oximport --project`. Рантайм: `r.NeuralTextures` Off / DecodeOnLoad / InferenceOnSample,
  `.Filter`, `.Prepass`, `.MinMip`, `.DecodeQuality`, `.MipBias`, `.Streaming`, `.ShaderPath`; стохастическая
  фильтрация, BC-хвост для остальных потребителей, стриминг латентов; настройки в редакторе (Project Settings →
  Rendering → Neural Rendering).
- Нейроденойзер (**экспериментальный**): `r.Denoiser Neural` — À-trous SVGF с ядром, предсказанным маленькой сетью;
  веса (`denoiser_weights.glsl`) обучены инструментом `NeuralDenoiserTest.TrainWeights` на path tracer'е движка
  (1/2/4/16 сэмплов) и AO (1/8). На AO лучше SVGF сразу после разрыва истории (29,0 против 26,4 дБ), но хуже после
  накопления (26,8 против 29,6 дБ) и втрое дороже (À-trous 6,7 против 2,3 мс в 1080p) — в меню настроек не
  предлагается.
- Общие файлы (аддитивно): `GpuResourceCache` (`IMaterialResolveHook`, `AssetProvider::loadNeuralTextures`),
  `GpuViewConstants::neuralMaterials` (вместо `shPad0/1`), флаг `kMaterialNeural`, `forward.frag` /
  `depth_prepass.frag` (`oxSampleMaterialView`), `ImportContext` (`readArtifact`, `sourceOf`), `MaterialAsset`
  (`neuralTextures`, `neuralTexturesOnly`), `rt::SvgfDenoiser`, `r.Denoiser` / `pp::DenoiserType` (+Neural).
- Шейдеры материалов с нейродекодером собираются только при первом включении `InferenceOnSample`; в режимах Off и
  DecodeOnLoad используются варианты без декодера (`OX_NTC_DISABLED`) — иначе обычные материалы на NVIDIA
  замедлялись (forward в 4K 2,3 мс вместо 0,7 мс).
- Cooperative matrix: ядро MLP ускорено с 65 до 8,8 мс (8,3 млн сэмплов; выравнивание строк shared memory, веса из
  буфера, 1024 сэмпла на рабочую группу); для compute-шейдеров теперь выбирается cooperative vector (4,7 мс), затем
  cooperative matrix.
- `.oxmat`: поля `neuralTextures` / `neuralTexturesOnly` пишутся только когда заданы — материалы без NTC сохраняются
  байт в байт как раньше (`oxshowcase_generate --check`).
- Замеры: `docs/dev/perf.md`, раздел «Neural rendering» — NTC Medium в 7,1 раза меньше BC7/BC5 на диске
  (2,9 против 20,6 МБ), InferenceOnSample +1,7 мс в 1080p и +6,3 мс в 4K на станции материалов, DecodeOnLoad — как Off.

### Переменная частота шейдинга (`r.VRS*`, глава 27, `docs/dev/modules/render_vrs.md`)
- Фича `VariableRateShading` (`VK_KHR_fragment_shading_rate`, карта частоты по плиткам 16×16 px): режимы
  `Adaptive` (содержимое + движение + DoF, по Yang et al. 2019), `Motion`, `Foveated`, `DebugOverlay`;
  гистерезис, периодический перезамер плиток (`r.VRS.RefreshPeriod`), отдельная статистика непрозрачной картинки
  для кадров с преломлением; карта подключается только к цветовым пассам из `r.VRS.Passes` (prepass, тени, пост,
  UI — никогда).
- Группа Shading: VRS включён на Low (порог 0.05, до 4×4) и Medium (0.03, до 2×2), выключен на High / Ultra и в
  голом `Renderer`. Замеры на RTX 3080 (станции Showcase): Medium −6…−9 % GPU в 4K при PSNR 45–52 дБ, Low до −12 %.
- rhi (аддитивно): `DeviceCaps` для shading rate, `DeviceDesc::enableFragmentShadingRate`,
  `TextureUsage::ShadingRateAttachment`, `Access::ShadingRateRead`, `PassBuilder::shadingRate`,
  `RenderGraph::pass(i)` / `passType` / `passRenderExtent` / `passShadingRate`, `RenderingDesc::shadingRate`,
  `GraphicsPipelineDesc::variableRateShading`.
- Тесты: `vrs_cpu_tests` (9), `VrsTest.*` / `RenderTest.Vrs*` (9 + 1 opt-in), замеры `ShowcaseVrs.*` (opt-in).

### Производительность рендера (`docs/dev/perf.md`)
- Трассировка лучей: гибридные эффекты (тени, AO, GI, отражения, видимость объёмного тумана, преломление,
  направляющие буферы Ray Reconstruction) не выполняются, пока кадр заменяет path tracer, — ни проходов, ни целевых
  текстур (`rt::pathTracerActive()`, тест `RayTracingTest.HybridEffectsSkipUnderPathTracer`). Кадр path tracer'а на
  тестовой RT-сцене: 10,3 → 1,7 мс в 1080p, 36,1 → 5,6 мс в 4K (RTX 3080).
- Экранные AO и SSR трассируются в половинном разрешении, когда высота рендера достигает
  `r.AO.HalfResAboveHeight` / `r.SSR.HalfResAboveHeight` (по умолчанию 1800 px, 0 — никогда): нативный 4K стоит
  столько же, сколько эти эффекты с плотностью 1080p. Станция 4 Showcase в 4K: 12,9 → 8,4 мс, PSNR 40,9 дБ против
  полного разрешения. Тест `ReflectionsTest.ScreenSpaceTracesHalveAboveRenderHeight`.
- Пайплайны GPU-driven меш-летов и меш-шейдеров собираются без нейродекодера текстур (`OX_NTC_DISABLED`), пока
  `r.NeuralTextures` не InferenceOnSample: ForwardOpaque на пути меш-летов 2,5 → 0,85 мс в 4K. Тест
  `NeuralTexturesTest.MeshletPathDecodesNeuralMaterials`.
- `OxwaldPlayer --pass-stats` / `--pass-csv <файл>`: GPU-время каждого прохода render graph, усреднённое за прогон,
  и распределение времени GPU-кадра (min / p10 / медиана / p90 / max); включает `--single-thread`. У
  `GpuDrivenTest.StressSceneTimings` новые опции `OX_RENDER_PERF_RES` и `OX_RENDER_PERF_CVARS`.

### Низкая задержка (Reflex) и генерация кадров FSR 3
- `r.Reflex` (`Off`, `On` — по умолчанию, синоним `Auto`, `OnBoost`) включает NVIDIA Reflex через
  `VK_NV_low_latency2`: режим и boost, `vkLatencySleepNV` перед началом кадра, маркеры
  симуляции/ввода/рендера/present, отчёты `vkGetLatencyTimingsNV`. Без Reflex (AMD, Intel, Apple, Android, старые
  драйверы, а также при генерации кадров) работает ограничитель движка `r.Reflex.Fallback`. `JustInTime`
  (по умолчанию) задерживает начало кадра на измеренное время ожидания в очереди. `Strict` ждёт, пока GPU закончит
  предыдущий кадр.
- rhi: Reflex API у `Swapchain` (`setLowLatencyMode`, `latencySleep`, `setLatencyMarker`, `latencyTimings`,
  `setPresentId`, `lowLatencyCapable`), `VkLatencySubmissionPresentIdNV` в `Device::submit`, `VkPresentIdKHR`.
  `ISwapchainProxy` + `Swapchain::setProxy` передают VkSwapchainKHR внешнему владельцу.
- `r.FrameGeneration` (`Off`, `FSR3`) включает генерацию кадров AMD FSR 3.1 (FidelityFX SDK 1.1.4, `amd_fidelityfx_vk.dll`
  загружается во время работы; vcpkg-порт `amd-fidelityfx-vk`, опция `OX_ENABLE_FSR3_FRAMEGEN`, без пакета —
  заглушка). Только в плеере, Windows x64, видеокарта любая. Между каждыми двумя отрисованными кадрами вставляется
  один сгенерированный (ratio 2,00). Подготовка FSR идёт по глубине и векторам движения в разрешении рендера,
  поэтому работает с любым апскейлером. Картинка без UI копируется на точке `Overlay`, и UI остаётся чётким.
  Композицию делает движок: FSR пишет промежуточный кадр в sRGB-буфер линейными значениями, движок кодирует их
  blit'ом.
- Reflex не может управлять своп-чейном прокси FSR 3 (это объект SDK, настоящий своп-чейн скрыт в DLL). При
  генерации кадров `r.Reflex On` поэтому включает ограничитель движка. Плеер печатает `reflex=framegen-proxy`,
  меню объясняет причину, `LatencyStats::reflexBehindFrameGeneration`.
- Исправлены зависания прокси FSR 3. В плеере GPU-bound прокси зависал за секунды (11 из 12 запусков): его поток
  интерполяции затирал ещё не показанную пару кадров. Теперь движок отдаёт кадр N прокси только после того, как тот
  собрал кадр N − 1 (ожидание не дольше 250 мс; `Stats::pacingWaitMs`, `pacingTimeouts`): 0 из 12. Несколько прокси
  FSR 3 одновременно на одной видеокарте зависают внутри SDK/драйвера. Поэтому GPU-тесты генерации кадров
  выполняются строго по одному (именованный мьютекс; при `ctest -j` раньше падали все три).
- Статистика: `EngineStats::latency` / `RenderStats::latency` (`engineLatencyMs` — от начала симуляции до готовности
  GPU, для любого производителя; `pcLatencyMs` от драйвера), `RenderStats::frameGeneration` (кадры отрисованные,
  показанные, сгенерированные, удержание кадра прокси). Всё это показывают `OxwaldPlayer --stats` и отладочный
  оверлей. Меню плеера: «Низкая задержка», «Генерация кадров». Редактор: Preferences → Display, Project Settings →
  Rendering → Display.
- Замеры на RTX 3080 (`docs/dev/perf.md`, «Latency»). Showcase, станция 4, 1440p Ultra: Reflex снижает задержку
  движка с 46,9 до 14,0 мс при −3 % fps. Генерация кадров: 104 → 163 кадра на экране (отрисованных 82), удержание
  кадра 24 мс; с ограничителем `Strict` задержка 12,5 мс, удержание 13 мс. С вертикальной синхронизацией без
  ограничения fps задержка ~48 мс; `t.MaxFPS 97` при 100 Гц даёт 6,4 мс.
- Тесты: `LowLatencyTest.*` (3, rhi, GPU), `Latency*`/`LatencyLimiter*` (7, runtime), `FrameGenerationTest.*` (3, GPU:
  пары кадров 1 : 1, PSNR сгенерированного кадра против промежуточного 40,95 дБ, переключение туда-обратно без
  ошибок валидации, Reflex скрыт через прокси и возвращается после).
- Руководство: глава [25](docs/guide/25-upscalers-postprocess.md), шаг 8; глава [26](docs/guide/26-quality-settings.md),
  настройки игрока.

### Android-порт (GameActivity, Vulkan)
- Новый модуль `android` (`engine/android`, только при сборке под Android): `AndroidPlatform` (жизненный цикл
  GameActivity, касания, геймпады, клавиатура), логи в logcat (`Oxwald`, `OxwaldStdout`), обработчик падений со
  стеком вызовов (`OxwaldCrash`) и передачей сигнала в debuggerd, ассеты APK через AAssetManager, подсказки ADPF
  (`APerformanceHint`, `AThermal`), удалённая консоль движка через `adb forward` (порт 27015 в debug-сборке).
- rhi: `ox_rhi_android` (`AndroidSurfaceProvider`, `VK_KHR_android_surface`). Swapchain переживает потерю окна:
  `ISurfaceProvider::surfaceGeneration()`, `Device::releaseSurface()/recreateSurface()`, обработка
  `VK_ERROR_SURFACE_LOST_KHR`. Если дисплей повёрнут и swapchain стоит в IDENTITY, Android возвращает
  `VK_SUBOPTIMAL_KHR` на каждом present; движок больше не пересоздаёт swapchain из-за этого в каждом кадре.
- runtime: `TouchInputMapper` (виртуальный стик, обзор, тап как клик, экранные кнопки, щипок как колесо мыши) в
  Enhanced Input, конечный автомат `AppLifecycle`, таблицы кодов клавиш и осей Android, `splitCommandLine`.
- core: `IAssetBundle`/`AssetIndex`/`BundleMount`/`extractBundle`: ассеты APK как источник VFS (`apk://`) с индексом,
  потому что AAssetManager не перечисляет каталоги.
- render: `mobileQualityProfile()` при первом запуске на Android: Low/Medium по GPU, FSR 1, без RT без
  `VK_KHR_ray_query`, вершинный путь без mesh-шейдеров.
- audio: `AudioEngine::setDeviceSuspended()` на паузу и возврат приложения.
- Gradle-проект `apps/android` (AGP 8.7, Kotlin DSL, ABI arm64-v8a и x86_64, debug/release, prefab GameActivity,
  ассеты движка и проекта в APK, слои валидации Khronos в debug, ASan/HWASan через `-PoxSanitizer`). Скрипты
  `tools/android/`: установка тулчейна в `E:\Development\Android`, vcpkg под Android, эмулятор, сборка, запуск,
  logcat, консоль, Tracy, LLDB (`debug.ps1`/`debug.sh`), смоук-тест (ctest-метка `android`), проверка сборки под
  Windows.
- vcpkg.json: `vulkan-loader`, `vulkan-validationlayers`, `glfw3`, `nvidia-dlss` и `glfw-binding` для imgui
  помечены `!android`. Набор пакетов для Windows не изменился.
- Руководство: глава [37](docs/guide/37-android.md); заметки: [docs/dev/modules/android.md](docs/dev/modules/android.md).

### Android: первый запуск на AVD и сборка arm64
- Проверено на эмуляторе (AVD API 35 x86_64, Vulkan 1.3 RTX 3080 через gfxstream): Showcase 300 кадров с
  валидацией, 0 ошибок Vulkan, `exit code 0`; LLDB (`tools/android/debug.ps1 -Batch`) останавливается в
  `ox::Engine::tick` и печатает стек до `android_main`. Собран APK для телефонов (arm64-v8a, 133 МБ); на устройстве
  ещё не запускался.
- Скрипты (Lua): `require` работает в собранной игре (`--pak`). Раньше модули искались только на диске, и в
  Showcase из пака (Windows и Android) не загружался `Scripts/game.lua` (`module 'lib.flow' not found`).
  `ScriptVM::setModuleFileReader`: если файла модуля нет на диске, VM спрашивает читателя; модуль `script` движка
  читает такие пути через VFS (`project://…`). Игре только из пака корень по умолчанию — `project://scripts`;
  Showcase добавляет `project://Assets/Scripts`.
- vcpkg: overlay-порт `sol2` с патчем для clang 19 (NDK r28): `&upvalue_this_member_variable<…>::call<…>` с
  `noexcept(std::is_nothrow_copy_assignable_v<T>)` не приводился к `lua_CFunction`. Порт-версия 3.5.0#2, на Windows
  поведение не меняется.
- runtime: разбор чисел с плавающей точкой в аргументах плеера без `std::from_chars` для `double` (его нет в libc++
  NDK r28).
- physics: если у Jolt нет конфигурации Debug (release-only триплеты Android), Debug-сборка линкуется с release
  `libJolt.a` и получает его определения (`NDEBUG`), иначе раскладка классов Jolt не совпала бы с библиотекой.
- Android-плеер: `TRACY_NO_INVARIANT_CHECK=1` до загрузки библиотеки (клиент Tracy на эмуляторе без invariant TSC
  вызывал `exit(1)` в статическом конструкторе, процесс перезапускался по кругу).
- Gradle/скрипты: debug-APK по умолчанию с урезанными библиотеками (неурезанная `liboxwald_player.so` весит ~760 МБ;
  символы для LLDB/Studio берутся из дерева сборки, `-PoxKeepSymbols=true` возвращает старое поведение);
  `build.ps1` удаляет прошлый APK перед упаковкой (инкрементальная упаковка AGP раздувала APK до 800+ МБ);
  `-Jobs` (по умолчанию 4); kotlin-bom 1.8.22 (дубликаты классов kotlin-stdlib-jdk7/jdk8); `JAVA_HOME` из
  пользовательских переменных (машинный JDK ломал `jlink` в AGP); у каждого Android-триплета свой корень установки
  vcpkg (`vcpkg_installed_android/<triplet>`): установка `arm64-android` в общий корень удаляла `x64-android`.
- Смоук-тест: `Android.DeviceSmoke` пропускается (код 77), когда устройства нет — раньше он падал в Windows
  PowerShell 5.1, потому что stderr adb («no devices/emulators found») превращался в исключение. Скриншот экрана
  снимается во время игры, а не после выхода; логи сохраняются в UTF-8; системное окно «Viewing full screen»
  подавляется. `debug.ps1`: `lldb.cmd` вместо `lldb.exe` (встроенный Python), без `--log-channels ""` (lldb-server
  обрывал первое подключение), `pkill -x` (`pkill -f` убивал собственный `sh`), выбор потока на точке останова в
  `-Batch`, выключенный JIT-загрузчик lldb.
- Скриншот плеера (`--screenshot`) больше не теряется при рендере в отдельном потоке: `RuntimeRenderer` забирает
  запросы один раз за кадр (раньше запрос между серединой и концом кадра оставался без текстуры захвата, на AVD это
  дало `exit code 3`); запрос, после которого не нарисовано ни одного кадра, при остановке рендерера завершается
  ошибкой в логе. AVD: четыре смоук-прогона по 300 кадров подряд с кодом 0.
- Скрипты: `debug.ps1 -Batch` выбирает поток на точке останова через `SBProcess` цели и не падает при выводе в файл
  (`adb push` пишет прогресс в stderr); `smoke-test.ps1` берёт устройство только из строки `[rhi] Device:`.

### Документация в PDF
- `tools/docs_pdf/build_docs_pdf.py` собирает всю документацию (README, руководство, документация разработчика,
  отчёты, CHANGELOG) в один PDF: обложка, оглавление с номерами страниц, закладки, колонтитул с номером страницы,
  подсветка кода, встроенные картинки, ссылки между `.md` превращены во внутренние. Рендер — headless Chrome/Edge.
  Тесты `pytest` зарегистрированы в ctest (`docs_pdf.pytest`), цель `docs_pdf` собирает PDF.
- Новые главы руководства: [34](docs/guide/34-destruction.md) «Разрушаемость»,
  [35](docs/guide/35-split-rendering.md) «Split rendering», [36](docs/guide/36-neural-rendering.md) «Нейронный
  рендеринг», [37](docs/guide/37-android.md) «Android»; в главе [27](docs/guide/27-gpu-driven-performance.md) —
  шаг про VRS. Документация разработчика: `docs/dev/modules/android.md`, `render_vrs.md`, `render_neural.md`;
  замеры этой сессии — в `docs/dev/perf.md`.

### Исправлено
- ctest: метки тестов, найденных `gtest_discover_tests`, терялись — `LABELS "render\;gpu"` разворачивался в
  `LABELS render gpu ENVIRONMENT …`, метка `gpu` была только у 19 тестов из ~280, а `OXWALD_SOURCE_DIR` не доходил до
  тестов. Новый `ox_gtest_labels()` (`cmake/OxwaldHelpers.cmake`) ставит метки уже найденным тестам; `ctest -L gpu` /
  `-LE gpu` снова делят полный прогон на последовательную GPU-часть и параллельную CPU-часть. Тест
  `ctest.DiscoveredGpuTestsAreLabelled`. Тесты `ox_showcase_tests` (создают Vulkan-устройство) помечены `gpu`.
- ui: окно Settings отладочных инструментов падало на assert Dear ImGui 1.92.7 в Debug — целые cvar'ы
  редактировались `InputInt(..., EnterReturnsTrue)`, а `InputScalar` этот флаг не поддерживает. Теперь
  `InputText` (`EnterReturnsTrue | CharsDecimal`), сохранение по Enter, как у строковых (`UiDebug.*`,
  `GuideUiImGui.DebugToolsAndConsole` в Debug).
- Редактор: `FramePacer` догоняет тик, опоздавший не больше чем на два периода, вместо перезапуска расписания —
  раньше любая задержка UI-потока дольше кадра стоила тика. Панель Stats больше не пересоздаёт ~75 строк таблицы
  каждые 500 мс (зависание UI-потока на 30–45 мс), а обновляет их на месте. Вместе: в Release на сцене Hub
  91–94 → 100 кадров/с на мониторе 100 Гц, без ограничения ~150 (упор в GPU). Тесты
  `pacerCatchesUpAfterShortHiccup`, `statsTableUpdatesRowsInPlace`; замеры темпа вьюпорта в Debug пропускаются
  (программная отрисовка ~25 мс).
- assets: `oxpack` удалял `.meta` ассетов, для которых в нём нет импортёра (`Assets/AI/guard.oxbt.meta` — у oxpack
  нет импортёров gameplay), как «осиротевшие», и ассет терял UUID. Meta, у которой есть исходный файл, больше не
  считается осиротевшей (`AssetRegistry.MetaOfSourceWithoutImporterIsKept`).
- VRS был включён по умолчанию (порог 0.08) и менял все эталонные картинки рендера: блочный пол в
  `GoldenTest.ManyPointLightsClustered`, размытое преломление в `TranslucencyTest.RefractiveSphereAbsorption` и ещё
  6 тестов. Кроме смены значений по умолчанию исправлены три ошибки оценки: грубые плитки занижали собственную
  детализацию (деление на шаг фрагмента → на его корень), плитка «залипала» в грубом режиме из-за текстур на мип
  крупнее (периодический перезамер), опак за преломляющим стеклом огрублялся по статистике стекла. Анализ кадра
  ускорен в 4 раза (0.42 → 0.10 мс в 4K).
- «Двухрежимное» время кадра FSR 1 / TAAU на станции 6 Showcase в 4K (то ~14, то 50–59 мс) — не дефект движка:
  отдельные кадры включали кванты времени других процессов на общем GPU (эмулятор Android, GPU-тесты, браузер). На
  свободном GPU стабильно 5,2 / 6,7 мс (медиана `--pass-stats`).
- assets: реестр больше не заменяет существующий `.meta` новым случайным UUID — нечитаемый (недописанный git'ом,
  редактором или другим процессом) meta оставляется как есть, ассет сохраняет прежний UUID; тот же файл, найденный
  по пути в другом регистре (ссылка модели на текстуру в Windows), не считается копией. Новая опция
  `AssetRegistry::Options::writeMetas = false` (только чтение) — её использует `oxshowcase_generate --check`,
  который раньше мог записать meta со случайным UUID. Тесты: 3 новых `AssetRegistry.*`.
- Тесты рендера: фикстура `RenderTest` сохраняет и восстанавливает все cvar'ы и группы качества
  (`CVarStateSnapshot`) — тесты в одном процессе `ox_render_gpu_tests` больше не наследуют пресеты друг друга
  (`TranslucencyTest` ставил Effects/Shading/Reflections в High). Тест `RenderTestIsolation.*`.
- Редактор: частота вьюпорта берётся из частоты экрана, на котором он находится (`ViewportPanel::pacingFps`, смена
  монитора перезапускает расписание) — мониторы 144/165/240 Гц не ограничены 120/125 fps.

## [0.1.1] — 2026-10-14 — Windows/RTX bring-up

Первый запуск движка вне macOS: Windows 11, NVIDIA GeForce RTX 3080 (драйвер 617.42), Visual Studio 2022 17.14
(MSVC 14.44), CMake 3.30, Ninja 1.12.1. До этого сборка под Windows не запускалась, а код трассировки лучей, DLSS,
меш-шейдеров и `drawIndirectCount` ни разу не исполнялся. Linux по-прежнему не проверен.

### Сборка под Windows/MSVC
- Собирается всё: движок, плеер, инструменты, тесты, примеры руководства, демо-проект и редактор. Пресет `dev`
  собирается с предупреждениями как ошибками: `/W4 /permissive- /utf-8 /WX`, отключены только C4100, C4127, C4201
  и C4324. Сужения типов, затенения имён, недостижимый код и прочие предупреждения исправлены в коде.
- CMake: `NOMINMAX` и `WIN32_LEAN_AND_MEAN`, `/Zc:preprocessor` (нужен для `__VA_OPT__`), `/bigobj`, define'ы Jolt
  для RelWithDebInfo, `ws2_32` и `winmm` для ENet, `gtest_discover_tests` в режиме `PRE_TEST`.
- Развёртывание рантайма: путь к библиотеке в манифесте слоя валидации с `.\\` (иначе загрузчик Windows ищет DLL
  в рабочем каталоге), DLL NGX по конфигурации (релизные для Release/RelWithDebInfo, для разработчиков — в Debug),
  плагины Qt рядом с редактором.
- `.gitattributes` фиксирует LF для текстовых файлов. Overlay-порт `nvidia-dlss` исправлен: загрузка из LFS по SHA,
  в рантайм попадают только `nvngx_*.dll`, отдельный список DLL для Debug. `tools/bootstrap.sh` выбирает триплет
  `x64-windows` в Git Bash.
- Переносимость исходников: недостающие стандартные заголовки (около 120 файлов), замена POSIX-вызовов,
  отображение pak в память через Win32, запись сгенерированных файлов в бинарном режиме, `Clock::sleepUntil` на
  waitable timer (иначе ограничитель кадров квантуется по 15,6 мс).
- Собственный декодер BC7 в `assets` вместо внутренней функции libktx, которую DLL-сборка ktx не экспортирует.
- Атомарная запись файлов (кэш ассетов, кэш SPIR-V) использует уникальные временные имена: при параллельном `ctest`
  несколько процессов писали в один `<path>.tmp`.

- Пресет `debug` собирается под MSVC с `/WX` и проходит тесты. Для этого: отладочная `TracyClient.dll`
  копируется рядом с исполняемыми файлами (vcpkg кладёт её в `debug/bin/Debug`); overlay-порт `glslang` с
  проверкой границ в `decorateStructType` — без неё отладочные итераторы MSVC аварийно завершали компиляцию
  каждого шейдера с вложенными buffer-блоками; тесты на число аллокаций пропускаются под отладочными
  итераторами, сравнения DLSS исключают баннер dev-рантайма NGX.

### Неопределённое и неуточнённое поведение, найденное портом
Эти ошибки были и на macOS, но там не проявлялись.
- **Порядок вычисления аргументов.** Чтение полей компонентов из Lua возвращало `nil`
  (`f(childRef(c, path), std::move(path))`: MSVC вычисляет аргументы справа налево). Два вызова генератора случайных
  чисел в аргументах одного конструктора меняли основания травинок; то же в генерации `fire_loop.wav` демо-проекта
  и в сценах GPU-тестов (`MatchesCpuPathWithShadows` строил на MSVC другую сцену).
- **Висячая ссылка на временный объект.** Редактор зависал в `Project::captureCVarSettings`: range-for по `.items()`
  временного JSON-объекта.
- **Отрицательный операнд `%` в GLSL.** Шов в объёмных облаках (`noise_gen.comp`): результат не определён, драйвер
  NVIDIA считает иначе, чем MoltenVK.
- **Распределения `std`.** `std::uniform_*_distribution` дают разные последовательности в разных реализациях STL:
  карта нормалей воды расходилась с эталоном. Алгоритм выписан явно.
- **Хеш и аллокации в STL MSVC.** Хеш `InstanceKey` клал все сущности в одну корзину, `GpuScene::updateInstances`
  был квадратичным (177 мс на 10 тыс. инстансов, стало 2–3 мс); временный `std::unordered_set` аллоцировал каждый
  кадр даже пустым.
- `oxshowcase_generate --check` удалял ещё открытый файл; теперь он также сравнивает числовые данные (PNG, WAV,
  сцены, меш манекена) с жёстким допуском: 26 бинарных ассетов отличаются от закоммиченных на последний бит
  (libm, FMA), ассеты не перегенерированы.

### DLSS
- DLSS проверен на RTX 3080: все режимы качества и DLAA, смена режима и размера выхода, статичная сцена, движение
  камеры, авто-экспозиция, пересоздание рендерера и устройства (новые GPU-тесты `PostProcessTest.Dlss*`).
- Знаки джиттера и векторов движения подтверждены измерением (PSNR против 16× SSAA), менять их не пришлось.
- Исправлено: устройство в тестах создавалось без расширений NGX — ошибки валидации и `FAIL_PlatformError`. Теперь
  проверка доступности сначала сверяет включённые расширения (`rhi::Device::hasInstanceExtension` /
  `hasDeviceExtension`) и без них NGX не вызывает; `r.Upscaler=DLSS` в этом случае работает как TAAU.
- Исправлено: утечка объектов NGX и падение при выходе, если NGX инициализировался без рендерера. NGX теперь
  завершается из `rhi::Device::addShutdownCallback`, до `vkDestroyDevice`.
- `r.Upscaler.Sharpness` на DLSS не влияет: параметр резкости в NGX устарел.

### GPU-driven и async compute
- Меш-шейдеры и `drawIndirectCount` подтверждены на NVIDIA; новые счётчики `RenderStats::meshShaderDrawCalls`
  и `indirectCountDrawCalls` проверяются в тестах.
- Async compute на отдельном семействе очередей: барьеры compute-прохода больше не называют графические стадии
  (`sanitizeStages` фильтрует стадии по семейству очереди), батчи режутся и на чтении после чтения между очередями,
  compute-батч отправляется вместе со следующим графическим. Перекрытие с графикой стало реальным.
- `TextureStreamingRespectsBudget`: допуск 5 % отличающихся пикселей (у NVIDIA другой выбор LOD при анизотропии 16×).
- Замеры RTX 3080 — в `docs/dev/perf.md`.

### Плеер и редактор
- Плеер: `--cvar` и `--quality` больше не затираются при применении пользовательских настроек (`Settings::apply`);
  `--width`/`--height` задают размер offscreen-кадра в режиме `--headless`.
- Редактор: Vulkan-surface для Win32 (и xcb для Linux; Wayland не реализован), пути через UTF-16, строка
  компилятора MSVC в «О программе», автоопределение качества учитывает поставщика возможностей GPU. Редактор
  открывает демо-проект с Vulkan-вьюпортом на Windows и корректно завершается.

### Трассировка лучей
- Код трассировки лучей впервые исполнен на железе и проверен на RTX 3080 (драйвер 617.42, слои валидации
  1.4.341): BLAS/TLAS, все проходы на ray query и RT pipeline. `RayTracingTest.*` — 20 тестов проходят, один
  пропускается по делу; обычная валидация и sync validation чистые. Станции 01, 02, 03, 04, 08 и 13 демо-проекта
  отрисованы в 1920×1080 с `r.RayTracing` и без него без ошибок валидации.
- Исправлено: `RayTracingSceneApi::activeThisFrame()` после кадра всегда возвращал `false` (сравнение с номером
  кадра, который `endFrame()` уже увеличил).
- Исправлено: небо в лучах GI, отражений и стекла не учитывало интенсивность IBL, из-за чего включение
  `r.RayTracing` меняло уровень ambient. **Изменение поведения:** свет неба в этих лучах теперь масштабируется как
  растровый IBL, на `ambientIntensity × r.IBL.Intensity`.
- Исправлено: денойзер теней тянул шлейф за движущейся тенью и оставлял бледную тень под движущимися объектами.
  Новое поле `DenoiserSettings::clampHistory` ограничивает историю диапазоном текущей окрестности 5×5; включено для
  теней солнца, локальных источников и ReSTIR.
- Исправлено: таблица привязки шейдеров (SBT) начинается с адреса, выровненного на `shaderGroupBaseAlignment`.
- Исправлено: `oxDdgiMod` не использует `%` с отрицательным операндом (в GLSL результат не определён).
- Адрес сжатого BLAS запрашивается повторно после уничтожения исходного: иначе GPU-assisted validation
  (слой 1.4.341) забывала структуру, сообщала `VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-12281` на каждый
  инстанс TLAS и RT-эффекты под ней не работали. Это ошибка слоя; на периодической пересборке деформируемого BLAS
  ложное срабатывание осталось.
- Исправлены сцены трёх тестов (отражения, GI, ReSTIR): например, плоский пол не может отражать объект за камерой.
  GI и ReSTIR теперь сверяются с path tracer'ом.
- Новые GPU-тесты: `PathTracerPipelineMatchesRayQuery` (режим 1 против режима 0),
  `RtLocalLightShadowsWithoutRestir`, `RtShadowsOfAlphaTestedAndTintedCasters`, `RtVolumetricFogVisibility`,
  `TlasFollowsMovingInstancesWithoutLeaks` (300 кадров движения), `SkinningTest.RayTracedShadowFollowsAnimation`
  (refit BLAS скиннинга), `PerfReportRayTracing` (время по проходам, `OX_RT_PERF=1`).
- Замеры RTX 3080 без валидации: все RT-эффекты по умолчанию стоят 9,2 мс GPU при 1080p и 31,2 мс при 4K, из них
  денойзер — 5,9 и 22,5 мс. Таблицы по проходам — в `docs/dev/perf.md` и `docs/dev/modules/render.md` §11.
- Не проверено: захват в Nsight Graphics, переключение `r.RayTracing` в окне вручную, меню настроек в игре.
  Открытые замечания — в `docs/dev/BACKLOG.md`.

### Тесты
- Полный `ctest` на Windows: 1249 тестов, ни один не падает, 3 пропускаются по делу:
  `DeviceLifecycle.SurfaceDeviceAfterHeadlessDevice` (у драйвера NVIDIA на Windows нет `VK_EXT_headless_surface`),
  `RayTracingCheckboxIsInertWithoutSupport` (на машине есть RT), `UiEngine.WindowedScreenshot` (headless-запуск).
  Тесты редактора проходят (4 из 4).
- На macOS изменения этой версии ещё не перепроверялись (см. `docs/dev/BACKLOG.md`).

## [0.1.0] — 2026-10-08

Первая версия OxwaldEngine (проект переименован из учебного DesEngine и написан заново).

### Сборка
- CMake-пресеты `debug`, `dev`, `release` и тестовые `dev`/`cpu`; все зависимости через vcpkg (manifest mode),
  overlay-порты для MoltenVK, NVIDIA DLSS и Vulkan; `tools/bootstrap.sh` (`--no-editor` — без Qt).
- Автообнаружение модулей в `engine/`, частичная сборка через `OX_MODULES`, опция `OX_WARNINGS_AS_ERRORS`.

### Модули движка
- **core** — типы, лог, assert, `Result<T>`, математика, хэши, UUID, сервисы (DI), события, job system (enkiTS),
  рефлексия с атрибутами, бинарная сериализация OXB1 и JSON, cvar'ы и группы масштабируемости, VFS, слежение за
  файлами, frame allocator, профилирование Tracy, debug draw.
- **scene** — ECS на EnTT, компоненты ядра, иерархия и трансформы, реестр компонентов, системы и фазы,
  сохранение сцен, префабы.
- **rhi** — Vulkan-устройство (volk, VMA, vk-bootstrap), DeviceCaps, bindless, пайплайны, shaderc с кэшем SPIR-V
  и hot reload, render graph, swapchain, BLAS/TLAS.
- **assets** — реестр ассетов с `.meta` и UUID, импортёры (glTF, assimp, текстуры, кубмапы), BC7/BC5, meshlets и LOD,
  асинхронный `AssetManager` с hot reload, архивы `.oxpak`.
- **render** — clustered forward+ PBR, IBL, тени CSM/spot/point с атласом и PCSS, пикинг, оверлеи редактора, отладочные
  режимы, GPU-статистика, бенчмарк автоопределения качества, API расширения фичами. Отражения (пробы, SSR,
  плоские), GTAO, irradiance volumes; froxel-туман и объёмные облака; OIT, преломление, вода, GPU-частицы; ray tracing
  (тени, отражения, AO, DDGI, преломление, ReSTIR DI, path tracer, денойзер SVGF); TAA/TAAU/FXAA, FSR 1, DLSS (NGX),
  постобработка; GPU-driven culling, meshlets, async compute, стриминг мипов; ландшафт, растительность, небо,
  compute-скиннинг.
- **physics** — Jolt: тела и формы, слои коллизий, запросы, события контактов, соединения, контроллер персонажа.
- **animation** — скелеты, клипы, blend spaces, аниматор и state machine, root motion, IK, скиннинг, импорт через assimp.
- **spline** — Безье, Catmull-Rom, B-сплайны, NURBS, параметризация по длине дуги, движение по пути, экструзия мешей.
- **audio** — miniaudio: 3D-звук, шины и микшер, эффекты, окклюзия.
- **ai** — Recast/Detour (навмеш, толпа, динамические препятствия), behavior trees, восприятие, utility AI.
- **net** — ENet-транспорт и транспорт в памяти, репликация, интерполяция, предсказание на клиенте, выделенный сервер.
- **script** — Lua 5.4 + sol2: песочница, скрипты сущностей со свойствами, hot reload.
- **async** — корутины C++20 (`Task`/`Future`/`Generator`, ожидание кадров и времени, переходы между потоками,
  `whenAll`/`whenAny`, отмена, планировщик), мост `await` для Lua, RPC-помощник поверх `net`.
- **ui** — собственные rhi-бэкенды для Dear ImGui (отладочный оверлей: статистика, консоль, cvar'ы и
  масштабируемость, инспектор, render graph, корутины) и RmlUi (игровой UI с моделями данных, hot reload, Lua),
  примеры меню, включая настройки графики.
- **world** — ландшафт с эрозией и CDLOD, растительность, солнце и луна (NOAA), небо Preetham, смена дня и ночи,
  ветер и погода, вода Герстнера с плавучестью, стриминг чанков.
- **gameplay** — ECS-компоненты и системы для физики, анимации, сплайнов, звука, ИИ, скриптов, сети и корутин;
  Lua API сущностей; провайдеры ассетов; компоненты мира; предсказываемые персонажи с повтором ввода.
- **runtime** — `Engine`, конвейер потоков игры и рендера, ввод в стиле Enhanced Input, проекты `.oxproj`
  и пользовательские настройки, фреймворк сохранений, консоль, `AssetManager` с поддержкой pak.

### Приложения и инструменты
- **OxwaldPlayer** — автономный рантайм: окно GLFW или headless, выделенный сервер, Vulkan-рендерер и UI.
- **OxwaldEditor** (Qt 6) — работает на настоящем движке: вьюпорт на Vulkan, outliner, инспектор на основе
  рефлексии, браузер контента с миниатюрами, консоль, статистика, undo/redo, play-in-editor, панели корутин,
  behavior trees и сохранений, свойства скриптов, инструменты сплайнов и ландшафта, настройки проекта
  и масштабируемости, тёмная и светлая темы, интерфейс RU/EN.
- **Инструменты** — `oxdump` (бинарные файлы ↔ JSON), `oximport` (импорт ассетов), `oxpack` (упаковка проекта в pak).

### Документация
- Руководство пользователя на русском (`docs/guide/`) с компилируемыми и тестируемыми примерами
  (`samples/guide_examples/`), документация для разработчиков (`docs/dev/`).

### Полировка перед выпуском
- CVar'ы: проверка дубликатов без учёта регистра, слушатели изменений (`CVarRegistry::addChangeListener`), алиасы
  значений enum (`r.Upscaler.Quality DLAA` = `Native`); `r.*`/`sg.*` из консоли, `set()`/`execute()` и Lua
  применяют графические настройки; встроенный Lua-API `cvar.get/set/...`; `log.*` в Lua учитывает `minLevel`,
  stderr-лог можно отключить.
- Рантайм: список сохранений читает только заголовки, `registerSaveableScoped` (RAII), предупреждения об опечатках
  в источниках ввода и модулях проекта, `rendering.rayTracingIfSupported` и стартовые настройки из проекта при первом
  запуске, автоопределение качества только при первом запуске, `--patch-pak`.
- Сцена: компоненты-теги (пустые структуры); `CameraComponent::focusDistance/focalLength` управляют DoF.
- Рендер: эмиссия у Unlit-материалов, clear coat, `LightComponent::volumetric/volumetricIntensity` в объёмном тумане,
  запекание проб (редактор: Tools → Bake Lighting Probes, загрузка из `Baked/` в редакторе и плеере), флаги
  `renderToImage`, имена таймингов `"<вид>/<проход>"`, потокобезопасная регистрация ресурсов, кэш пайплайнов
  Vulkan на диске, регистрация компонентов рендера в плеере.
- RHI: ручные GPU-зоны Tracy, include-каталоги для inline-шейдеров, исправлено падение при создании оконного
  устройства после headless (vk-bootstrap), атомарная запись кэша пайплайнов, гонка развёртывания Vulkan runtime.
- Сеть/async: репликация массивов и словарей (`NetCodec<std::vector/std::map>`, reflected-поля в gameplay),
  публичный `MemoryHarness`, отмена отдельного RPC-запроса.
- Прочее: `AudioEngine::loadSound` через VFS, `onUnload` при уничтожении стримера, потокобезопасные константные
  запросы сплайнов, `Animator` разделяет владение скелетом, исправлена гонка в адаптере job system Jolt
  (падение `PhysicsWorld.CustomJobExecutor`), `oxpack` упаковывает `.rml/.rcss/.json` и `Baked/`, создаёт патч-паки.
- Сборка: overlay-порт MoltenVK сохраняет корректную подпись кода, `OX_WARNINGS_AS_ERRORS` включён в пресетах
  `dev`/`debug`, предупреждение компоновщика о повторных библиотеках отключено, NGX-рантайм копируется рядом с
  исполняемыми файлами.
- По итогам Showcase: автоэкспозиция откалибрована на средний серый (+0.8 EV) и усредняет 50–90 перцентили;
  8-битные скайбоксы масштабируются `ldrSkyLuminance` / `r.Sky.LdrLuminance`; сетевой рантайм опрашивает транспорт
  без мира (смена уровня не обрывает подключение); `createPrefab` детерминирован; скриншоты в оконном режиме.
