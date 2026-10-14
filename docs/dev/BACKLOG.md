# Integration backlog

Open items after the 0.1.0 polish pass and the Windows/RTX bring-up (0.1.1), each with the reason it is still open.
Fixed items were removed (see CHANGELOG.md, "Полировка перед выпуском" and "0.1.1 — Windows/RTX bring-up"). Closed by
the bring-up: the DLSS jitter sign (verified on an RTX 3080, nothing had to change — see
`modules/render_postprocess.md`) and the NGX runtime deployment / executable-directory feature path on Windows.
The ray tracing code ran on hardware for the first time during the bring-up (RTX 3080); what was verified and fixed
is in `modules/render.md` §11, the open ray tracing items are in their own section below.

- ai: `BTContext::user` is a `void*` (owner set through `BehaviorTree::setUserData`). Kept on purpose: `ai` does not
  depend on `scene`, and gameplay wraps the owning entity. A typed handle would need an `ai` → `scene` dependency.
- animation: clips/skeletons use their own binary format instead of `ox::serial`. Migrating means a new on-disk
  format plus a cache-invalidation step for every imported asset; scheduled together with the fastgltf importer.
- rhi: no programmatic Xcode GPU capture trigger. Captures work through Xcode / `MTL_CAPTURE_ENABLED=1` and
  MoltenVK's `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE` (documented in `RenderDocCapture::unavailableReason`); an
  `MTLCaptureManager` wrapper needs an Objective-C++ path in rhi.
- editor/vcpkg: vcpkg's app-local step rewrites install names of the dylibs it copies into `OxwaldEditor.app`, which
  invalidates their ad-hoc signatures; the editor's POST_BUILD `codesign --force --deep` stays. The installed
  MoltenVK itself is validly signed now (overlay port), so the re-sign steps in `DeployVulkanRuntime.cmake` and
  the editor tests are safety nets only. Static Qt still links its own MoltenVK reference (ICD manifest points to
  Qt's copy).
- net: the replicated field set is fixed when an object spawns (by design: both sides build the same property
  list from the spawn data). Containers inside reflected fields replicate as whole blobs; entity references inside
  containers keep their UUIDs (only level entities resolve on clients).
- gameplay: one `ScriptComponent` per entity; child colliders are not merged into the parent's rigid body
  (compound shapes must be authored on the body entity); physics raycasts return nothing in edit mode (no physics
  world exists until play). All three are design changes in gameplay, not defects.
- render/postprocess: DLSS manual checks still open after the automatic tests on the RTX 3080: windowed player and
  editor run with DLSS (window resize included), Debug build with the NGX development DLLs and their debug overlay,
  4K frame times on showcase stations 4 and 6 (Off / TAA / FSR 1 / TAAU / DLSS Quality / Performance). NGX on Linux
  (deployment, feature path, runtime) is untested: no Linux machine.
- render/postprocess: when NGX initialisation fails inside a `Renderer` (`DlssFeature::initialize`) on an NVIDIA
  device that was not probed before, that first frame runs without an upscaler; TAAU is selected only once the
  failed probe is known (`dlss::knownUnavailable`). Needs the feature selection to be re-evaluated after a failed
  `initialize` (or a probe before the selection).
- render: material height maps (`heightTexture`, `heightScale`) are not used yet — parallax needs a larger
  `GpuMaterial` (it is exactly 128 bytes today) and tangent-space ray marching in every material shader.
- render/editor: viewport asset loads run synchronously on the UI thread (the engine's job system dies with the
  engine on project switches while the viewport renderer lives on); needs an editor-owned job system.
- MoltenVK: copying a GLSL struct that contains a 64-bit `buffer_reference` by value reads zeros (driver bug);
  workaround in the shaders: access such members through the push constant block directly.
- render/volumetrics: sky light scattered by the froxel fog is not occluded (indoors the fog glows with the full sky
  ambient). Workaround: `VolumetricFogComponent::ambientIntensity` scales only the fog's sky light (keep the
  surfaces' `EnvironmentComponent::ambientIntensity`). A real fix needs sky visibility per froxel (irradiance-volume
  visibility or a sky-occlusion map), a new feature rather than a fix.
- runtime/net: while the engine loads a level asynchronously no systems run, so gameplay networking is not polled
  during the load (handshakes resume afterwards; long loads can hit the 5 s transport timeout). Polling without an
  attached world is fixed; polling during loads needs an engine-level network service.
- MoltenVK 1.4.2 logs "Blending is enabled for attachment with format VK_FORMAT_R32_UINT" for every non-blendable color attachment even when blendEnable is false (MVKPipeline.mm:1994, driver bug). Harmless; the rhi already disables blending for integer formats.

## Found during the Windows/RTX bring-up (0.1.1)

- platform (Windows): non-ASCII paths. About 170 `path.string()` calls plus `fopen` / stb / assimp go through the
  ANSI code page, so paths with characters outside it fail. Idea for a one-step fix: an application manifest with
  `activeCodePage=UTF-8` on every executable. Not done, not tested.
- ai/net determinism across platforms: `std::uniform_real_distribution` is implementation-defined and differs
  between libc++ and the MSVC STL. Still used in `engine/ai/src/nav_query.cpp` (`frand` for Detour's random point
  queries) and in the net transports' link simulation (`enet_transport.cpp`, `memory_transport.cpp`,
  `simulated_transport.cpp`): the same seed gives different gameplay on different platforms. The render content
  generators (water, vegetation) were already moved to explicit algorithms.
- world/render: `shaders/render/world/veg_cull.comp` appends visible entries with `atomicAdd`, so their order is
  not deterministic from run to run.
