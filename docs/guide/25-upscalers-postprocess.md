# 25. Апскейлеры, сглаживание и постобработка

> Модуль `render`, таргет `Oxwald::render`, пространство имён `ox::render`. API — `<oxwald/render/features/postprocess/postprocess.hpp>`, компонент объёма — `<oxwald/render/components/postprocess.hpp>`. Фичи регистрирует `registerPostProcessFeatures` (вызывается из `Renderer::create`).

## Зачем

Последний участок кадра решает две задачи.

1. **Чёткость и скорость.** Сглаживание (TAA, FXAA) убирает «лесенки» и мерцание тонкой геометрии. Апскейлеры (FSR 1, TAAU, DLSS) рисуют сцену в меньшем разрешении и восстанавливают полное, отдавая 30–60 % времени кадра на другие эффекты.
2. **«Киношность».** Экспозиция и адаптация глаза, bloom, глубина резкости, размытие в движении, баланс белого и цветокоррекция, эффекты объектива (виньетка, хроматическая аберрация, зерно).

Технический художник управляет второй частью через **PostProcessVolume** — объём на сцене, как в Unreal: глобальный «look» уровня плюс локальные коробки (пещера, подводная часть, зона взрыва) с плавным переходом. Программист и меню графики управляют первой частью через cvar'ы `r.AntiAliasing`, `r.Upscaler`, `r.Upscaler.Quality`.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| Разрешение рендера / выхода | Сцена рисуется в *render*-разрешении, на экран выходит *output*. Их отношение задаёт режим качества апскейлера или `r.ScreenPercentage` |
| Джиттер | Субпиксельный сдвиг проекции каждый кадр (последовательность Halton). Из истории кадров временные методы (TAA, TAAU, DLSS) собирают сглаженное изображение и детали выше render-разрешения |
| Временной апскейлер | TAAU или DLSS: использует джиттер, векторы движения и историю и **заменяет** TAA |
| Пространственный апскейлер | FSR 1 (EASU + RCAS): масштабирует один кадр. AA ему нужен отдельно (TAA/FXAA в render-разрешении) |
| DLAA | DLSS в режиме `Native` (100 %): только сглаживание нейросетью, без апскейла |
| Pre-exposure | HDR-кадр хранится умноженным на экспозицию (с задержкой 3 кадра), чтобы FP16 не терял точность ни в 300 лк, ни в 100 клк |
| `PostProcessVolumeComponent` | Объём постобработки (имя в редакторе — `PostProcessVolume`) |
| Категория | Группа настроек с флагом `override*` (`overrideBloom`, `overrideGrading`…). Объём меняет только категории с поднятым флагом |

## Шаг 1. Цепочка кадра

| Точка | Фича (порядок) | Что делает |
| --- | --- | --- |
| PreDepth | AutoExposure | Pre-exposure этого кадра из GPU-результата кадра 3 кадра назад |
| BeforePostProcess | AutoExposure (−100) | Гистограмма логарифма яркости (128 корзин, центр весомее) → среднее между перцентилями → адаптация EV100 |
| BeforePostProcess | TAA \| TAAU \| DLSS (0) | TAA — в render-разрешении; TAAU и DLSS выдают уже **output**-разрешение, и постобработка дальше идёт в нём |
| PostProcess | DepthOfField (200), MotionBlur (300), Bloom (400) | Работают в том разрешении, которое сейчас у `SceneColorHDR` |
| Upscale | FSR1 | EASU + RCAS после постобработки; временные апскейлеры только занимают слот |
| AfterUpscale | Sharpen (50), PostComposite (100) | CAS-резкость; хроматическая аберрация → bloom + грязь на линзе → виньетка → баланс белого и LUT цветокоррекции |
| core Tonemap | | ACES / AgX / Neutral / Linear (`r.Tonemapper`) |
| Overlay | LdrPost (−100000) | FXAA, пользовательская LUT-текстура, зерно — до оверлеев редактора и UI |

Цветокоррекция стоит **до** тонмаппера: параметры запекаются в 32³ LUT в лог-пространстве (от −10 до +6.5 EV относительно среднего серого). Поэтому ACES, AgX и Neutral получают уже скорректированную картинку. Пользовательская LUT-текстура (display referred, как в UE) применяется **после** тонмаппинга.

## Шаг 2. Сглаживание: TAA и FXAA

`r.AntiAliasing`: `0` — нет, `1` — FXAA, `2` — TAA.

