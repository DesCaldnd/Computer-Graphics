# Final verification — dev / debug / release (2026-10-15)

Windows 11, MSVC 2022 (Russian resources), RTX 3080 (driver 617.42), i9-9900K, 32 GB. Branch `oxwald-engine`,
uncommitted tree. Builds and configures only from Git Bash through the vcvars wrapper. Configure options as in
`build/fixes`: `-DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_TARGET_TRIPLET=x64-windows
-Damd-fidelityfx-vk_DIR=E:/Development/vcpkg_ffx/installed/x64-windows/share/amd-fidelityfx-vk`, plus
`-DOX_DOCS_PYTHON=E:/Development/OxwaldEngine/build/docs_venv/Scripts/python.exe`. Without that option the system
Python has no `markdown`, and `docs_pdf.pytest` is not registered at all (configure prints "docs_pdf: ... skipped").
GPU work ran under `E:\Development\OxwaldEngine\gpu-lock.d`. Logs: `E:\Development\OxwaldEngine\build\verify_*.log`.

## 1. dev (RelWithDebInfo, /WX)

- `cmake --preset dev --fresh -B E:/Development/OxwaldEngine/build/dev …`: 19 s.
- `cmake --build … --clean-first -j 8`: **784 steps, 1302 s (21 min 42 s), 0 errors, 0 warnings**. Ninja deps
  `#deps 0` = 3 (the qrc file and two mocs_compilation files, as expected).
- An incremental rebuild after the ctest label fix and the debug_tools fix (19 steps, mostly links) took 30 min,
  because 8 RelWithDebInfo test executables link in parallel and write their PDBs to E:.

## 2. dev ctest

The first run used name lists (CPU executables at `-j 6`, GPU executables serially at `-j 1`). After the
coordinator's label fix (`ox_gtest_labels`), the final run used labels:

| part | command | tests | passed | failed | skipped | time |
|---|---|---|---|---|---|---|
| CPU | `ctest -LE "gpu\|android" -j 6` | 1139 | 1137 | 0 | 2 | 90 s |
| GPU | `ctest -L gpu -j 1` (lock) | 281 | 275 | 0 | 6 | 543 s |
| Android | `Android.DeviceSmoke` | 1 | – | 0 | 1 (77, no device / APK) | 1 s |

1420 tests in total, **0 failures**. There were no failures to re-run. One run accidentally ran all 1420 tests at
`-j 6` without the GPU lock (cmd.exe swallowed the `|` in the label regex). Every test passed there too, so GPU
contention does not cause failures at the moment.

This covers the editor tests (6 classes, including GizmoTests, SettingsTests and ViewportPacingTests),
`ox_showcase_tests`, `docs_pdf.pytest` (10 pytest, passed) and `Showcase.GeneratorCheck`
(`oxshowcase_generate --check`: **green**, no difference in `OxwaldShowcase.oxproj`; the file was not touched).

Skips, all explained:
- Env-gated tools and benchmarks: `NeuralTextures.Report` (`OX_NTC_REPORT`), `NeuralDenoiserTest.TrainWeights`
  (`OX_TRAIN_NEURAL_DENOISER`), `NeuralTexturesTest.PerfReportMaterialStation` (`OX_NTC_PERF`),
  `RenderTest.VrsPipelineStateOverhead` and `ShowcaseVrs.StationsQualityAndPerformance` (`OX_VRS_PERF`).
- Hardware-gated: `RayTracingTest.RayTracingCheckboxIsInertWithoutSupport` (checks devices *without* RT),
  `DeviceLifecycle.SurfaceDeviceAfterHeadlessDevice` (no `VK_EXT_headless_surface` on NVIDIA),
  `UiEngine.WindowedScreenshot` (no presentable surface in the headless test), `Android.DeviceSmoke` (77).

