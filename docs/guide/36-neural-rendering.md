# 36. Нейронный рендеринг: MLP на GPU, нейросжатие текстур, нейроденойзер

> Модули `assets` (CPU: `<oxwald/assets/neural/mlp.hpp>`, `<oxwald/assets/neural/ntc.hpp>`) и `render`
> (GPU: `<oxwald/render/neural/neural.hpp>`, пространство имён `ox::render::neural`). Шейдеры —
> `engine/shaders/render/neural/`. Внешних зависимостей нет: ни PyTorch, ни CUDA — обучение на C++, инференс в GLSL.

## Зачем

Маленькая нейросеть (многослойный перцептрон, MLP) на несколько тысяч весов умеет заменять в рендерере «ручные»
формулы и форматы данных:

- **нейросжатие текстур (NTC)** — все текстуры материала (albedo + альфа, нормаль, occlusion/roughness/metallic,
  emissive) кодируются вместе в квантованные латентные сетки и крошечный декодер. На диске материал занимает в
  несколько раз меньше, чем BC7/BC5, а в режиме *InferenceOnSample* меньше и в видеопамяти: декодер выполняется прямо
  в шейдере материала. Идея — работа NVIDIA «Random-Access Neural Compression of Material Textures» (2023);
- **нейроденойзер** — вместо подобранных вручную весов фильтра À-trous в денойзере SVGF ядро для каждого пикселя
  предсказывает сеть, обученная на эталонах path tracer'а движка (`r.Denoiser Neural`);
- **инфраструктура** — загрузка сетей на GPU и их вычисление в compute- и fragment-шейдерах тремя способами: обычные
  FP32/FP16-вычисления (работают везде, включая MoltenVK и Android), тензорные ядра через
  `VK_KHR_cooperative_matrix` и `VK_NV_cooperative_vector`.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `ox::nn::Mlp` | Полносвязная сеть на CPU: эталонный инференс, обучение (обратное распространение + Adam), сериализация. Детерминирована: один seed — одинаковые веса на любой машине и при любом числе потоков |
| `packFp16` | Упаковка весов для GPU: FP16, строки дополнены до кратных 16 (тайлы тензорных ядер), блоки выровнены на 64 байта |
| `NeuralNetwork` / `NeuralInference` | Веса в GPU-буфере (BDA) / пакетное вычисление в compute-шейдере |
| Путь (`NeuralPath`) | `ScalarF32`, `ScalarF16`, `CoopMatrix` (только compute), `CoopVector` (compute и fragment) |
| `NeuralTextureSet` (`.oxntc`) | Сжатый набор текстур материала: латенты, декодер, BC-«хвост» мелких мипов (fallback) |
| Латентная пара | Две сетки признаков G0 (1/stride разрешения) и G1 (вдвое грубее) на каждые два мипа текстуры |
| Fallback-хвост | Мипы не больше `fallbackSize` (по умолчанию 128) хранятся обычным BC. Ими же пользуются RT-хиты, тени с альфа-тестом, захват проб — всё, что не умеет вызывать декодер |
| Стохастическая фильтрация (STF) | Декодер считает один тексел одного мипа на сэмпл; мип, тексел и смещение вдоль анизотропного следа выбираются случайно с весами трилинейного/анизотропного фильтра, усредняет TAA |

## Шаг 1. Сеть на CPU и на GPU

```cpp
#include <oxwald/assets/neural/mlp.hpp>
#include <oxwald/render/neural/neural.hpp>

ox::nn::Mlp net = ox::nn::Mlp::make(32, {64, 64}, 16);   // 32 -> 64 -> 64 -> 16, ReLU, линейный выход
net.initialize(42);

ox::nn::TrainOptions o;                                 // MSE + Adam, косинусное затухание шага
o.steps = 2000;
o.batch = 256;
ox::nn::TrainResult r = ox::nn::trainRegression(net, inputs, targets, count, o);
net.roundToHalf();                                      // ровно те веса, что увидит GPU

using namespace ox::render::neural;
NeuralNetwork gpu;
gpu.upload(device, net);                                // FP16-веса в device-local буфере
NeuralInference inference(device);
std::vector<float> outputs;
inference.evaluate(gpu, inputs, count, outputs, bestComputePath(device.caps()));
```

