# Latency area (Reflex + FSR 3 frame generation): text to merge

Written by the latency agent on 2026-10-15. Three blocks, each ready to paste:
1. `docs/dev/perf.md`: a new section (English), after "Frame pacing: editor viewport and windowed player".
2. `CHANGELOG.md`: replaces the placeholder `<!-- latency: merged later -->` under
   "### Низкая задержка (Reflex) и генерация кадров FSR 3".
3. `docs/guide/README.md`: the chapter 25 row (the chapter got step 8, "frame generation and low latency"). The
   chapter 26 row does not change.

---

## 1. `docs/dev/perf.md`

```markdown
## Latency: NVIDIA Reflex, engine limiter, FSR 3 frame generation (RTX 3080, 2026-10-15)

Windowed `OxwaldPlayer` (RelWithDebInfo, validation off) on the Showcase, RTX 3080 (driver 617.42), i9-9900K, monitor
5120×1440 at 100 Hz. Window `--width 2560 --height 1440` (client area / swapchain 2560×1421). Each row is 3 runs with
a fresh user dir. Per run: the median of the 1-second `--stats` windows, skipping the first three (loading,
pipeline compiles). The table shows the median of the 3 runs; the spread between runs was ≤ 3 % unless noted.
**Latency here is not end-to-end (click to photon).** There is no hardware latency analyzer. Two measures are used:
- *Engine latency* (`LatencyStats::engineLatencyMs`): simulation start → GPU finished the frame. CPU timestamps: the
  game thread's `SimulationStart` marker and a thread waiting on the graphics timeline. Works for any vendor and also
  through the frame generation proxy.
- *PC latency*: NVIDIA's simulation start → GPU render end, from `vkGetLatencyTimingsNV` (`VK_NV_low_latency2`). It
  is reported only while Reflex owns the swapchain. Both measures agree within 0.2 ms wherever both exist.

Neither measure includes scan-out or display time. With frame generation, the rendered frame is also held by the
proxy for *hold* = `RenderStats::frameGeneration.addedLatencyMs`: from our present call until the proxy composes the
frame for its own present, after the generated frame in front of it. Fallback-limiter rows use `OX_DISABLE_REFLEX=1`
(device created without `VK_NV_low_latency2`, so `r.Reflex On` runs the engine limiter on NVIDIA hardware).

GPU bound — station 4 (Volumetrics), `--quality ultra`, `r.VSync 0` (mailbox):

| Mode | fps | GPU ms | engine latency ms | PC latency ms | latency sleep ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| `r.Reflex Off` | 104.1 | 9.53 | 46.9 | 46.9 | 0 |
| `r.Reflex On` | 101.2 | 9.50 | 14.0 | 14.0 | 9.4 |
| `r.Reflex OnBoost` | 102.0 | 9.42 | 13.8 | 13.7 | 9.3 |
| limiter `JustInTime` (no Reflex) | 82.8 | 9.13 | 22.6 | — | 11.6 |
| limiter `Strict` (no Reflex) | 91.7 | 9.28 | 10.9 | — | 10.4 |

CPU bound — station 7 (Physics), `--quality low`, 960×540 window, `r.VSync 0`:

| Mode | fps | GPU ms | engine latency ms | PC latency ms |
| --- | ---: | ---: | ---: | ---: |
| `r.Reflex Off` | 1516 | 0.31 | 2.05 | 2.01 |
| `r.Reflex On` | 1511 | 0.31 | 2.04 | 2.00 |
| `r.Reflex OnBoost` | 1521 | 0.31 | 2.03 | 1.99 |
| limiter `JustInTime` (no Reflex) | 1524 | 0.31 | 1.44 | — |
| limiter `Strict` (no Reflex) | 832 | 0.34 | 1.21 | — |

FSR 3 frame generation (`r.FrameGeneration FSR3`). Station 4 as above. Reflex cannot run through the proxy, so with
`r.Reflex On` the engine limiter runs:

| Mode | rendered fps | presented fps | ratio | GPU ms | engine latency ms | hold ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| frame generation off, `r.Reflex Off` (from the first table) | 104.1 | 104.1 | 1 | 9.53 | 46.9 | — |
| frame generation off, `r.Reflex On` | 101.2 | 101.2 | 1 | 9.50 | 14.0 | — |
| FSR 3, `r.Reflex Off` | 81.5 | 162.7 | 2.00 | 10.25 | 45.8 | 24.0 (22.7–28.4) |
| FSR 3, `r.Reflex On` → limiter `JustInTime` | 73.8 | 147.1 | 2.00 | 9.93 | 21.4 | 23.2 |
| FSR 3, `r.Reflex On`, `r.Reflex.Fallback Strict` | 79.6 | 158.7 | 2.00 | 10.33 | 12.5 | 13.3 |

Vsync (FIFO, 100 Hz) — station 1 (Lighting), quality High (GPU 4.2 ms):

| Mode | fps (presented) | engine latency ms | PC latency ms | hold ms |
| --- | ---: | ---: | ---: | ---: |
| `r.Reflex Off` | 100.0 | 48.6 | 48.4 | — |
| `r.Reflex On` | 100.0 | 46.5 | 46.4 | — |
| `r.Reflex Off`, `t.MaxFPS 97` | 97.0 | 6.4 | 6.3 | — |
| `r.Reflex On`, `t.MaxFPS 97` | 97.0 | 6.4 | 6.2 | — |
| FSR 3, `r.Reflex Off` | 50 / 99.9 | 47.3 | — | 20.8 |
| FSR 3, `r.Reflex On` (JustInTime) | 50 / 99.9 | 25.2 | — | 20.8 |
| FSR 3, `r.Reflex On`, `t.MaxFPS 49` | 49 / 97.8 | 7.9 | — | 9.0 |

Reading:
- **Reflex**, GPU bound: engine latency 46.9 → 14.0 ms (−70 %) for −3 % fps. The ~3.5 frames queued behind the GPU
  shrink to about one. Boost changes nothing measurable on a desktop RTX 3080, which does not downclock under this
  load. CPU bound: nothing to remove (2 ms, no queue).
- **Engine limiter**, used without Reflex (AMD/Intel/Apple/Android and through the FSR 3 proxy). *Strict* beats
  *JustInTime* when GPU bound: 10.9 ms at 92 fps vs 22.6 ms at 83 fps. JustInTime oversleeps there: the GPU idles
  ~3 ms per frame, because Windows timer wake-ups and the per-frame CPU floor exceed its 1 ms headroom. When CPU bound,
  JustInTime is free (1.44 ms at full fps) and Strict halves the frame rate. JustInTime stays the default; tightening
  it is an open item (`docs/dev/modules/runtime.md` "Latency").
- **Vsync** without a frame cap: the queue sits in the presentation engine, and neither Reflex nor anything before
  the GPU removes it (48.6 → 46.5 ms). A cap just under the refresh rate (`t.MaxFPS 97` at 100 Hz) takes latency to
  6.4 ms with or without Reflex.
- **Frame generation** doubles the presented rate exactly (ratio 2.00 in every run). The rendered rate drops 104 →
  82 fps: our frame gets +0.7 ms GPU for the HUD-less copy and the prepare dispatch, and the rest is the proxy's
  interpolation and pacing on the same GPU. It adds the proxy hold: 24 ms here, about two rendered frames. The
  limiter is essential with it. JustInTime gives 45.8 → 21.4 ms engine latency for −9 % rendered fps. Strict gives
  12.5 ms and also shortens the hold to 13 ms, for −2 %. Rough simulation-to-composition totals: 70 ms (FSR 3 alone),
  45 ms (+ JustInTime), 26 ms (+ Strict), against 14 ms for Reflex without frame generation.

Reproduce (Git Bash; set the validation layers off). Run in `E:/Development/OxwaldEngine/build/<dir>/bin`:

    OX_VULKAN_VALIDATION=0 ./OxwaldPlayer.exe --project <repo>/samples/OxwaldShowcase \
      --scene project://Assets/Scenes/Stations/04_Volumetrics.oxscene --windowed --width 2560 --height 1440 \
      --quality ultra --stats --frames 1500 --user-dir <empty dir> --cvar r.VSync=0 --cvar r.Reflex=On
    # limiter rows: prefix OX_DISABLE_REFLEX=1, add --cvar r.Reflex.Fallback=JustInTime|Strict
    # frame generation: add --cvar r.FrameGeneration=FSR3 (needs amd_fidelityfx_vk.dll next to the exe)
    # CPU bound: --scene .../07_Physics.oxscene --width 960 --height 540 --quality low --frames 8000
    # vsync: --scene .../01_Lighting.oxscene --cvar r.VSync=1 [--cvar t.MaxFPS=97]

Use a fresh `--user-dir` per run. With a reused one, the first run of a new configuration once ran at the previous
configuration's quality: station 4 Ultra showed 2.97 ms GPU instead of 9.5 ms. That looks like saved user settings
winning over `--quality`. The runtime owner should check this.
```

---

## 2. `CHANGELOG.md` (replaces `<!-- latency: merged later -->`)

```markdown
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
```

---

## 3. `docs/guide/README.md` (row of chapter 25)

```markdown
| [25. Апскейлеры и постобработка](25-upscalers-postprocess.md) | TAA/FXAA, FSR 1, TAAU, DLSS и DLAA (требования, режимы, рантайм NGX), экспозиция, bloom, DoF, motion blur, LUT, эффекты линз, `PostProcessVolume`, генерация кадров FSR 3 и низкая задержка (Reflex, ограничитель движка) |
```