Labelling gap for the coordinator: the `ox_showcase_tests` cases (`ShowcaseScripts.AllCompile`,
`ShowcaseSaves.*`, `ShowcaseDestruction.*`, `ShowcaseVrs.*`) carry only `showcase` and create a Vulkan device. They
land in the CPU part. They passed there, but they are not serialized. `Showcase.GeneratorCheck` (`showcase` only)
also appears in `-L gpu`, because it is the `showcase_content` fixture setup of the `Showcase.Scene.*` tests.

## 3. debug (Debug, /WX, MSVC debug iterators)

- Reconfigure + `cmake --build … -j 8`: 556 steps, 1201 s, 0 errors, 0 warnings, deps 0 = 3.
- CPU part (`-LE "gpu|android" -j 6`, after the label fix): 1139 tests, **1136 passed, 1 failed, 2 skipped**, 159 s.
- GPU part (all 279 GPU-executable tests serially, before the label fix): **279/279 passed, 8 skipped**, 1610 s
  (27 min). The 9 tests that the label fix newly put under `gpu` (`GuideRhiDevice.*`, `GuideRuntimeRenderer.*`,
  `GuideUiEngine.*`, `AutoDetectGpu.BenchmarkAndApply`) were also run serially afterwards: 9/9 passed. Debug skip on
  top of the dev skips: `GpuDrivenTest.SteadyStateExtractAndDrawListsDoNotAllocate` (allocation count is skipped
  under MSVC debug iterators, commit 66c123c).
- So the whole GPU set ran in Debug.

Failures and what happened to them:
1. **`UiDebug.AllToolWindowsDrawWithoutEngine` and `GuideUiImGui.DebugToolsAndConsole` — real bug, fixed.**
   Dear ImGui 1.92.7 asserts `(flags & ImGuiInputTextFlags_EnterReturnsTrue) == 0` in `InputScalar`.
   `engine/ui/src/debug_tools.cpp` (`cvarEditor`, int cvars without enum names) called
   `ImGui::InputInt(id, &i, 1, 100, ImGuiInputTextFlags_EnterReturnsTrue)`. The assert aborted every Debug run of the
   Settings tool window. In Release the flag was silently unsupported. Fix: int cvars are edited with `InputText`
   (`EnterReturnsTrue | CharsDecimal`) and committed through `setCVar` on Enter, the same as string cvars. The +/-
   step buttons are gone. The two existing tests are the regression tests (they abort without the fix in Debug).
   Both pass in Debug, and the dev label run passed after the fix.
2. **`editor.ViewportPacingTests` (`viewportTickReachesFrameLimitWhileEngineTicks`) — fails only in Debug, also in
   isolation** (37.6 ticks/s in the parallel run; 40.7, 40.3 and 33.3 ticks/s alone; at least 60 required; dev:
   120.0/s). This is not a pacing or watcher regression. Temporary timing (reverted) showed engine tick ≈ 0.28 ms,
   but the offscreen software viewport paint (`ViewportCanvas::paintEvent` → `paintCanvas`, QPainter renderer, Qt
   debug DLLs) ≈ **25 ms per frame** for 320×180 in Debug. One paint per event-loop pass caps both the viewport and
   the engine timer at ~40/s. Not fixed: it needs a decision (for example, skip the throughput assertion in Debug
   builds, or make the software canvas cheaper in Debug). Tolerances were not touched.

## 4. release

- `cmake --preset release -B E:/Development/OxwaldEngine/build/release …` + `cmake --build … -j 8`: 397 steps,
  368 s, 0 errors, 0 warnings (the release preset builds without tests).
- Smoke: `OxwaldPlayer --project samples/OxwaldShowcase --headless --frames 300` → `frames=300 fps=492.1
  renderer=Vulkan` (default scene). With `--scene project://Assets/Scenes/Hub.oxscene` → `frames=300 fps=407.9`,
  0 warnings or errors in the log.