В своих шейдерах подключите `render/neural/mlp.glsl` и сгенерируйте слои нужного размера макросами:

```glsl
#include <render/neural/mlp.glsl>
OX_NN_LAYER_F32(layer0, 32, 64)   // float x[32] -> float y[64]
OX_NN_LAYER_F32(layer1, 64, 64)
OX_NN_LAYER_F32(layer2, 64, 16)
// ... layer0(weights, w0, b0, true /*ReLU*/, x, h0); layer1(...); layer2(..., false, h1, y);
```

`OX_NN_LAYER_F16` (нужен `shaderFloat16`, define `OX_NN_FP16`) работает с `f16vec2`, `OX_NN_LAYER_CV` (define
`OX_NN_COOPVEC`) — с `coopvecNV`. Путь на тензорных ядрах для compute-шейдеров — `render/neural/mlp_coopmat.glsl`:
каждый subgroup прогоняет тайлы по 16 сэмплов через слои, активации лежат в shared memory, веса читаются прямо из
буфера. На RTX 3080 он вдвое медленнее cooperative vector (шаг 6), поэтому `bestComputePath` выбирает cooperative
vector, а cooperative matrix — только там, где `VK_NV_cooperative_vector` нет.

Возможности устройства — в `DeviceCaps` (`cooperativeMatrix`, `cooperativeMatrixFp16Accumulate`,
`cooperativeVector`, их стадии шейдеров, `shaderFloat16`, `storageBuffer16BitAccess`); `neuralCapsString(caps)`
печатает сводку. На RTX 3080 с драйвером 617.42 доступны оба расширения:

```
fp16 yes, cooperative matrix yes (fp16 accumulate yes), cooperative vector yes (fragment yes), subgroup 32;
compute path CoopVector, fragment path CoopVector
```

На MoltenVK и большинстве мобильных GPU расширений нет — используется скалярный путь, ничего настраивать не нужно.

## Шаг 2. Сжатие материала при импорте

NTC включается в `.meta` материала (`.oxmat`), импортёр `material`:

```json
{
  "importer": "material",
  "settings": {
    "neuralCompression": "Replace",
    "neuralPreset": "Medium",
    "neuralTargetBitsPerTexel": 0,
    "neuralFallbackSize": 128
  }
}
```

| Настройка | Значение |
| --- | --- |
| `neuralCompression` | `Off` — обычный материал; `On` — дополнительно обучить под-ассет `<материал>#NeuralTextures`, обычные текстуры остаются зависимостями; `Replace` — материал зависит только от нейронабора, `oxpack` не кладёт исходные текстуры в пак |
| `neuralPreset` | `Low` (stride 4, 4+4 признака, скрытый слой 32), `Medium` (stride 4, 12+8, 64), `High` (stride 2, 8+8, 64), `Custom` |
| `neuralTargetBitsPerTexel` | Если > 0, выбирается пресет, ближайший к заданному битрейту (бит на тексел mip 0 для всех каналов вместе; для сравнения: BC7 albedo + BC5 normal + BC7 ORM = 24) |
| `neuralLatentStride`, `neuralFeatures0/1`, `neuralLatentFormat` (`Q4`/`Q8`), `neuralHidden` (32/64) | Параметры для `Custom` |
| `neuralSteps` | Шаги обучения (0 = автоматически по числу текселей: 2 000–16 000) |
| `neuralFallbackSize` | Мипы не больше этого размера хранятся как BC |
| `neuralMaxSize`, `neuralSeed` | Уменьшить исходники перед обучением; seed (результат от него зависит, от числа потоков — нет) |

Импортёр берёт **исходные** PNG/JPG текстур материала (с их `flipY`/`maxSize`), а не уже сжатые BC — иначе потери
сложились бы. Обучение идёт на CPU во всех потоках; материал 512² — около 10 с, 2048² — несколько минут. В записи
импорта (`info` под-ассета) — битрейт, размер против BC7/BC5 и PSNR/SSIM по каналам.