- render (Windows CPU): `GpuScene::updateInstances` is the CPU hotspot on Windows — 12.8 ms at 50k instances (machine
  under load) against 2.0 ms on the M4 Pro; the remaining cost is the `std::unordered_map` of instance slots. The
  quadratic hash collision case and the per-frame allocation are fixed, the map itself is not replaced.
- render perf: `r.AsyncCompute` / `r.ParallelRecording` and all CPU numbers of the RTX 3080 section in perf.md were
  measured with validation on and the machine under load. Re-measure on an idle machine before touching the `Auto`
  heuristics (reproduce: `GpuDrivenTest.StressSceneAsyncComputeAndParallelRecording`, `OX_RENDER_PERF=<n>`).
- render tests: several GPU test scenes still draw more than one random number inside one argument list
  (`rng.range()` in different arguments of the same call: `postprocess_gpu_tests.cpp`, `reflections_tests.cpp`,
  `renderer_tests.cpp`, `volumetrics_tests.cpp` stress / perf scenes). The evaluation order is unspecified, so MSVC
  and clang build different scenes there. Fixed only in `gpu_driven_tests.cpp`.
- rhi/render validation: GPU-assisted validation (`VK_LAYER_GPUAV_ENABLE=1`, layer 1.4.341) reports "Descriptor
  index N references a resource that was destroyed" in `IBL.Generate` (`ibl/prefilter.comp:14/31`), occasionally in
  `HiZ.Early` / `GpuCull.Late`. Not ray tracing code, intermittent, not analysed. Reproduce:
  `RayTracingTest.RestirShadowsManyLights` or `PathTracerPipelineMatchesRayQuery` under GPU-AV.
- editor (Linux): the Vulkan viewport creates Win32 and xcb surfaces; a Wayland surface is not implemented (the
  editor falls back to the software viewport there). Nothing of the editor was run on Linux.
- samples: `oxshowcase_generate --check` on Windows finds 26 binary assets that differ from the committed ones in
  the last bit (libm, FMA); they are compared within a tight numeric tolerance instead and were not regenerated.
- macOS re-verification after the bring-up, nothing of this was run on the Mac yet: the determinism fixes reproduce
  the clang/libc++ behaviour by construction, so the three goldens (water, clouds, vegetation) and
  `oxshowcase_generate --check` must still pass there; the render graph planner now splits more batches when async
  compute is on (off under `Auto` on MoltenVK) and its two new tests
  (`RenderGraphPlan.AsyncComputeBarriersNameNoGraphicsStages`, `IndependentGraphicsPassesDoNotWaitForAsyncCompute`)
  ran on Windows only.

- **4K upscaler frame time is bimodal on the world station.** Showcase station 6 at 3840×2160 with FSR 1 or TAAU
  Quality renders at either ~14 ms or 50–59 ms GPU with identical settings (DLSS at the same render resolution is
  stable at 16 ms). Not investigated; numbers and the command line are in `perf.md` (RTX 3080, DLSS pass).
- **Volumetrics do not scale with resolution.** Station 4 costs 37–39 ms GPU at native 4K against ~21 ms at 67 %
  render scale; which pass dominates was not measured.
- **glslang is an overlay port** (`vcpkg-overlays/ports/glslang`, bounds check in `decorateStructType`): types missing
  from `glslangTypeToIdMap` share one member remapper, a nested conversion shrinks it, and MSVC debug iterators abort.
  Report upstream and drop the overlay once fixed.
- **Debug preset on MSVC**: DLSS runs with the development NGX runtime, which stamps a banner into every frame (the
  DLSS tests leave that band out); allocation-count tests are skipped under debug iterators.

## Ray tracing: open after the first hardware run (RTX 3080, 0.1.1)

Details and the verified state: `modules/render.md` §11.

- rt/skinning: the first build of a skinned (deformable) BLAS may read the skinning arena before that frame's
  compute skinning has written it. The refits of the following frames correct the pose (in
  `SkinningTest.RayTracedShadowFollowsAnimation` the shadow is right by frame 4), but the tree may stay suboptimal
  until the next scheduled rebuild. Not measured.
- rt/denoiser: the AO and reflection signals lag under moving occluders. Only the visibility signals (sun, local
  lights, ReSTIR) clamp their history (`DenoiserSettings::clampHistory`); a heavy-tailed signal would lose energy
  with the same clamp.
- rt/path tracer: the hybrid RT passes (shadows, DDGI, AO, reflections and their denoisers, about 9 ms at 1080p on
  the RTX 3080) keep running under `r.PathTracing`, although the path traced image replaces the frame.
- rt/DDGI: colour bleed is about 2/3 of the path traced reference (`RtGlobalIlluminationBleedsColour`: 0.052 vs
  0.078), and features smaller than about the probe spacing (2 m by default) are not resolved.
- rt/path tracer: mode 1 (RT pipeline) casts no shadows from translucent objects (opaque shadows only).
- rt/reflections: fine-grained speckle noise on rough metals and around bright emissive objects with RT reflections
  (showcase stations 03 and 13). Not investigated.
- rt/validation: `VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-12281` under GPU-assisted validation on the
  periodic rebuild of a deformable BLAS is a false positive of layer 1.4.341 (it reports the new, live BLAS).
- rt/verification not done: Nsight Graphics capture (TLAS instance count, BLAS compaction ratio); interactive
  toggling of r.RayTracing in a windowed player / editor (only `RuntimeToggleRebuildsGraphWithoutLeaks` ran); the
  in-game settings menu; the manual checklist items marked "not done" in `modules/render.md` §11 (contact
  hardening at 0.5° vs 3°, 100+ ReSTIR lights, roughness sweep, DDGI scrolling and light leaks, off-screen god
  rays); Linux. The RT fixes were not re-run on the Mac.