| | FXAA | TAA |
| --- | --- | --- |
| Принцип | Ищет края в готовом LDR-кадре и размывает вдоль них | Накопление джиттерированных кадров с отсечением истории |
| Тонкие провода, мерцание | Не лечит | Лечит |
| Ghosting | Нет | Возможен на быстрых объектах без векторов движения (частицы, прозрачные) |
| Цена, 1080p, M4 Pro | 0.11 мс | 0.56 мс (+ CAS 0.18 мс) |
| Нужен апскейлерам | FSR 1 может взять его вход | Временные апскейлеры заменяют TAA |

Измерено на сцене тонких стержней против эталона 16× SSAA: ошибка на краях 16.0 без AA, 12.3 с FXAA и 7.4 с TAA (PSNR 30.0 → 32.8 → 38.5 дБ).

| CVar | По умолчанию | Low / Medium / High / Ultra (группа AntiAliasing) |
| --- | --- | --- |
| `r.AntiAliasing` | 0 | None, FXAA, TAA, TAA |
| `r.TAA.Quality` | 2 | 0, 1, 2, 3 (min/max clamp → variance clip → +Catmull-Rom/dilation/anti-flicker → шире) |
| `r.AntiAliasing.Samples` | 8 | 4, 8, 8, 16 (период джиттера TAA) |
| `r.FXAA.Quality` | 2 | 0, 1, 2, 3 (4/8/12/16 шагов поиска края) |
| `r.TAA.Sharpness`, `r.TAA.CurrentFrameWeight`, `r.TAA.AntiFlicker` | 0.25, 0.08, true | — |

MSAA в рендерере нет (forward+ без MSAA-целей), поэтому временной AA — основной путь.

## Шаг 3. Апскейлеры: FSR 1, TAAU, DLSS

`r.Upscaler` = `Off`, `FSR1`, `DLSS`, `TAAU`; `r.Upscaler.Quality` — режим:

| `UpscalerQuality` | Масштаб рендера | 1080p-выход | 4K-выход |
| --- | --- | --- | --- |
| `UltraPerformance` | 33 % | ≈ 640×360 | ≈ 1280×720 |
| `Performance` | 50 % | 960×540 | 1920×1080 |
| `Balanced` | 58 % | — | — |
| `Quality` (по умолчанию) | 67 % | — | — |
| `Native` (DLAA для DLSS) | 100 % | 1920×1080 | 3840×2160 |

Апскейлер сам задаёт render-разрешение (перекрывает `r.ScreenPercentage`), длину джиттера и mip bias материалов (текстуры остаются чёткими):

```cpp
#include <oxwald/render/features/postprocess/postprocess.hpp>

f32 scale = upscalerRenderScale(UpscalerQuality::Performance);   // 0.5
f32 bias  = upscalerMipBias(UpscalerType::DLSS, scale);           // log2(0.5) − 1 = −2 (у FSR1/TAAU: −1)
u32 phases = upscalerJitterPhases(scale);                         // 8 × (1/0.5)² = 32 (в пределах 8..64)
```

Полный пример: `samples/guide_examples/25-upscalers-postprocess/upscalers.cpp` (тест `QualityModesMath`).

| | FSR 1 | TAAU | DLSS |
| --- | --- | --- | --- |
| Тип | Пространственный (EASU + RCAS, порт MIT на GLSL) | Временной (ядро TAA с историей в output-разрешении) | Нейросеть NVIDIA (NGX, Vulkan) |
| AA | Берёт кадр после TAA/FXAA в render-разрешении | Встроен | Встроен (DLAA при 100 %) |
| Платформы | Везде | Везде | NVIDIA RTX на Windows/Linux |
| Качество (50 %, тест против SSAA / билинейного) | 37.7 дБ против 35.0 | 32.1 дБ против 25.9 | на Mac не измерить |
| Цена 540p→1080p, M4 Pro | EASU 0.32 + RCAS 0.15 мс | 0.55 мс (+0.18 CAS) | — |

`r.Upscaler.Sharpness` (0.2) управляет резкостью: RCAS для FSR, CAS для TAAU, sharpness для DLSS. Что выбирается в кадре:

```cpp
RenderSettings s;
s.antiAliasing = 2;                         // TAA
s.upscaler = i32(UpscalerType::FSR1);       // → фичи TAA + FSR1
s.upscaler = i32(UpscalerType::TAAU);       // → TAAU (TAA выключается)
s.upscaler = i32(UpscalerType::DLSS);       // → DLSS, а где его нет — TAAU
```