Без проекта можно сжать набор картинок утилитой:

```sh
oximport --ntc --albedo rock_albedo.png --normal rock_normal.png --orm rock_orm.png \
         --settings '{"neuralPreset": "Medium"}' --out out/
# NeuralTextureSet 1024x1024, preset Medium, 3 neural mips + 8 BC mips, hidden 64
#   payload ... B (latents ..., network ..., fallback ...) = 3.9 bits/texel; BC7/BC5 baseline ... B (5.4x)
#   albedo     NTC  45.72 dB  SSIM 0.9861   |  BC  52.66 dB  SSIM 0.9969
oximport Assets/Materials/Rock.oxmat --project MyGame --settings '{"neuralCompression": "On"}'   # внутри проекта, .meta не меняется
```

Из кода — `ox::assets::encodeNeuralTextures(sources, settings)`, `serializeNeuralTextureSet`,
`decodeToTextures` (BC7/BC5), `evaluateNeuralTextures` (PSNR/SSIM против исходников и против BC).

## Шаг 3. Три режима в рантайме: `r.NeuralTextures`

| Значение | Что происходит | Экономия | Цена |
| --- | --- | --- | --- |
| `Off` | Обычные текстуры. Материалы с `neuralCompression: Replace` (исходников нет) декодируются, как в DecodeOnLoad | — | — |
| `DecodeOnLoad` (по умолчанию) | При загрузке материала набор декодируется в BC7/BC5 на рабочих потоках; пока идёт декодирование, материал показывает fallback-хвост | диск, загрузка из сети | время загрузки (CPU) |
| `InferenceOnSample` | Латенты (`B4G4R4A4` или `RGBA8`) и FP16-веса лежат в видеопамяти, шейдеры материала декодируют сэмпл | диск и VRAM, полоса памяти | время GPU в depth prepass и forward |

Переключается на лету (консоль, настройки, Lua): ресурсы прежнего режима освобождаются, материалы пересобираются
со следующего кадра. В редакторе — «Настройки проекта → Рендеринг → Neural Rendering».

```cpp
ox::CVarRegistry::instance().set("r.NeuralTextures", "InferenceOnSample");
auto stats = ox::render::neural::neuralTextureStats(renderer);   // материалы, латенты, веса, декодировано, в очереди
```

| CVar | По умолчанию | Группа Textures: Low / Medium / High / Ultra | Что делает |
| --- | --- | --- | --- |
| `r.NeuralTextures` | DecodeOnLoad | — (Persist) | режим |
| `r.NeuralTextures.Filter` | Stochastic | — (Persist) | `Stochastic` — один вызов декодера на сэмпл, шум убирает TAA; `Bilinear` — четыре вызова, без шума (для кадров без TAA, скриншотов, тестов) |
| `r.NeuralTextures.Prepass` | 1 | 0 / 0 / 1 / 1 | декодировать нормали и в depth prepass; 0 — prepass читает fallback (дешевле, буфер `Normals` для SSR/SSAO грубее) |
| `r.NeuralTextures.MinMip` | 0 | 2 / 1 / 0 / 0 | самый детальный нейро-мип в памяти и в шейдере; больше — меньше VRAM, мыльнее вблизи |
| `r.NeuralTextures.DecodeQuality` | 0 | 0 / 0 / 0 / 1 | уровень BC7-энкодера (UASTC) для DecodeOnLoad: время загрузки против качества |
| `r.NeuralTextures.MipBias` | 0 | — | смещение LOD нейро-сэмплирования |
| `r.NeuralTextures.Streaming` | on | — | резидентность латентных пар по плотности текселей |
| `r.NeuralTextures.ShaderPath` | Auto | — | путь MLP в шейдерах материала: Auto, Scalar, FP16, CoopVector (читается, когда рендерер впервые создаёт шейдеры с нейродекодером) |

Без TAA (`r.AntiAliasing` не 2) стохастическая фильтрация видна как шум — выберите `Bilinear` или включите TAA.

## Шаг 4. Как это устроено в кадре