- `GoldenTest.*` in Release: the release preset has `OX_BUILD_TESTS=OFF`, so the deployable dir was left untouched.
  A separate `E:/Development/OxwaldEngine/build/release_tests` (release preset + `-DOX_BUILD_TESTS=ON`, only target
  `ox_render_gpu_tests`, 261 s) ran all 10 `GoldenTest.*` and 3 `Golden/WorldSkyTest.*`: **13/13 passed**.

### Editor fps, Release, Showcase default scene (Hub), default layout (viewport 4728×918)

`OxwaldEditor --project <Showcase> --smoke-seconds 30`, monitor 5120×1440 @ 100 Hz. Preferences came from
`OX_EDITOR_PREFS_DIR`, a scratch dir, so the user's own preferences were not touched. `throttleWhenUnfocused=false`
was set because a window started from the shell is not necessarily focused; this is the same as a focused window. The
project was a byte copy of `samples/OxwaldShowcase` on E:, so the repo was not touched.

| mode | fps | frame interval | CPU ms | GPU ms |
|---|---|---|---|---|
| `matchDisplayRefresh` on (default), continuous redraw (`OX_EDITOR_VIEWPORT_STATS=1`) | **91–94** (smoke 93.1) | 10.6–11.0 ms | 0.70–0.75 | 6.0–6.3 |
| `matchDisplayRefresh` off, `frameLimit 0` (uncapped), continuous redraw | **138–143** (smoke 143.1) | 7.0–7.3 ms | 0.70–0.76 | 6.5–6.8 (GPU-bound, slot wait 5.3) |
| default prefs, normal redraw-on-change (what the status bar shows) | **59.6** | – | 0.94 | – |

- With the default the viewport reaches 92–94 fps, not 100. The target is 100 (10 ms period), but the measured mean
  interval is 10.75 ms, so about 7 % of ticks are lost. Likely cause: `FramePacer::next` restarts its schedule
  whenever a tick is more than one period late. Every event-loop stall over ~10 ms (the 16 ms engine edit tick plus
  other events on the UI thread) costs a tick instead of being caught up. The old 112–117 fps at limit 120 in
  `perf.md` shows the same ~6 % loss. Not changed. Options: allow catch-up within about 2 periods; or, with
  `matchDisplayRefresh`, present FIFO and let vsync pace the viewport.
- In normal use (no stats env), the viewport redraws only when something changes. In edit mode the engine's 16 ms
  tick marks it dirty (`RuntimeHost::ticked` → redraw), so the status bar shows ~60 fps regardless of the 100 Hz
  target. That is the engine tick rate, not a render limit (frame CPU 0.9 ms). If the user expects 100 fps in the
  status bar, the edit tick (16 ms) would have to follow the display rate too.

## 5. Deployable Release output

`E:\Development\OxwaldEngine\build\release\bin` (also reachable as `C:\Programming\OxwaldEngine\build\release\bin`
through the junction): 228 MB, 50 entries. It contains `OxwaldEditor.exe` (20 MB), `OxwaldPlayer.exe` (19 MB),
`oxdump`, `oximport`, `oxpack`, `oxshowcase_generate`, `rhi_window_smoke`, Qt 6 DLLs and plugins (`platforms/`,
`styles/`, `imageformats/`, `iconengines/`), `vulkan/`, DLSS (`nvngx_dlss*.dll`) and FSR 3 (`amd_fidelityfx_vk.dll`)
runtimes, and the vcpkg DLLs. Player (headless, 300 frames) and editor (windowed, Vulkan viewport, 30 s) both run.
Nothing was copied into the repo.

## Changed files

- `engine/ui/src/debug_tools.cpp`: int cvar editor fix (above). It is the only source change of this verification.
  The temporary timing code in `viewport_panel.cpp` / `runtime_host.cpp` was restored from byte-identical backups.

## Round 2 (after coordinator fixes)