Полный пример: `samples/guide_examples/25-upscalers-postprocess/upscalers.cpp` (тест `FeatureSelection`) и `postprocess_gpu.cpp` (тест `TaauRendersAtHalfResolution`: при TAAU Performance вид рендерит 128×128 при выходе 256×256).

### Доступность для меню

```cpp
for (const UpscalerAvailability& a : upscalerAvailability(&device)) {
    // a.type, a.name ("AMD FSR 1.0", "NVIDIA DLSS"…), a.available, a.temporal, a.reason
    combo.addItem(a.name, a.available, a.reason);   // недоступный пункт — серый, с подсказкой
}
```

Без устройства (`upscalerAvailability()`) известны только статические причины: платформа, сборка, вендор. С устройством DLSS проверяется через NGX (один раз на устройство). Так же устроены редактор (**Project Settings → Rendering → Anti-Aliasing & Upscaling**: недоступный пункт серый, плюс баннер «NVIDIA DLSS unavailable») и **Game User Settings**.

## Шаг 4. DLSS

**Требования**

| Что | Требование |
| --- | --- |
| GPU | NVIDIA RTX (Turing и новее, тензорные ядра) |
| ОС | Windows x64 или Linux x64/aarch64. На macOS DLSS нет: NVIDIA NGX не поддерживает Metal/MoltenVK |
| Драйвер | Достаточно новый: иначе причина «NVIDIA DLSS needs a newer driver (at least X.Y)» |
| Сборка | CMake-опция `OX_ENABLE_DLSS` (по умолчанию `ON`) и порт vcpkg `nvidia-dlss` с рантаймом |
| Vulkan-расширения | Нужные NGX расширения добавляет `appendUpscalerVulkanExtensions(desc)` **до** `rhi::Device::create` |

**Как это собирается.** Оверлей-порт `vcpkg-overlays/ports/nvidia-dlss` на Windows и Linux скачивает NVIDIA DLSS SDK (git с LFS) и ставит заголовки, статическую библиотеку NGX и рантайм. На остальных платформах ставятся только заголовки. `find_package(nvidia-dlss)` даёт:

- `NVIDIA::DLSSHeaders` — всегда; на macOS через него компилируется (но не линкуется) `dlss_ngx.cpp` в объектной библиотеке `ox_dlss_syntax_check`, чтобы код NGX не «сгнил»;
- `NVIDIA::DLSS` — только с рантаймом. Тогда в `ox_render` линкуется `dlss_ngx.cpp` и появляется публичный дефайн `OX_RENDER_HAS_DLSS=1`. Иначе работает заглушка `dlss_stub.cpp`, которая отвечает причиной;
- `NVIDIA_DLSS_RUNTIME_FILES` → кэш-переменная `OX_DLSS_RUNTIME_FILES`: на Windows это `*.dll` из `bin/` порта (`nvngx_dlss.dll`), на Linux — `libnvidia-ngx-*.so.*`.

**Где NGX ищет рантайм.** `NVSDK_NGX_VULKAN_Init_with_ProjectID` получает единственный путь поиска фич — **текущий рабочий каталог процесса** (`std::filesystem::current_path()`). Служебные данные NGX пишет в `<temp>/oxwald_ngx`. Значит, файлы из `OX_DLSS_RUNTIME_FILES` должны лежать в каталоге, из которого запускается игра. Автоматического копирования к исполняемым файлам пока нет — добавьте в свой таргет:

```cmake
if(OX_DLSS_RUNTIME_FILES)
    add_custom_command(TARGET MyGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OX_DLSS_RUNTIME_FILES} $<TARGET_FILE_DIR:MyGame>)
endif()
```

и запускайте игру из каталога исполняемого файла (или кладите DLL рядом с рабочим каталогом ярлыка).

**Как включить**

```cpp
rhi::DeviceDesc desc;
appendUpscalerVulkanExtensions(desc);                 // рантайм-рендерер делает это сам
auto device = rhi::Device::create(desc);
// …
if (upscalerAvailability(UpscalerType::DLSS, device.get()).available) {
    CVarRegistry::instance().set("r.Upscaler", "DLSS");
    CVarRegistry::instance().set("r.Upscaler.Quality", "Quality");   // или "Native" — DLAA
}
```