1. Фича `NeuralTextures` (точка `PreDepth`) раз в кадр принимает загруженные наборы и результаты декодирования,
   управляет резидентностью латентов и публикует таблицу `GpuNeuralMaterial` (128 байт на набор) в
   `VIEW.neuralMaterials`.
2. В `GpuMaterial` нейроматериал помечен флагом `kMaterialNeural` и слотом таблицы в `flags >> 8`, а его индексы
   текстур указывают на fallback-хвост — поэтому трассировка лучей, тени с альфа-тестом, захват проб отражений и
   прозрачность работают без изменений (с более грубыми текстурами).
3. `forward.frag` и `depth_prepass.frag` вызывают `oxSampleMaterialView(...)` из `render/neural/ntc.glsl`: для
   обычного материала это прежний `oxSampleMaterial`, для нейроматериала — LOD по производным UV, случайный мип
   (трилинейно), тексел (билинейно) и точка вдоль большой оси анизотропного следа, затем декодер. Мипы не детальнее
   fallback-хвоста берутся из BC-текстур аппаратным фильтром.
4. Вход декодера (32 значения): билинейно выбранные признаки G0 и G1, синус/косинус положения внутри ячейки G0,
   чётность мипа. Выход — до 16 каналов в фиксированном порядке (`NtcChannelLayout`).

**Стриминг.** Fallback-хвосты и текстуры DecodeOnLoad регистрируются в `GpuResourceCache` как обычные текстуры —
ими управляет стример мипов (`r.Streaming.*`, глава 27). Латенты в InferenceOnSample стримятся парами: по той же
оценке плотности текселей (проекция ограничивающей сферы × тайлинг UV) фича держит в памяти только нужные пары и
выгружает лишние через 30 кадров.

## Шаг 5. Нейроденойзер: `r.Denoiser Neural`

Денойзер ray traced эффектов (тени, AO, отражения; глава 24) — SVGF: временное накопление, оценка дисперсии и четыре
прохода À-trous 5×5. С `r.Denoiser Neural` первые два этапа те же, а веса 24 соседних отсчётов в À-trous
предсказывает маленькая сеть (6 признаков → 16 → 16 → 1, своя на каждую итерацию): разницы глубины (относительно
градиента), нормали и яркости (относительно шума), расстояние, относительный уровень шума. Это kernel-predicting
подход, но сеть вызывается на каждый отсчёт, а не свёрточной сетью на кадр — так она переносима на любое
устройство без тензорных ядер.

Веса — сгенерированный файл `engine/shaders/render/neural/denoiser_weights.glsl` (поставляется со
шейдерами движка). Переобучить:

```sh
OX_TRAIN_NEURAL_DENOISER=1 ox_render_gpu_tests --gtest_filter=NeuralDenoiserTest.TrainWeights
```

Тест рендерит сцены path tracer'ом (эталон — 1536 сэмплов на пиксель, вход — 1, 2, 4 и 16 сэмплов) и синтетический
AO (1 и 8 сэмплов) с эталоном на 256 сэмплов, обучает итерации по очереди на CPU и перезаписывает файл весов. Нужна
трассировка лучей (path tracer); пользоваться готовыми весами можно на любом GPU. Значения `r.Denoiser`: `SVGF`,
`DLSSRR`, `Auto`, `Neural` (Ray Reconstruction при `Neural` не включается).

> **Экспериментально.** С поставляемыми весами нейроядро лучше SVGF на свежей истории (после разрыва), но мыльнее,
> когда временное накопление сошлось, и À-trous стоит втрое дороже (цифры — в шаге 6). Поэтому `Neural` доступен
> только через cvar (консоль, `ProjectSettings`), в меню настроек его нет; по умолчанию используйте `SVGF` или `Auto`.

## Шаг 6. Цифры

RTX 3080 (драйвер 617.42), i9-9900K, RelWithDebInfo, валидация выключена, 2026-10-14. Полные таблицы и методика —
[`docs/dev/perf.md`](../dev/perf.md), раздел «Neural rendering». Воспроизвести:

```sh
OX_NTC_PERF=1 OX_VULKAN_VALIDATION=0 ox_render_gpu_tests --gtest_filter=NeuralTexturesTest.PerfReportMaterialStation
OX_NEURAL_PERF=1 OX_VULKAN_VALIDATION=0 ox_render_gpu_tests --gtest_filter=NeuralGpuTest.InferenceThroughput:NeuralDenoiserTest.ComparedToSvgf
```

**Размер и качество** (пресет Medium, шесть материалов станции в духе станции 02 шоукейса: пять 512² и кожа 2048²):

| | BC7/BC5 (обычный импорт) | NTC Medium |
| --- | --- | --- |
| На диске, все 6 материалов | 20,6 МБ | 2,92 МБ (в 7,1 раза меньше; 3,75–3,95 бит/тексел) |
| В видеопамяти, InferenceOnSample | — | 2,92 МБ (латенты + веса + BC-хвост) |
| PSNR albedo (дБ) | 48,5–57,3 | 42,1–48,4 |
| PSNR нормалей (дБ) | 49,2–52,7 | 35,8–39,0 |
| PSNR шероховатости (дБ) | 53,8–55,2 | 37,6–46,3 |
| SSIM albedo | 0,993–1,000 | 0,971–0,993 |

NTC проигрывает BC7 по PSNR (BC7 — 8 бит/тексел на канал-группу, NTC — ~4 бита на весь материал), но на глаз
разница на станции не видна (скриншоты `ntc_station_*.png` в `%TEMP%/oxwald_render_out`). Больше всего теряют
нормали — на материалах, где важен мелкий рельеф, берите пресет High.

**Время GPU, станция материалов** (TAA, стохастическая фильтрация, путь MLP в шейдерах — Auto = cooperative vector;
в скобках — только depth prepass + forward):

| Режим | 1080p, мс | 4K, мс | Текстуры + латенты в VRAM (1080p / 4K) |
| --- | --- | --- | --- |
| Off (BC7/BC5) | 0,9 (0,26) | 3,4 (0,94) | 6,8 / 9,8 МБ |
| DecodeOnLoad | 0,8 (0,24) | 3,1 (0,94) | 7,0 / 10,0 МБ |
| InferenceOnSample | 3,1 (1,9) | 11,6 (7,2) | 3,0 / 4,7 МБ |
| InferenceOnSample + Bilinear | 7,7 (5,1) | 30,6 (20,2) | 3,0 / 4,7 МБ |
| InferenceOnSample, путь FP16 | 14,1 (9,4) | 55,2 (36,7) | 3,0 / 4,7 МБ |
| InferenceOnSample, путь Scalar (FP32) | 27,2 (22,7) | 100,3 (65,1) | 3,0 / 4,7 МБ |

VRAM меньше полного размера на диске, потому что стример мипов держит в памяти только нужные мипы. DecodeOnLoad
по скорости кадра не отличается от Off — это обычные BC-текстуры. InferenceOnSample стоит ~1,7 мс при 1080p и
~6,3 мс в 4K на этой станции (почти весь экран — нейроматериалы), зато экономит половину памяти текстур. Без
cooperative vector (пути FP16 / Scalar) режим InferenceOnSample непригоден — используйте DecodeOnLoad.

Шейдеры материалов с нейродекодером собираются только при первом включении InferenceOnSample; в режимах Off и
DecodeOnLoad рендерер использует варианты без него (`OX_NTC_DISABLED`). До этого разделения сам факт наличия
декодера в `forward.frag` замедлял обычные материалы: forward в 4K — 2,3 мс вместо 0,7 мс.

**Время загрузки (DecodeOnLoad на CPU, рабочие потоки):** 512² — 0,12–0,4 с на материал (сеть + BC-энкодер), кожа
2048² — 2,8–3,6 с (сеть 1,0 с + BC7 1,7–2,4 с); разброс — из-за параллельной нагрузки на CPU во время замеров. Декодирование на GPU (`NtcGpuDecoder`) всех нейро-мипов кожи —
1,2–1,3 мс через cooperative vector, 6,2–6,5 мс cooperative matrix, 10,3 мс FP16, 19–25 мс FP32 (без BC-сжатия).