Same machine, procedure and rules as above. Logs: `E:\Development\OxwaldEngine\build\r2_*.log` / `r2_*.txt`. The
E: copy of the Showcase project (`E:\Development\OxwaldEngine\verify_showcase`) was gone and was re-created as a
byte copy of `samples/OxwaldShowcase`. The scratch preferences in `E:\Development\OxwaldEngine\verify_prefs\{match,uncapped}`
were reused unchanged.

### Included changes

- Coordinator: `FramePacer` catch-up (`kCatchUpPeriods = 2`) + `pacerCatchesUpAfterShortHiccup`, Debug QSKIP of
  the two measured tick-rate tests, `ox_showcase_tests` labelled `showcase gpu`.
- Android agent: `runtime_renderer.cpp` (shutdown completes unpicked screenshot requests with `done(false)`) and
  `UiEngine.ScreenshotRequestedAfterTheLastFrameFailsAtShutdown`. Both are compiled into the final dev, debug and
  release builds (ninja rebuilt `runtime_renderer.cpp` and `ui_gpu_tests.cpp`).
- This round (each with a test):
  1. **`oxpack` deleted `Assets/AI/guard.oxbt.meta` in the repo.** `Showcase.GeneratorCheck` failed with
     "differs: AI/guard.oxbt.meta". Cause: the Android build ran `oxpack` on `samples/OxwaldShowcase` (14:24:53,
     the same second as `build/android/content/OxwaldShowcase.oxpak`). `oxpack` links only `Oxwald::assets` and
     does not register the gameplay importers (`.oxbt`, `.oxanimctrl`). The registry therefore saw no source for
     `guard.oxbt.meta`, treated it as an orphan and deleted it (`deleteOrphanMetas`). Reproduced with both the dev
     and the release `oxpack` on a scratch copy. That loses the asset's UUID: the editor would give `guard.oxbt` a
     new one and the AI station's reference breaks.
     Fix in `engine/assets/src/asset_registry.cpp`: a meta whose source file still exists is not an orphan, even when
     this registry has no importer for it. Test `AssetRegistry.MetaOfSourceWithoutImporterIsKept` (an `.oxbt` meta
     is kept, a real orphan meta is still deleted). After the fix, the dev and release `oxpack` keep the meta. The
     deleted meta was written back byte-identical to HEAD (`git show HEAD:path > path`), so the file has no diff.
  2. **The Debug build of `ox_editor_tests` did not compile:** C4702 (unreachable code) under `/WX`, because
     the unconditional `QSKIP` in `OX_SKIP_TICK_RATE_IN_DEBUG()` returns before the rest of the test. The macro now
     skips through a non-constant `inline volatile bool g_skipTickRates = true` (`editor/tests/test_viewport_pacing.cpp`).
     Release and dev expand to `(void)0` as before.
  3. **The editor viewport still ran at ~93 fps at 100 Hz after the pacer fix. Root cause: `StatsPanel`.** A
     temporary probe (event timing in `QApplication::notify` plus late viewport ticks; removed again, and the files
     were restored from byte-identical backups) showed 2 stalls per second of 30-45 ms, exactly every 500 ms. Each
     was `StatsPanel::refresh` -> `fillTable`, which deleted and re-created every row (~75 GPU passes: 150 `QLabel` +
     75 `QProgressBar`) each time. That meant ~700 polish, deferred-delete and meta-call events (~40 ms) on the UI
     thread. A stall over 2 periods restarts the pacer, so each one cost 3-4 ticks. The catch-up pacer could not
     help, because the stalls were longer than its 2-period window.
     Fix (`editor/src/panels/stats_panel.{hpp,cpp}`): `fillTable` (now a public static) updates rows in place when
     the row count is unchanged and rebuilds only when the count changes. Test
     `ViewportPacingTests::statsTableUpdatesRowsInPlace` (same widgets after a refresh, new texts and values; a
     different count rebuilds; no rows shows the placeholder).
  4. `UiEngine.ScreenshotRequestedAfterTheLastFrameFailsAtShutdown` used the headless-surface provider, so it
     skipped on NVIDIA (no `VK_EXT_headless_surface`) and never ran here. It now opens a real GLFW window, the same
     way as `UiEngine.WindowedScreenshotsWhileRenderingOnTheRenderThread`. **Passes** (3 of 3 runs, plus the
     final run).