Из консоли: `r.Upscaler DLSS`, `r.Upscaler.Quality Native`. Если DLSS недоступен, `r.Upscaler=DLSS` работает как TAAU: выставлять его «на всякий случай» безопасно.

**Возможные причины недоступности** (`UpscalerAvailability::reason`):

| Причина | Что делать |
| --- | --- |
| «…requires an NVIDIA RTX GPU on Windows or Linux (not available on macOS / MoltenVK)» | Ничего: платформа |
| «this build was configured with OX_ENABLE_DLSS=OFF» | Пересобрать с `-DOX_ENABLE_DLSS=ON` |
| «this build has no NVIDIA DLSS runtime…» | Порт `nvidia-dlss` без рантайма (не та платформа или нет git LFS при сборке порта) |
| «NVIDIA DLSS requires an NVIDIA RTX GPU (this GPU: …)» | Не NVIDIA |
| «NGX initialisation failed: …» | Чаще всего нет `nvngx_dlss.dll` / `.so` в рабочем каталоге |
| «NVIDIA DLSS needs a newer driver (at least X.Y)» | Обновить драйвер |
| «NVIDIA DLSS is not supported on this GPU/driver: …» | GTX без тензорных ядер и т. п. |

Что передаётся в DLSS: цвет в HDR (`IsHDR`), глубина с reversed-Z (`DepthInverted`), векторы движения в render-разрешении (`MVLowRes`, масштаб `−render size`), джиттер в пикселях рендера, текстура экспозиции и `InPreExposure`. Без текстуры экспозиции включается флаг `AutoExposure`. При смене камеры история сбрасывается, при смене размера или качества фича пересоздаётся. Frame generation и ray reconstruction не интегрированы.

> На Mac DLSS только компилируется. Запуск на RTX стоит один раз проверить с отладочным оверлеем NGX: знак джиттера взят по соглашению UE. Тест `PostProcessTest.DlssRendersOnRtx` в `ox_render_gpu_tests` на RTX-машине сравнивает DLSS Quality с нативным кадром (PSNR > 25 дБ).

## Шаг 5. Постобработка

Эффекты, меняющие картинку, в коде по умолчанию **выключены**: голый `Renderer` в тестах и инструментах рисует «чистый» кадр. Рантайм и редактор включают их уровнем масштабируемости (или `applyRecommendedSettings`, шаг 6). Сила эффектов задаётся объёмами (шаг 7).

| CVar | По умолчанию в коде | Low / Medium / High / Ultra (группа PostProcess) |
| --- | --- | --- |
| `r.Bloom` | false | on, on, on, on |
| `r.Bloom.Quality` | 3 | 1, 2, 3, 4 (4 + q мипов) |
| `r.DepthOfField` | false | off, on, on, on |
| `r.DOF.Quality` | 2 | 0, 1, 2, 3 (2 + q колец выборки) |
| `r.MotionBlur` | false | off, on, on, on |
| `r.MotionBlur.Quality` | 2 | 0, 1, 2, 3 (4 + 4q выборок) |
| `r.Vignette` / `r.ChromaticAberration` / `r.FilmGrain` | true | CA и зерно выключены на Low |
| `r.Bloom.Intensity` 0.04, `r.Sharpen` 0, `r.ColorGrading` true | | — |
| `r.Exposure.Auto` false, `r.Exposure.MinEV100` −4, `.MaxEV100` 20, `.SpeedUp` 3, `.SpeedDown` 1 (EV/с) | | — |
| `r.Tonemapper` | ACES (ACES, AgX, Neutral, Linear) | — |
| `r.Exposure.Mode` / `.EV100` / `.Compensation` | Camera / 10 / 0 | — |

Что делает каждый эффект:

