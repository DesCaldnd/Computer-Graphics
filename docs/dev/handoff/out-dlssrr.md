# DLSS Ray Reconstruction — additions for perf.md / CHANGELOG (for the coordinator to merge)

## docs/dev/perf.md — new subsection under "## NVIDIA RTX 3080 (2026-10)", after "### Ray tracing"

### DLSS Ray Reconstruction — `RayReconstructionTest.PerfReportRayReconstruction` (`OX_RT_PERF=1 OX_VULKAN_VALIDATION=0`)

Validation **off**, RelWithDebInfo, driver 617.42, one run on 2026-10-14 under the GPU lock. Scene of
`RayTracingTest.PerfReportRayTracing` (240 objects, mirror-like patch, glass, 8 shadowed point lights, sun, fog);
48 warm-up frames, then the average of 8 frames with one moving instance; each configuration starts with a fresh
view. "RT passes" are all `RT.*` / `Rt*` passes (under RR the SVGF stages are skipped, only the format resolve /
half-res upsample and `RT.RRGuides` remain); "upscaler pass" is `DLSS` or `DLSSRR`. RR runs preset E.

| Output | Configuration | Render | GPU frame ms | RT passes ms | of it denoise / resolve | RR guides | Upscaler pass ms |
|---|---|---|---|---|---|---|---|
| 1080p | SVGF, native, no AA | 1920×1080 | 9.28 | 8.16 | 5.21 | — | — |
| 1080p | SVGF + DLSS Quality | 1280×720 | 5.95 | 4.53 | 2.46 | — | 0.74 |
| 1080p | RR Quality | 1280×720 | 7.18 | 2.20 | 0.04 | 0.07 | 4.28 |
| 1080p | SVGF + DLSS Performance | 960×540 | 5.06 | 3.24 | 1.48 | — | 1.30 |
| 1080p | RR Performance | 960×540 | 4.70 | 1.77 | 0.02 | 0.04 | 2.43 |
| 1080p | SVGF + DLAA | 1920×1080 | 9.97 | 8.11 | 5.20 | — | 0.74 |
| 1080p | RR DLAA | 1920×1080 | 13.74 | 3.49 | 0.36 | 0.16 | 9.12 |
| 4K | SVGF, native, no AA | 3840×2160 | 33.37 | 29.85 | 20.70 | — | — |
| 4K | SVGF + DLSS Quality | 2560×1440 | 17.53 | 13.47 | 9.24 | — | 2.17 |
| 4K | RR Quality | 2560×1440 | 24.47 | 6.01 | 0.41 | 0.27 | 16.51 |
| 4K | SVGF + DLSS Performance | 1920×1080 | 14.34 | 8.32 | 5.30 | — | 4.78 |
| 4K | RR Performance | 1920×1080 | 13.52 | 3.24 | 0.07 | 0.16 | 9.05 |
| 4K | SVGF + DLAA | 3840×2160 | 35.17 | 29.32 | 21.26 | — | 2.25 |
| 4K | RR DLAA | 3840×2160 | 49.02 | 9.21 | 0.27 | 0.61 | 36.16 |

On the RTX 3080 Ray Reconstruction saves the SVGF stages but its network costs more than they do, except in
Performance mode (−0.36 ms at 1080p, −0.82 ms at 4K). It is a quality option on this GPU, not a speed-up. The
default preset (F) instead of E: RR pass 4.10 / 2.45 / 8.16 ms at 1080p Quality / Performance / DLAA and 14.60 /
9.32 / 31.24 ms at 4K (4–15 % cheaper, 0.2 dB further from the path tracer, more flicker at DLAA).

VRAM: the first RR frame raises the driver-reported usage by ~180–510 MB (varied between runs); NGX keeps it after
the RR feature is released until NGX shuts down with the device. Repeated RR ↔ SVGF toggling does not grow it
(`RuntimeToggleWithoutLeaks`).

Image quality (`ImageQualityAndStabilityVersusSvgfPlusDlss`, 640×360, half-resolution PSNR):

| Configuration | vs path tracer (4096 spp) | vs converged hybrid | flicker |
|---|---|---|---|
| SVGF + DLSS Quality | 25.81 dB | 34.54 dB | 0.037 |
| RR Quality | 25.32 dB | 37.82 dB | 0.089 |
| SVGF + DLAA | 25.67 dB | 37.12 dB | 0.031 |
| RR DLAA | 25.32 dB | 38.19 dB | 0.045 |

Why RR is lower against the path tracer but higher against the converged hybrid frame: see
`modules/render_postprocess.md`, "DLSS Ray Reconstruction".

## CHANGELOG.md (Russian, matching the file) — section for the RR area

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
