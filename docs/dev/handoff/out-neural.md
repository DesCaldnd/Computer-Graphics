# Neural area: text for the coordinator to merge (2026-10-14)

## docs/dev/perf.md — new section (English, append after "Ray tracing")

```markdown
## Neural rendering (RTX 3080, 2026-10-14)

Validation off, RelWithDebInfo, driver 617.42, GPU lock held. Reproduce:
`OX_NTC_PERF=1 OX_VULKAN_VALIDATION=0 ox_render_gpu_tests --gtest_filter=NeuralTexturesTest.PerfReportMaterialStation`
(trains the sets once, cached in `%TEMP%/oxwald_ntc_cache`; `OX_NTC_PERF_PATHS=FP16,Auto` picks the material shader
paths) and `OX_NEURAL_PERF=1 OX_VULKAN_VALIDATION=0 ox_render_gpu_tests
--gtest_filter=NeuralGpuTest.InferenceThroughput:NeuralDenoiserTest.ComparedToSvgf`.

### Neural texture compression — size and quality (`PerfReportMaterialStation`, preset Medium)

Six materials of a station like showcase station 02 (generated 512² textures + a 2048² leather from the legacy 4K
set). "BC" = the regular importer output (BC7 sRGB albedo / BC7 ORM / BC5 normal, full mip chains, `.oxtex` bytes);
"NTC" = the `.oxntc` payload (latents + FP16 network + BC fallback tail ≤ 128 px), which is also the InferenceOnSample
VRAM. PSNR / SSIM against the source images; the BC column is the BC encoding of the same sources.

| material | size | BC bytes | NTC bytes | bits/texel | train s | PSNR albedo NTC / BC | normal NTC / BC | roughness NTC / BC | SSIM albedo NTC / BC |
|---|---|---|---|---|---|---|---|---|---|
| tiles | 512² | 1 048 656 | 194 960 | 3.95 | 5.3 | 45.7 / 52.7 | 39.0 / 52.7 | 42.2 / 53.8 | 0.986 / 0.997 |
| concrete | 512² | 699 104 | 173 088 | 3.95 | 5.1 | 44.4 / 54.1 | 37.5 / 52.3 | — | 0.977 / 0.998 |
| wood | 512² | 699 104 | 173 088 | 3.95 | 7.2 | 47.0 / 51.2 | — | 37.6 / 54.8 | 0.993 / 0.997 |
| leather | 2048² | 16 777 296 | 2 029 968 | 3.75 | 340 | 42.1 / 48.5 | 35.8 / 49.2 | 46.3 / 55.2 | 0.971 / 0.993 |
| rock | 512² | 699 104 | 173 088 | 3.95 | 13.7 | 48.4 / 52.9 | 38.1 / 52.3 | — | 0.988 / 0.996 |
| screen | 512² | 699 104 | 173 088 | 3.95 | 10.8 | 43.9 / 57.3 | — | — | 0.977 / 1.000 |
| **total** | | **20 622 368** | **2 917 280** (7.1x smaller) | | | | | | |

Load time of DecodeOnLoad (CPU, `decodeToTextures`: network + BC7/BC5 encode at `r.NeuralTextures.DecodeQuality 0`),
three runs while other processes loaded the CPU: 512² materials 118–716 ms (typically 120–180 ms: ~70 ms network +
~70 ms BC), leather 2048² 2.8–3.6 s (network 1.0–1.2 s + BC7 1.7–2.4 s). GPU decode of all neural mips of the leather
set (`NtcGpuDecoder`, no BC encode): CoopVector 1.2–1.3 ms, CoopMatrix 6.2–6.5 ms, FP16 10.3 ms, FP32 19–25 ms.

### Neural texture compression — GPU time (`PerfReportMaterialStation`)

Floor (tiles 4x4), wall (concrete), 18 objects; TAA, stochastic filtering unless noted; 24 frames per configuration,
the timing of the last one. "textures + latents" = `GpuResourceCache::textureBytes()` + latents + weights (the
texture streamer keeps only the wanted mips resident, hence less than the disk sizes).

| r.NeuralTextures | material shader path | 1080p frame ms | 1080p prepass + forward ms | 4K frame ms | 4K prepass + forward ms | textures + latents 1080p / 4K |
|---|---|---|---|---|---|---|
| Off (BC7 / BC5) | — | 0.89 | 0.26 | 3.38 | 0.94 | 6.8 / 9.8 MiB |
| DecodeOnLoad | — | 0.84 | 0.24 | 3.08 | 0.94 | 7.0 / 10.0 MiB |
| InferenceOnSample | Auto (CoopVector) | 3.12 | 1.91 | 11.62 | 7.24 | 3.0 / 4.7 MiB |
| InferenceOnSample, Bilinear | Auto (CoopVector) | 7.69 | 5.07 | 30.62 | 20.17 | 3.0 / 4.7 MiB |
| InferenceOnSample | FP16 | 14.13 | 9.36 | 55.24 | 36.72 | 3.0 / 4.7 MiB |
| InferenceOnSample | Scalar (FP32) | 27.22 | 22.74 | 100.31 | 65.13 | 3.0 / 4.7 MiB |

InferenceOnSample costs +1.7 ms at 1080p and +6.3 ms at 4K on this station (nearly every pixel is a neural
material) and halves texture memory; without cooperative vectors it is not usable (use DecodeOnLoad).

Before this measurement the material shaders always contained the decoder: with the cooperative-vector variant the
Off / DecodeOnLoad rows were 2.66 ms of prepass + forward at 4K (forward 2.26 ms) instead of 0.94 ms — on every
NVIDIA GPU with cooperative vectors, for every material, with neural textures not even in use. The renderer now keeps
a second pipeline set with the decoder (`prepassNeural` / `forwardNeural`), created on the first InferenceOnSample
frame (render_neural.md).

### MLP paths — `NeuralGpuTest.InferenceThroughput`

Batch inference, 32 inputs → H → H → 16 outputs, FP32 in/out buffers, average of 8 timed dispatches:

| samples | hidden | ScalarF32 | ScalarF16 | CoopMatrix | CoopVector |
|---|---|---|---|---|---|
| 2.07 M (1080p) | 32 | 2.22 ms | 2.01 ms | 1.54 ms | 0.97 ms |
| 8.29 M (4K) | 32 | 21.6 ms | 8.19 ms | 5.84 ms | 3.45 ms |
| 2.07 M (1080p) | 64 | 7.38 ms | 4.56 ms | 2.20 ms | 1.21 ms |
| 8.29 M (4K) | 64 | 30.2 ms | 18.1 ms | 8.79 ms | 4.67 ms |

The KHR cooperative-matrix kernel was 65.3 ms (8.3 M, hidden 64) before tuning: shared-memory rows of 64 halves put
all 16 rows of a tile in the same bank (16-way conflicts on every `coopMatLoad`/`coopMatStore`) and the weights were
staged per 256-sample workgroup (44 KB shared memory per workgroup → low occupancy). Padding the rows by 8 halves
(65 → 29 ms), loading the weight tiles straight from the buffer (L1/L2) and 1024 samples per workgroup (→ 8.8 ms).
It stays ~2x slower than the driver's cooperative-vector kernels, so `bestComputePath` now prefers CoopVector >
CoopMatrix > FP16 > FP32.

### Neural denoiser — `NeuralDenoiserTest.ComparedToSvgf`

1-spp stochastic AO (`denoise_test_noise.comp`, no RT needed) through `rt::SvgfDenoiser`, RMSE / PSNR against a
256-spp reference, 192², then the cost at 1080p (same temporal and variance passes for both kernels):

| | frame 1 (fresh history) | frame 25 (converged) | 1080p denoiser | of which À-trous (4 iterations) |
|---|---|---|---|---|
| noisy input | 0.2214 (13.1 dB) | — | — | — |
| SVGF | 0.0476 (26.4 dB) | **0.0329 (29.6 dB)** | **2.49 ms** | **2.28 ms** |
| Neural | **0.0355 (29.0 dB)** | 0.0457 (26.8 dB) | 6.91 ms | 6.70 ms |

Training (`NeuralDenoiserTest.TrainWeights`, ~30 s): 22 frames (path tracer 1/2/4/16 spp vs 1536 spp, AO 1/8 spp vs
256 spp); held-out path traced frames on the CPU replica (spatial only): noisy 31.9 dB, SVGF 40.1 dB, neural 40.5 dB.
The À-trous shader first ran at 35.7 ms: a runtime iteration index into the 1604-float weight array put it in local
memory; one generated function per iteration (constant indices → immediates) gives 7.2 → 6.7 ms. The neural kernel
is better than SVGF right after a history reset but blurrier once the history has converged and ~3x the À-trous cost:
`r.Denoiser Neural` is marked experimental and not offered in the settings menus.
```

## CHANGELOG.md — additions to the "[Unreleased] — нейронный рендеринг" section (Russian, like the file)

Replace the denoiser bullet and the "Замеры" bullet with:

```markdown
- Нейроденойзер (**экспериментальный**): `r.Denoiser Neural` — À-trous SVGF с ядром, предсказанным маленькой сетью;
  веса (`denoiser_weights.glsl`) обучены инструментом `NeuralDenoiserTest.TrainWeights` на path tracer'е движка
  (1/2/4/16 сэмплов) и AO (1/8). На AO лучше SVGF сразу после разрыва истории (29,0 против 26,4 дБ), но хуже после
  накопления (26,8 против 29,6 дБ) и втрое дороже (À-trous 6,7 против 2,3 мс в 1080p) — в меню настроек не
  предлагается.
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
```

## docs/guide/README.md

Nothing new (chapter 36 is already listed).