- **Экспозиция.** По умолчанию — физическая камера: EV100 из `CameraComponent` (диафрагма, выдержка, ISO). С `autoExposure` строится гистограмма: среднее яркости между перцентилями 70 и 95, адаптация вверх 3 EV/с, вниз 1 EV/с, в пределах `minEV100..maxEV100`. На тестовой сцене авто-экспозиция из 300 лк и из 100 клк сходится к одинаковой яркости.
- **Bloom.** Без порога, с сохранением энергии: `bloomIntensity` — доля кадра, заменённая его размытой версией. Светятся только по-настоящему яркие пиксели (emissive, солнце в отражениях). Грязь на линзе — `bloomDirtTexture` × `bloomDirtIntensity`.
- **Глубина резкости.** Физическая камера: `focusDistance` (0 — DoF выключен), `aperture` (f-число; 0 — из `CameraComponent::aperture`), `focalLength` (мм; 0 — из вертикального FOV на полнокадровой матрице 24 мм), `maxBokehSize` (% ширины кадра). В `CameraComponent` нет дистанции фокуса, поэтому DoF включается **только объёмом**.
- **Размытие в движении.** По объектам и камере (реконструкция McGuire): `motionBlurAmount` — доля выдержки (0.5 = затвор 180°), `motionBlurMax` — предел длины (% ширины кадра).
- **Баланс белого.** `temperature` — температура источника, который станет нейтральным, как White Temp в UE: меньше — холоднее картинка. `tint`: −1 зелёный … +1 пурпурный.
- **Цветокоррекция.** `saturation`, `contrast` (вокруг 18 % серого в лог-пространстве), `lift`/`gamma`/`gain` в стиле ASC-CDL. Запекается в 32³ LUT; LUT пересчитывается только при изменении параметров.
- **Пользовательская LUT.** Полоса N²×N (например, 1024×32 или 256×16): красный — по X внутри среза, зелёный — по Y, синий — по срезам. Применяется после тонмаппера с силой `lutIntensity`.
- **Объектив.** `vignetteIntensity`, `chromaticAberration` (до ~0.5 % кадра в углах), `filmGrainIntensity`, `sharpen` (CAS).

Цена на M4 Pro, 1080p (мс): авто-экспозиция 0.04, bloom (7 мипов) ≈ 0.3, DoF ≈ 0.2–0.35, motion blur ≈ 0.22, HDR-композит (CA + bloom + виньетка + LUT) 0.2, LDR-пост (зерно, LUT) 0.11.

## Шаг 6. «Auto»: рекомендация по железу

```cpp
const RecommendedSettings rec = recommendedSettings(device, benchmarkScore);   // NGX проверяется по устройству
OX_LOG_INFO("render", "{}", rec.rationale);   // "score 10 (Low): FSR 1 at 58 % + TAA (DLSS unavailable)"
applyRecommendedSettings(rec);                // уровни всех групп + r.AntiAliasing, r.Upscaler, r.Upscaler.Quality
```

| Балл бенчмарка (100 = GTX 1060) | Уровень | С DLSS (RTX) | Без DLSS |
| --- | --- | --- | --- |
| ≥ 160 | Ultra | DLSS `Native` (DLAA) | Нативно + TAA |
| 80–160 | High | DLSS `Quality` | Нативно + TAA |
| 35–80 | Medium | DLSS `Balanced` | TAAU `Quality` |
| < 35 | Low | DLSS `Performance` | FSR 1 `Balanced` + TAA |

Apple M4 Pro набирает около 112 баллов → High, нативное разрешение, TAA. Так работают `RuntimeRendererOptions::autoDetectQuality` и кнопка Auto-Detect в редакторе. Подробнее о бенчмарке — в [главе 26](26-quality-settings.md).

Полный пример: `samples/guide_examples/25-upscalers-postprocess/upscalers.cpp` (тесты `RecommendedSettings`, `ApplyRecommendedSettingsAndCVars`).

## Шаг 7. PostProcessVolume

Объём — компонент `PostProcessVolumeComponent` на сущности:

| Поле | По умолчанию | Смысл |
| --- | --- | --- |
| `enabled` | true | Учитывать ли объём |
| `unbound` | true | Бесконечный (глобальные настройки уровня); false — коробка |
| `extents` | (5, 5, 5) м | Половинные размеры коробки в локальном пространстве (умножаются на масштаб сущности) |
| `blendRadius` | 1 м | На каком расстоянии **снаружи** коробки вес падает до 0 |
| `blendWeight` | 1 | Максимальный вес 0..1 |
| `priority` | 0 | Объёмы применяются по возрастанию приоритета; больший побеждает |
| `settings` | — | `PostProcessSettings`: флаги `override*` и значения категорий |

Категории `PostProcessSettings` и значения по умолчанию:

| Категория (флаг) | Поля (по умолчанию) |
| --- | --- |
| Экспозиция (`overrideExposure`) | `autoExposure` (false), `exposureCompensation` (0 EV, сверх `r.Exposure.Compensation`), `minEV100` (−4), `maxEV100` (20), `adaptationSpeedUp` (3 EV/с), `adaptationSpeedDown` (1 EV/с), `histogramLowPercent` (50), `histogramHighPercent` (90); среднее между ними экспонируется как средний серый 18 % |
| Bloom (`overrideBloom`) | `bloomIntensity` (0.04), `bloomDirtTexture` (—), `bloomDirtIntensity` (0) |
| Глубина резкости (`overrideDepthOfField`) | `focusDistance` (0 = выкл.), `aperture` (0 = из камеры), `focalLength` (0 = из FOV), `maxBokehSize` (1.5 %) |
| Размытие в движении (`overrideMotionBlur`) | `motionBlurAmount` (0.5), `motionBlurMax` (5 %) |
| Баланс белого (`overrideWhiteBalance`) | `temperature` (6500 K), `tint` (0) |
| Цветокоррекция (`overrideGrading`) | `saturation` (1), `contrast` (1), `lift` (0, 0, 0), `gamma` (1, 1, 1), `gain` (1, 1, 1) |
| LUT (`overrideLut`) | `lutTexture` (—), `lutIntensity` (1) |
| Объектив (`overrideLens`) | `vignetteIntensity` (0), `chromaticAberration` (0), `filmGrainIntensity` (0), `sharpen` (0, иначе `r.Sharpen`) |

Пример: глобальный «look» уровня и холодная обесцвеченная пещера с плавным входом.

```cpp
#include <oxwald/render/components/postprocess.hpp>

Entity global = world.create("GlobalPostProcess");
auto& g = global.add<PostProcessVolumeComponent>();          // unbound по умолчанию
g.settings.overrideExposure = true;
g.settings.autoExposure = true;
g.settings.exposureCompensation = 0.5f;

Entity cave = world.create("CavePostProcess");
cave.setPosition({10.0f, 0.0f, 0.0f});
auto& c = cave.add<PostProcessVolumeComponent>();
c.unbound = false;
c.extents = glm::vec3(2.0f);        // коробка 4×4×4 м
c.blendRadius = 2.0f;               // плавный вход за 2 м
c.priority = 1;                     // поверх глобального
c.settings.overrideGrading = true;
c.settings.saturation = 0.0f;
c.settings.overrideWhiteBalance = true;
c.settings.temperature = 4000.0f;   // нейтрализуем тёплый свет факелов → картинка холоднее
```

**Как смешивается.**

1. Базу дают cvar'ы: `postProcessDefaults(settings)` берёт `r.Exposure.*`, `r.Bloom.Intensity`, `r.Sharpen`.
2. Вес каждого объёма в позиции камеры (`postProcessVolumeWeight`): у unbound это `blendWeight`; у коробки — `blendWeight` внутри и линейный спад до 0 на расстоянии `blendRadius` снаружи.
3. Объёмы с весом > 0 применяются по возрастанию `priority`. Для каждой категории с флагом `override*` числа интерполируются: `lerp(текущее, объём, вес)`. Дискретные значения (флаги, текстуры) переключаются при весе ≥ 0.5. Категории без флага объём не трогает.

```cpp
// У входа в пещеру (1 м снаружи, вес 0.5): bloom = lerp(0.2, 0.6, 0.5) = 0.4, saturation = 0.5.
PostProcessSettings s = blendPostProcessVolumes(postProcessDefaults(settings), volumes, cameraPos);
// Или прямо по снапшоту кадра:
PostProcessSettings s2 = resolvePostProcessSettings(snapshot, settings, cameraPos);
```

Полный пример: `samples/guide_examples/25-upscalers-postprocess/postprocess_volumes.cpp` (тесты `WeightsByDistance`, `BlendByPriorityPerCategory`, `ComponentOnSceneAndSerialization`) и `postprocess_gpu.cpp` (тест `BloomFromVolume`).

> Объём задаёт **силу** эффекта, а cvar — **включён ли** он. Bloom с `bloomIntensity = 0.5` не появится, если `r.Bloom` выключен (уровень PostProcess или голый `Renderer` в тестах). DoF дополнительно требует `focusDistance > 0`.

## Типичные ошибки и подводные камни

