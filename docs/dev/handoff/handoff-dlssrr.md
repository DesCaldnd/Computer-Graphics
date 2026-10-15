# DLSS Ray Reconstruction — handoff (2026-10-14, evening)

## State: done (uncommitted)
All RR tests pass in E:/Development/OxwaldEngine/build/dlssrr (RelWithDebInfo, RTX 3080, driver 617.42):
`RayReconstructionCpu.*` 5/5 (ox_render_tests: 86/86), `RayReconstructionTest.*` 6/6, `PostProcessTest.Dlss*` 7/7.
Temporary `RRX-TEMP` env switches removed (no `getenv` left in the RR / NGX files).

## Changes in this pass
- dlss_ngx.cpp: RR preset E for all quality modes (`RayReconstruction.Hint.Render.Preset.*`); live NGX feature counter
  (`dlss::liveFeatureCount()`, public `ngxLiveFeatureCount()` in postprocess.hpp / upscaler_api.cpp; stub returns 0).
- postprocess_rr_tests.cpp: quality test also compares against a converged hybrid frame (32 spp, no SVGF, TAA 64
  frames); asserts RR >= SVGF - 0.25 dB there and >= SVGF - 0.6 dB against the path tracer (justification in the test
  and in render_postprocess.md). Toggle test: asserts NGX feature count and no VRAM growth over 6 toggles; no longer
  expects the driver VRAM to drop back after RR is switched off (NGX keeps the RR network until device shutdown).
- Docs: guide 24 (step 5 RR subsection, concept row, pitfall), guide 25 (RR paragraph in step 4), render_postprocess.md
  (RR section, tests, limits). perf.md / CHANGELOG text in out-dlssrr.md.

## Findings
- Quality gap vs path tracer (RR 25.32 vs SVGF+DLSS 25.81 dB) is the hybrid lighting bias (missing second bounce in
  reflections) that SVGF's residual sparkle partly fills by chance; vs converged hybrid RR wins by 1-3 dB. Ruled out
  by measurement: roughness encoding (Unpacked alpha: same), hit distance (no effect), matrices (needed: 3x flicker
  without), firefly clamp (16x vs 64x identical), guide formulas match NVIDIA's (diffuse = base*(1-metal), spec = F0*A+B).
- Toggle "leak" was a wrong expectation: NGX keeps ~180-510 MB after first RR use until NGX shutdown; destroying the
  Renderer frees only ~120 MB. Live feature count returns to the SVGF value; no growth over toggles.
- The DLSS SR feature of a view stays allocated while RR runs (2 live features) — small, not changed.
- r.VRS is off by default in code and the RR tests do not enable it: VRS does not affect these results.
- Build dir note: the dlssrr dir had a broken .ninja_deps (objects with 0 deps → stale layout → SEH in SetUp); fixed
  with `ninja -t clean` + rebuild.
- One PerfReport run ended silently right after device creation (no output, exit status not captured); the rerun was fine.

## Open
- Not checked by eye in the windowed player / editor; no Linux; only RTX 3080 (RR is expensive on Ampere).