**Пути MLP** (8,3 млн сэмплов, сеть 32 → 64 → 64 → 16): cooperative vector 4,7 мс, cooperative matrix 8,8 мс,
FP16 18,1 мс, FP32 30,2 мс. Для вычислительных шейдеров движок выбирает cooperative vector, при его отсутствии —
cooperative matrix (`bestComputePath`).

**Нейроденойзер** (сигнал 1-spp AO, эталон 256 сэмплов, 1080p):

| | Кадр 1 (свежая история) | Кадр 25 (история накоплена) | À-trous, 1080p |
| --- | --- | --- | --- |
| Без денойзера | 13,1 дБ | — | — |
| SVGF | 26,4 дБ | **29,6 дБ** | **2,3 мс** |
| Neural | **29,0 дБ** | 26,8 дБ | 6,7 мс |

Нейроядро лучше SVGF сразу после разрыва истории, но хуже после накопления и втрое дороже, поэтому
`r.Denoiser Neural` помечен как **экспериментальный** и в меню настроек не предлагается.

## Типичные ошибки и подводные камни

- **Шум на нейроматериалах.** Стохастической фильтрации нужен TAA. Без него — `r.NeuralTextures.Filter Bilinear`.
- **В режиме Off материал мыльный.** Материал импортирован с `Replace`: исходных текстур нет, показывается
  декодированный набор (пока он декодируется — fallback-хвост).
- **Отражения/тени с альфа-тестом грубее основного изображения.** Так задумано: всё, кроме forward и prepass, читает
  fallback-хвост (≤ 128 px по умолчанию). Увеличьте `neuralFallbackSize`, если это заметно.
- **Долгий импорт.** Обучение идёт на CPU. Для итераций по контенту ставьте `neuralPreset: Low` или `neuralSteps`
  поменьше, финальное качество — перед сборкой.
- **Альфа-тест.** Отсечение по альфе в prepass использует fallback-альфу; тонкие вырезы сжимайте с
  `neuralFallbackSize` побольше или не сжимайте такие материалы.
- **Материалы внутри моделей (glTF/FBX).** NTC включается только для `.oxmat`-материалов (или через API для своих
  `MaterialAsset`).

## API

| Заголовок | Что внутри |
| --- | --- |
| `<oxwald/assets/neural/mlp.hpp>` | `nn::Mlp`, `trainRegression`, `Adam`, `WorkerPool`, `Pcg32`, `packFp16`, FP16-конверсия |
| `<oxwald/assets/neural/ntc.hpp>` | `NeuralTextureSet`, `NeuralTextureSettings`, `MaterialImportSettings`, `encodeNeuralTextures`, `decodeNeuralMip`, `decodeToTextures`, `evaluateNeuralTextures`, `(de)serializeNeuralTextureSet` |
| `<oxwald/render/neural/neural.hpp>` | `NeuralPath`, `neuralPathSupported`, `NeuralNetwork`, `NeuralInference`, `NtcGpuDecoder`, `NeuralTextureMode`, `neuralTextureStats`, `addNeuralTextureSet`, `flushNeuralTextures`, `neuralDenoiserSelected` |
| `render/neural/mlp.glsl`, `mlp_coopmat.glsl` | слои MLP: FP32, FP16, cooperative vector, cooperative matrix |
| `render/neural/ntc.glsl` | `NtcMaterial`, `oxNtcDecode`, `oxSampleMaterialNtc`, `oxSampleMaterialView` |
| `render/neural/denoise_neural.comp`, `denoiser_weights.glsl` | À-trous с обученным ядром |

## Что дальше

- [19. Материалы](19-materials.md) — поля `MaterialAsset`, на которые раскладываются каналы NTC.
- [24. Трассировка лучей](24-ray-tracing.md) — денойзер SVGF и `r.Denoiser`.
- [26. Качество графики](26-quality-settings.md) — группа Textures.
- [27. GPU-driven рендеринг и производительность](27-gpu-driven-performance.md) — стриминг мипов.
- [31. Ассеты](31-assets.md) — импорт, `.meta`, `oximport`, `oxpack`.
- [Оглавление](README.md)