- **Мыло после включения апскейлера.** Режим `Performance` или `UltraPerformance` на 1080p даёт 540p/360p. Для 1080p берите `Quality`; `Performance` рассчитан на 4K-выход.
- **Двойное сглаживание.** TAAU и DLSS заменяют TAA. FSR 1 его не заменяет — для FSR держите `r.AntiAliasing = 2` (TAA) или хотя бы FXAA.
- **Ghosting за частицами и прозрачными объектами.** Они не пишут векторы движения (реактивной маски пока нет). Уменьшите размер быстрых частиц или используйте `StretchedBillboard` ([глава 23](23-transparency-water-particles.md)).
- **Тонкие провода «рвутся» в TAAU 50 %.** Субпиксельные детали TAAU сохраняет частично. Поднимите режим или используйте DLSS.
- **DLSS «включён», но работает TAAU.** Это фолбэк: смотрите `upscalerAvailability(UpscalerType::DLSS, &device).reason`. Частая причина на Windows — `nvngx_dlss.dll` нет в *рабочем каталоге* процесса.
- **`DLAA` и `Native` — одно и то же.** Канонически значение называется `Native` (так его печатают консоль и
  `toString()`), но cvar принимает и синоним `DLAA` (`r.Upscaler.Quality DLAA`) — им пользуются меню и
  `UserSettings::graphics.upscalerQuality`.
- **DoF не появляется.** У `CameraComponent` нет дистанции фокуса: задайте `focusDistance` в объёме с `overrideDepthOfField`, и включите `r.DepthOfField`.
- **Эффекты есть в игре, но нет в тестах.** В коде bloom, DoF и motion blur выключены по умолчанию — включите их cvar'ами или уровнем PostProcess.
- **Грязь на линзе и LUT появляются с задержкой.** Текстуры берутся из кэша ресурсов, только когда загружены.
- **FXAA и зерно на debug view.** Они идут после решения тонмаппера о debug view, поэтому режимы отладки тоже получают FXAA и зерно. Для чистых debug-скриншотов выключите их.
- **Резкая граница объёма.** `blendRadius = 0` даёт скачок на границе коробки. Для переходов ставьте 1–3 м.
- **Порядок объёмов «не тот».** Решает `priority`, а не порядок сущностей. При равных приоритетах порядок зависит от порядка сущностей в снапшоте — задавайте разные.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`features/postprocess/postprocess.hpp`](../../engine/render/include/oxwald/render/features/postprocess/postprocess.hpp) | `UpscalerType`, `UpscalerQuality`, `upscalerName`, `upscalerRenderScale`, `upscalerMipBias`, `upscalerJitterPhases`, `UpscalerAvailability`, `upscalerAvailability`, `appendUpscalerVulkanExtensions`, `RecommendedSettings`, `recommendedSettings`, `applyRecommendedSettings`, `PostProcessVolumeSnapshot`, `postProcessVolumeWeight`, `postProcessDefaults`, `blendPostProcessVolumes`, `resolvePostProcessSettings`, `registerPostProcessFeatures` |
| [`components/postprocess.hpp`](../../engine/render/include/oxwald/render/components/postprocess.hpp) | `PostProcessSettings`, `PostProcessVolumeComponent`, `registerPostProcessTypes` |
| [`render_settings.hpp`](../../engine/render/include/oxwald/render/render_settings.hpp) | `RenderSettings::antiAliasing`, `upscaler`, `upscalerQuality`, `screenPercentage`; `registerRenderCVars` |
| [`render_view.hpp`](../../engine/render/include/oxwald/render/render_view.hpp) | `RenderView::renderExtent`, `outputExtent`, `jitterPixels` |

Сборка DLSS: [`engine/render/CMakeLists.txt`](../../engine/render/CMakeLists.txt), порт [`vcpkg-overlays/ports/nvidia-dlss`](../../vcpkg-overlays/ports/nvidia-dlss/portfile.cmake). Шейдеры: `engine/shaders/render/postprocess/`. Для разработчиков модуля: [`docs/dev/modules/render_postprocess.md`](../dev/modules/render_postprocess.md).

## Что дальше

- [04. CVars и качество](04-cvars-quality.md) — `UserSettings` (`upscaler`, `upscalerQuality`) и меню графики.
- [26. Настройки качества](26-quality-settings.md) — бенчмарк и автоопределение.
- [24. Трассировка лучей](24-ray-tracing.md) — RTX на тех же картах, что и DLSS.
- [23. Прозрачность, вода и частицы](23-transparency-water-particles.md) — яркие частицы для bloom.
- [30. Редактор](30-editor.md) — Project Settings → Rendering, объёмы в инспекторе.
- [Оглавление](README.md).
