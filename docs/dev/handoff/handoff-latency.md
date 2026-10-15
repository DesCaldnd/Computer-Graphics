# Handoff: Reflex (F) + FSR3 frame generation (G) — final state 2026-10-15 (~11:30)

Build dir: E:/Development/OxwaldEngine/build/latency (RelWithDebInfo, configured with
-Damd-fidelityfx-vk_DIR=...; the cache now points at `<repo>/vcpkg_installed/x64-windows/share/amd-fidelityfx-vk`).
Last build: latency_build17.log, 0 warnings, `ninja -t deps` "#deps 0" = 3.

## Status: done, except the open items below

Code (uncommitted, see `git status`):
- rhi: Reflex on `Swapchain` (+ `lowLatencyCapable()`), `ISwapchainProxy`, `VkLatencySubmissionPresentIdNV`,
  FG queues. `engine/rhi/tests/gpu/low_latency_tests.cpp`.
- runtime: `r.Reflex`, `r.Reflex.Fallback`, `LatencyLimiter`, `Engine::latencyBeginFrame`, `LatencyStats`
  (+ `reflexBehindFrameGeneration`). `engine/runtime/tests/latency_tests.cpp`.
- render: `runtime/latency_tracker.hpp`, `features/framegen/*` (FSR 3 proxy; pacing guard in `present()`; stats
  `generatedPresented/realPresented/pacingWaitMs/pacingTimeouts`). `tests/gpu/frame_generation_tests.cpp`
  (watchdog + diagnostics, cross-process named mutex).
- ui/player: Reflex-behind-FG reason in the settings menu / overlay / `--stats` (`reflex=framegen-proxy`).
- No temporary hacks are left (the FGX_* switches were already removed; my temporary OXFG_TMP_* experiments are removed
  too). `OX_DISABLE_REFLEX=1` (runtime_renderer.cpp) is a deliberate switch to measure the limiter on NVIDIA; it is
  documented in runtime.md.

Tests (2026-10-15, RTX 3080): LowLatency 3/3 (alone, under load, `ctest -j4` of all 77 rhi tests); runtime
Latency*/Limiter* 7/7 (also 8 copies in parallel); FrameGeneration 3/3 (20 serial runs, 6 next to other GPU suites,
5 next to windowed presenters, `ctest -R FrameGeneration -j 3` ×9 runs, `-R "FrameGeneration|Golden|Reflections" -j 6`
38/38); ox_ui_tests 13/13, ox_ui_gpu_tests 7 pass + 1 skip.

Why LowLatency/Latency failed in the old full runs: the 17:46 run had a half-built area. In the 20:42 "cpu" run
the limiter controller was still the old one, and the FG tests (also LowLatency) ran in the *parallel* part because
the `gpu` ctest label is lost (gtest_discover_tests splits "rhi\;gpu"; the coordinator is fixing the label
registration).

FG hangs found and handled today: (1) single process, pair drops → pacing guard in
`Fsr3FrameGenerator::present` (player 11/12 hangs → 0/12). (2) Several FSR 3 proxies at once (`ctest -j`) →
SDK/driver deadlock, not fixable on our side → FG tests serialise via a named mutex. Details:
docs/dev/modules/render_postprocess.md "Frame generation".

Reflex + FG: not possible with FidelityFX 1.1.4 Vulkan (the proxy swapchain is an SDK object, the real swapchain is
private, the presents come from the proxy's thread without present ids). `r.Reflex On` uses the engine limiter; this
is documented and reported in the stats and UI.

Stub path (no FFX package / OX_ENABLE_FSR3_FRAMEGEN=OFF / non-Windows): checked by reading the code, not built.
`OX_RENDER_HAS_FIDELITYFX` is undefined → `OX_FG_FFX 0`; availability() returns a reason, create() returns null, the
tests GTEST_SKIP.

Measurements: tables in docs/dev/handoff/out-latency.md (for perf.md). Scripts and logs are in the session
scratchpad (meas.py, reparse.py, logs/); they are not kept.

Docs updated: render_postprocess.md (FG: Reflex, stats, hangs, tests, timings), runtime.md (Latency: FG, limiter
numbers + JustInTime weakness), guide 25 (step 8, errors, API), guide 26 (player settings rows, numbers). The text
for perf.md, CHANGELOG and the guide README is in out-latency.md (I did not edit those files).

## Open
1. JustInTime limiter oversleeps when GPU bound (83 fps / 22.6 ms vs Strict 92 fps / 10.9 ms vs Reflex 101 fps /
   14 ms). Ideas: adapt the headroom when the GPU starves (negative gpuQueue), a high-resolution waitable timer for
   `Clock::sleepUntil`. Consider Strict as the default while frame generation is on (12.5 ms, hold 13 ms, −2 %
   rendered fps vs JustInTime 21.4 ms, −9 %).
2. Vsync without a frame cap: ~48 ms that Reflex does not remove. Could auto-cap below the refresh rate when
   `r.Reflex` is on with FIFO, e.g. through `LowLatencyMode::minimumIntervalUs` / `t.MaxFPS`.
3. The pacing guard is empirical (the mechanism is inferred from the SDK sources and the stacks). Re-check with a
   newer FidelityFX SDK if one ships with Vulkan again.
4. Observation for the runtime owner: with a reused `--user-dir`, the saved quality seemed to win over `--quality` on
   the first run (see out-latency.md).
5. The editor rows (Preferences / Project Settings: Low Latency, Latency Limiter, Frame Generation) exist in the code.
   I did not run them in the editor in this session.