### dev (RelWithDebInfo)

- Builds: 0 errors, 0 warnings, ninja `#deps 0` = 3 after every build. The final `cmake --build` reported "no work
  to do". One build was cut off by the agent's 30 min background limit while linking and was resumed.
- `ctest -L editor`: 6/6 passed. `ViewportPacingTests` (dev): 10 test functions passed, 0 skipped (viewport
  119.8 ticks/s at limit 120, engine 62.6/s, 60 Hz screen 59.3/s).
- `ctest -L showcase -j 1` (lock): 23 tests, 22 passed, 1 skipped (`ShowcaseVrs.StationsQualityAndPerformance`,
  env-gated), 0 failed, 90 s.
- `ctest -N`: **1422 tests**; `-L gpu` 286, `-LE "gpu|android"` 1136, `-L android` 1 (`Showcase.GeneratorCheck`
  is in both the CPU and the GPU sets as the fixture setup of `Showcase.Scene.*`).

**Final run with all changes:**

| part | command | tests | passed | skipped | failed | time |
|---|---|---|---|---|---|---|
| CPU | `ctest -LE "gpu\|android" -j 6` | 1136 | 1135 | 1 | 0 | 82 s |
| GPU | `ctest -L gpu -j 1` (lock) | 286 | 279 | 7 | 0 | 549 s |
| Android | `ctest -L android` | 1 | 0 | 1 (no device) | 0 | 2 s |

For the README: **1422 tests, 1413 passed, 9 skipped, 0 failed**. (Distinct tests: `Showcase.GeneratorCheck` ran
in both the CPU and the GPU parts and is counted once.) Skips: `NeuralTextures.Report`,
`NeuralDenoiserTest.TrainWeights`, `NeuralTexturesTest.PerfReportMaterialStation`, `RenderTest.VrsPipelineStateOverhead`,
`ShowcaseVrs.StationsQualityAndPerformance` (env-gated); `RayTracingTest.RayTracingCheckboxIsInertWithoutSupport`,
`DeviceLifecycle.SurfaceDeviceAfterHeadlessDevice`, `UiEngine.WindowedScreenshot` (hardware: no headless surface on
NVIDIA); `Android.DeviceSmoke` (no device).

Screenshot tests (final GPU run): `UiEngine.WindowedScreenshot` skipped (headless surface, as before),
`UiEngine.ScreenshotRequestedAfterTheLastFrameFailsAtShutdown` **passed**,
`UiEngine.WindowedScreenshotsWhileRenderingOnTheRenderThread` **passed**.

Flake observed once: in an intermediate CPU run, `editor.ViewportPacingTests` failed
(`viewportTickReachesFrameLimitWhileEngineTicks`: 20.0 viewport and 11.3 engine ticks/s, 24.6 s instead of ~8 s). It
ran in the first batch, together with the shader compiler tests, `OxwaldPlayer.HeadlessSmoke`, `docs_pdf` and so on.
The event loop itself was starved: the engine timer also dropped to 11/s, so this is not the pacer. It could not be
reproduced: 3 runs alone, 2 runs with the shader-compile tests, 2 runs with the 19 tests of that first batch at
`-j 6`, and 2 more full CPU runs all passed at 118.7-120.4/s. Tolerances were not changed. Keep an eye on it on
loaded machines.

### debug (Debug)

- `cmake --build ... --target ox_editor_tests -j 4`: 0 errors, 0 warnings after fix 2 (before it: C4702),
  deps 0 = 3.
- `ctest -L editor`: **6/6 passed**. `editor.ViewportPacingTests`: 8 passed, **2 skipped**
  (`viewportTickReachesFrameLimitWhileEngineTicks`, `viewportTicksAtItsScreenRefreshRate`), 0 failed.

### release

- Full rebuild `cmake --build E:/Development/OxwaldEngine/build/release -j 6`: 0 errors, 0 warnings, deps 0 = 2
  (qrc + one mocs file; the release preset has no test targets).

Editor fps, measured exactly as in section 4 (scratch prefs, project copy on E:, `--smoke-seconds 30`,
`throttleWhenUnfocused=false`, `OX_EDITOR_VIEWPORT_STATS=1`, Showcase Hub, viewport 4728x918, 100 Hz):

| mode | round 1 | after pacer fix only | after StatsPanel fix |
|---|---|---|---|
| `matchDisplayRefresh` on, continuous redraw | 91-94 (smoke 93.1) | 92.5-93.6 (smoke 94.0) | **99.3-100.7 (smoke 100.0)**, interval 9.94-10.06 ms, CPU 0.68 ms, GPU 6.5 ms |
| off, `frameLimit 0` | 138-143 (smoke 143.1) | 142-144 (smoke 142.3) | **138-153.5 (smoke 152.5)**, GPU-bound (slot wait 5.4 ms; 110-140 while warming up) |

The probe also showed a mean timer lateness of ~1.3 ms. The catch-up pacer absorbs it, and it costs no ticks. A
remaining small cost: each log line costs ~2.5 ms on the UI thread (console panel). This only matters when
something logs per frame.

Player smoke (final Release binaries, lock): `OxwaldPlayer --project samples/OxwaldShowcase --headless --frames 300`
-> `frames=300 fps=814.9 renderer=Vulkan`; with `--scene project://Assets/Scenes/Hub.oxscene` -> `frames=300
fps=618.9`; 0 warnings and 0 errors in both logs.

### Open points for the coordinator

- `oxpack` still does not pack `.oxbt` / `.oxanimctrl`, because it has no gameplay importers. **The Showcase pak,
  including the Android build's `OxwaldShowcase.oxpak`, lacks `AI/guard.oxbt`.** The AI station's behaviour tree
  is therefore missing in packaged builds. Options: link `oxpack` with `ox_gameplay` and call
  `gameplay::registerGameplayImporters` (this pulls in physics, script and the rest of gameplay), or move the two
  JSON-blob importers into a light library. Not changed here.
- `tools/android/build.ps1` prefers `E:\Development\OxwaldEngine\build\android-hostcheck\bin\oxpack.exe` (built
  2026-10-14), which still has the meta-deleting registry. Rebuild `android-hostcheck` (or use dev/release
  `oxpack`) before the next Android pack, or `guard.oxbt.meta` is deleted from the repo again.
- The docs that quote 91-94 fps at 100 Hz or the old pacing explanation (`perf.md`, `editor.md`, guide `30-editor`,
  if they do) should be updated to "100 fps at 100 Hz; uncapped ~150 (GPU-bound)".

### Changed files (round 2)

- `engine/assets/src/asset_registry.cpp`, `engine/assets/tests/registry_tests.cpp` (fix 1)
- `samples/OxwaldShowcase/Assets/AI/guard.oxbt.meta` (written back byte-identical to HEAD; no diff)
- `editor/tests/test_viewport_pacing.cpp` (fix 2, test for fix 3)
- `editor/src/panels/stats_panel.hpp`, `editor/src/panels/stats_panel.cpp` (fix 3)
- `engine/ui/tests/gpu/ui_gpu_tests.cpp` (point 4)
- The temporary probe in `editor/src/app/main.cpp` and `editor/src/viewport/viewport_panel.cpp` was removed. Both
  files were restored from backups and are byte-identical to their state before the probe.
