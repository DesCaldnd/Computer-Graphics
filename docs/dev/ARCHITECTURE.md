# OxwaldEngine — architecture and contributor conventions

This is the contract every module follows. Read it fully before writing code.
The product roadmap is in `OxwaldEngine — план развития.md` (Russian). The user guide lives in `docs/guide/`.

## 1. Build

- C++20, CMake ≥ 3.25, Ninja, vcpkg manifest mode (`vcpkg.json`). **Every third-party dependency comes from vcpkg**
  (overlay ports in `vcpkg-overlays/ports` for MoltenVK and the NVIDIA DLSS SDK). Do not use FetchContent, git
  submodules or Homebrew libraries.
- Dependencies are installed once into `<repo>/vcpkg_installed` (shared by all build dirs).
- Presets: `debug`, `dev` (RelWithDebInfo, default for daily work), `release`.
  ```sh
  cmake --preset dev                       # configure → build/dev
  cmake --build --preset dev               # build everything
  ctest --preset dev                       # all tests
  ctest --preset cpu                       # tests that do not need a GPU
  ```
- Working on one area while others are mid-edit: use your own build dir and restrict modules:
  ```sh
  cmake --preset dev -B build/<you> -DVCPKG_MANIFEST_INSTALL=OFF -DOX_MODULES="core;scene"
  cmake --build build/<you> --target ox_scene_tests && ctest --test-dir build/<you> -L scene
  ```
- macOS: the project is compiled with AppleClang (`/usr/bin/clang++`), same as the vcpkg ports.
- Dependencies: `tools/bootstrap.sh` installs everything into `vcpkg_installed`; `tools/bootstrap.sh --no-editor` skips
  Qt (engine, player and tests only).
- Options (top-level `CMakeLists.txt`): `OX_BUILD_TESTS` (ON), `OX_BUILD_EDITOR` (ON; declared but currently
  unused — the editor is built whenever Qt 6 is found), `OX_ENABLE_TRACY` (ON), `OX_ENABLE_DLSS` (ON; only has an
  effect where the NGX runtime exists), `OX_MODULES` (empty = everything; also filters the top-level dirs `editor`,
  `apps`, `tools`, `samples`), and `OX_WARNINGS_AS_ERRORS` (option default OFF; **ON in the `dev` and `debug`
  presets**; adds `-Werror` / `/WX` to targets using `ox_set_warnings` — all modules and their tests, the editor,
  the player and the tools. Code under `samples/` gets the same warnings but they stay non-fatal). The `release`
  preset turns off `OX_BUILD_TESTS` and `OX_ENABLE_TRACY`. On macOS the link step passes
  `-no_warn_duplicate_libraries` (CMake repeats static archives on purpose; Apple's ld warned about each).

## 2. Repository layout

```
engine/<module>/                one static library per module, auto-discovered
    CMakeLists.txt              calls ox_add_module(<module> PUBLIC_DEPS ... PRIVATE_DEPS ...)
    include/oxwald/<module>/    public headers  → #include <oxwald/<module>/foo.hpp>
    src/                        implementation (+ private headers)
    tests/                      GoogleTest sources → ox_<module>_tests ; tests/data for fixtures
engine/shaders/<area>/          GLSL shaders, include root is engine/shaders (e.g. #include "common/pbr.glsl")
editor/                         Qt editor application (OxwaldEditor) + ox_editor library + ox_editor_tests
apps/player/                    standalone game runtime OxwaldPlayer (GLFW window or headless, no editor)
tools/                          bootstrap.sh (vcpkg install), oxdump (binary ↔ JSON), oximport (import one asset),
                                oxpack (cook a project into a .oxpak), rhi_window_smoke, sanitizers/ (TSan suppressions)
samples/OxwaldShowcase/         sample project demonstrating every feature
samples/guide_examples/         compilable/tested versions of the user guide snippets (ctest label `guide`)
docs/guide/                     user guide (Russian) with code examples
docs/dev/                       developer docs (this file, module notes in modules/, BACKLOG, perf notes)
vcpkg-overlays/ports/           custom vcpkg ports (moltenvk, nvidia-dlss, vulkan)
vcpkg-overlays/triplets/        arm64-osx triplet building the ports with AppleClang
```

Module targets: `ox_<name>` with alias `Oxwald::<name>`. Modules:

| Module | Depends on | Purpose |
| --- | --- | --- |
| `core` | glm, nlohmann-json; private enkiTS; optional Tracy (`OX_ENABLE_TRACY` + package present) | types, log, assert, math, hashing, UUID, time, jobs, services (DI), events, reflection, serialization, cvars & scalability, paths/VFS, file watching, frame allocator, profiling macros, debug-draw collector |
| `scene` | core, EnTT | ECS world, entities, core components, hierarchy/transforms, component registry, systems & phases, scene (de)serialization, prefabs |
| `rhi` | core, volk, Vulkan-Headers; private VMA, vk-bootstrap, shaderc, spirv-reflect. Separate `ox_rhi_glfw` (rhi + GLFW surface) | Vulkan device, swapchain, resources, bindless, command lists, render graph, shader compiler & hot reload, BLAS/TLAS, DeviceCaps |
| `assets` | core, optional animation (`OX_ASSETS_HAS_ANIMATION`); private scene, assimp, fastgltf, meshoptimizer, KTX, stb, tinyexr, optional zstd (`OX_ASSETS_HAS_ZSTD`) | asset database (UUID + .meta), importers, CPU mesh/texture/material data, meshlets/LODs, async loading & hot reload, pak archives |
| `render` | rhi, scene, assets (when configured, else header-only); private volk. Deferred optional links: runtime (`IRenderer` adapter, `OX_RENDER_HAS_RUNTIME`), world (`OX_RENDER_HAS_WORLD`), private gameplay (`OX_RENDER_HAS_GAMEPLAY`); NVIDIA DLSS/NGX on Windows/Linux (`OX_RENDER_HAS_DLSS`) | clustered forward+ renderer, GPU scene, extract, materials, feature extension API, picking, editor overlays, debug views, stats, quality auto-detect. Feature areas: **lighting/shadows** (PBR, IBL, CSM/spot atlas/point, PCF/PCSS); **reflections & AO** (probes, SSR, planar, GTAO); **GI** (irradiance volumes, RT DDGI); **volumetrics** (froxel fog, volumetric clouds); **translucency** (OIT, refraction, water, GPU particles); **ray tracing** (RT shadows/reflections/AO/GI/refraction, ReSTIR DI, path tracer, SVGF denoiser); **post-processing & upscalers** (auto exposure, bloom, DOF, motion blur, grading, TAA/TAAU/FXAA, FSR 1, DLSS); **GPU-driven** (GPU/HiZ culling, LODs, meshlets/mesh shaders, async compute, texture streaming, parallel recording); **world rendering** (terrain, vegetation, sky, compute skinning). See `modules/render*.md` |
| `physics` | core; private Jolt (built with `-fno-rtti`) | physics world, shapes, queries, constraints, character controller (ECS glue lives in `gameplay`) |
| `animation` | core; private assimp | skeletons, clips, blending, state machines, IK, root motion, skinning |
| `spline` | core | Bézier, Catmull-Rom, B-spline/NURBS, arc-length parametrisation, path following, extrusion |
| `audio` | core; miniaudio compiled in privately | 3D audio, buses/mixer, effects, occlusion hooks |
| `ai` | core; private Recast/Detour/DetourCrowd/DetourTileCache | navmesh, path finding, crowds, dynamic obstacles, behaviour trees, perception, utility AI |
| `net` | core; private ENet | client/server transport, replication, prediction/interpolation, dedicated server |
| `script` | core, Lua 5.4, sol2; optional async (`OX_SCRIPT_HAS_ASYNC`: Lua `await` bridge) | Lua VM, sandbox, bindings, hot reload |
| `async` | core. Separate header-only `Oxwald::async_net` (async + net) for request/response RPCs | C++20 coroutines for gameplay: `Task<T>`, `Future<T>`, frame/time awaiters, thread hops, `whenAll/Any`, cancellation bound to entities (the Lua `await` bridge lives in `script`) |
| `ui` | render, runtime, imgui, RmlUi; private RmlUi Debugger; optional script/async/gameplay (`OX_UI_HAS_*`). Skipped when render or runtime is not configured | in-game debug UI/profiler (ImGui: stats, console, cvars/scalability, inspector, render graph, coroutines), game UI (RmlUi: data models, hot reload, Lua); own rhi backends drawn at the renderer's `Overlay` point |
| `world` | core; private tinyexr, stb | terrain heightfields, erosion, CDLOD, vegetation placement, sky/time of day, wind/weather, water & buoyancy, chunk streaming (CPU side, ECS-agnostic; header-only physics bridge) |
| `gameplay` | scene, physics, animation, spline, audio, ai, net, script; optional async, assets, world (`OX_GAMEPLAY_HAS_*`) | ECS components & systems binding the CPU modules to the world, Lua entity API, asset providers, world components + `WorldRenderData` |
| `runtime` | core, scene, Threads; optional GLFW (`OX_HAS_GLFW`); deferred optional links to physics, audio, script, ai, net, animation, spline, world, assets, async, gameplay, rhi/`ox_rhi_glfw` (`OX_HAS_<MODULE>`) | `Engine`, game loop, render thread, input, projects, settings, save games, console. Does **not** link `render`/`ui`: they implement runtime interfaces (`IRenderer`) and are linked by the apps |

Modules are configured in alphabetical order; links to modules configured later are resolved with
`cmake_language(DEFER)` (render, runtime, gameplay). Optional links compile in only when the target exists, so any
`OX_MODULES` subset that satisfies the hard dependencies builds.

Outside `engine/`:
- `editor/` (`ox_editor`, `OxwaldEditor`, macOS app bundle `build/<preset>/bin/OxwaldEditor.app`): hard deps core,
  scene, Qt 6 (Core/Gui/Widgets/Svg/Concurrent); optionally links rhi, assets, gameplay, runtime, async, render,
  physics, script, ai, world, spline, audio, animation (`OX_EDITOR_HAS_<MODULE>`). Built whenever Qt 6 is found (the `OX_BUILD_EDITOR`
  option is declared but not consulted yet — leave `editor` out of `OX_MODULES` to skip it).
- `apps/player/` (`OxwaldPlayer`): runtime, plus render and ui when configured (`OX_HAS_RENDER`, `OX_HAS_UI`); command
  line in `<oxwald/runtime/launch.hpp>`. Headless/server smoke tests are registered with ctest (label `player`).
- `tools/`: each tool skips itself when the modules it needs are not configured.

A module never includes another module's `src/`. Only public headers.

## 3. Code conventions

- Namespace `ox` (sub-namespaces per module are fine: `ox::rhi`, `ox::render`, `ox::physics`, ...).
- Files `snake_case.hpp/.cpp`; types `PascalCase`; functions/variables `camelCase`; members `m_camelCase`;
  constants `kCamelCase`; macros `OX_UPPER_CASE`.
- `#pragma once`. Fixed-width aliases from `<oxwald/core/types.hpp>` (`u32`, `f32`, …).
- Logging: `OX_LOG_INFO("category", "fmt {}", x)` (std::format). Invariants: `OX_ASSERT(cond, "fmt", ...)`.
- Errors that callers can handle → return `std::expected`-like `ox::Result<T>` (core) or `bool` + log. No exceptions
  across module boundaries (third-party exceptions are caught at the boundary).
- No global mutable singletons for engine services — services are registered in `ox::Services` (DI) and passed in.
  Logging, the type/reflection registry, the component registry and the cvar registry are the only process-wide
  registries (they hold type metadata, not runtime state).
- Math: glm, right-handed, **Y-up**, −Z forward for cameras, depth range [0,1] (`GLM_FORCE_DEPTH_ZERO_TO_ONE`),
  **reversed-Z** depth buffers in the renderer (near = 1, far = 0). Rotations are quaternions everywhere — never Euler
  angles in runtime state (Euler only as an editor display convenience).
- Comments explain *why*, not *what*. Keep them sparse.
- Profiling: `OX_PROFILE_ZONE()` / `OX_PROFILE_ZONE_N("name")` / `OX_PROFILE_FRAME()` from `<oxwald/core/profile.hpp>`
  (Tracy when `OX_ENABLE_TRACY`, no-ops otherwise).

## 4. Tests

- GoogleTest. Every public feature gets tests. Put them in `engine/<module>/tests/*.cpp`.
- Tests that need a Vulkan device are labelled `gpu` (`ox_add_module(... TEST_LABELS gpu)` or split into a second test
  target). They must **skip** (`GTEST_SKIP()`) cleanly when no device or a required capability (ray query, mesh
  shaders, DLSS) is missing — never fail because the hardware lacks a feature.
- Rendering tests render offscreen (no window), read back and compare with golden images in `tests/data/golden/`
  using a tolerance; when the golden is missing and `OX_UPDATE_GOLDEN=1` is set, the test writes it.
- Tests must be deterministic and run in < 10 s each.

## 5. Engine-wide systems (owned by `core` / `scene`)

### Services (DI)
`ox::Services` holds one instance per interface type: `services.add<IPhysicsWorld>(std::make_unique<...>())`,
`services.get<IPhysicsWorld>()`, `services.tryGet<T>()`. The `runtime::Engine` builds it; systems receive it.

### Reflection & serialization
`ox::reflect` registers types with fields and attributes (display name, tooltip, range, step, color, asset type,
hidden, read-only, category). JSON (de)serialization, the editor inspector, undo/redo diffs, network replication
and Lua bindings are all driven by it. Register a component once:
```cpp
OX_REFLECT_TYPE(ox::LightComponent, "Light")
    .field("type", &LightComponent::type)
    .field("color", &LightComponent::color, ox::attr::Color{})
    .field("intensity", &LightComponent::intensity, ox::attr::Range{0.f, 100000.f})
    ...;
```

### Serialization formats (scenes, prefabs, save games)
One reflection-driven archive API (`ox::serial`) with two interchangeable backends:
- **Binary** (primary, `.oxscene`, `.oxprefab`, `.oxsave`): little-endian, chunked container
  (`magic "OXB1"`, format version, header with *type schema table*: type name hash + per-field name hash/type tag),
  then tagged field records. Unknown fields are skipped by size, missing fields keep defaults → old files load in new
  builds and vice versa. Large trivially-copyable arrays are stored as raw blocks. CRC32 per chunk for corruption
  detection. Strings/arrays/maps/optionals/enums (by name hash)/entity references (remapped on load)/asset refs (UUID).
- **JSON** (human readable, `.json` suffix e.g. `level.oxscene.json`): same logical tree, field names, enums as strings.
- Lossless conversion both ways: `ox::serial::binaryToJson`, `jsonToBinary`, and the CLI `tools/oxdump`
  (`oxdump save.oxsave` → pretty JSON to stdout; `oxdump --to-binary in.json out.oxsave`).
- **Save-game framework** (`runtime`, built on the same archive): save slots, metadata header (timestamp, play time,
  level, thumbnail, game version), components/fields marked `ox::attr::SaveGame` (or a whole entity with
  `SaveGameComponent`) are persisted, plus arbitrary user sections (`ISaveable` services: quests, inventory, ...),
  versioned migrations (`registerMigration(fromVersion, fn)`), atomic writes (temp file + rename), autosave,
  async save/load on the job system.

### CVars and scalability (quality presets, like UE)
- `ox::CVar<T>` — named console variables (`"r.Shadows.Resolution"`) with description, flags and change callbacks;
  persisted to user/project settings JSON.
- Scalability groups: `ViewDistance, AntiAliasing, Shadows, GlobalIllumination, Reflections, PostProcess, Textures,
  Effects, Foliage, Shading, Volumetrics, RayTracing`, each with level `Low=0, Medium=1, High=2, Ultra=3`
  (+ `Custom` when individual cvars were overridden). A cvar may bind to a group with per-level values:
  ```cpp
  static ox::CVar<int> cvShadowRes("r.Shadows.Resolution", 2048, "Shadow map resolution",
                                   ox::Scalability::Shadows, {512, 1024, 2048, 4096});
  ```
  `ox::scalability::setOverall(Level)` / `setGroup(group, level)` apply the per-level values.
- Auto-detect: the renderer runs a short GPU benchmark (`render::benchmark`) and maps the score to levels.
- Upscaler and ray tracing are cvars too: `r.Upscaler` (`Off, FSR1, DLSS, TAAU`), `r.Upscaler.Quality`
  (`UltraPerformance, Performance, Balanced, Quality, Native` — Native = DLAA with DLSS), `r.RayTracing` (bool; only settable when
  `DeviceCaps::rayTracingSupported`), `r.RayTracing.*` per effect.

### ECS
EnTT registry wrapped by `ox::World`/`ox::Entity`. Components are plain structs registered with reflection and the
`ComponentRegistry` (name, add/remove/has, serialize, editor visibility). Each module defines its own components in
its own headers. Systems implement `ox::ISystem` with a phase: `PreUpdate`, `FixedUpdate` (physics/logic at fixed
60 Hz), `Update`, `PostUpdate` (transforms), `Extract` (render snapshot).

### Debug draw
`ox::DebugDraw` (core) collects lines/boxes/spheres/arrows/text with color, lifetime and depth-test flag. Physics,
splines, AI, editor all use it; the renderer draws it.

## 6. Rendering architecture

- Vulkan ≥ 1.2 with the 1.3 feature set required: dynamic rendering, synchronization2, descriptor indexing,
  buffer device address, timeline semaphores (core in 1.3 or as extensions — MoltenVK on macOS).
- `rhi` exposes `Device`, `Buffer`, `Texture`, `Sampler`, `Pipeline`, `CommandList`, `Fence`/timeline, `Swapchain`,
  bindless heap (one global descriptor set: sampled images, storage images, samplers; buffers through device
  addresses), upload ring buffers, a transfer queue, `DeviceCaps`, debug names via `VK_EXT_debug_utils`.
- `RenderGraph`: passes declare reads/writes; the graph computes barriers/layout transitions, culls unused passes and
  aliases transient textures. Rebuilt when settings change (e.g. toggling ray tracing) — no restart.
- Render features (`render::IRenderFeature`) plug passes into the frame at well-defined injection points and declare
  their cvars. Raster and ray traced variants of an effect are two features selected by cvars.
- Shaders: GLSL 4.60 compiled at runtime by shaderc with `#include` (root `engine/shaders`), cached SPIR-V, hot
  reload on file change. A test compiles every shader.
- Ray tracing: `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (+ optional `VK_KHR_ray_tracing_pipeline`).
  The UI checkbox is disabled with a tooltip explaining why when unsupported (e.g. MoltenVK/Apple GPUs).
- DLSS via NGX (Windows/Linux + NVIDIA RTX only); FSR 1 (EASU+RCAS) as the portable upscaler.

## 7. Collaboration rules (several people/agents work in parallel)

- Own your directories. Don't edit another module's files except for tiny, clearly necessary fixes — note them in
  your report.
- Don't touch git state (no commit/checkout/stash/reset). The integrator commits.
- Use your own build dir (`build/<you>`) with `OX_MODULES`; never delete other build dirs.
- Don't add dependencies to `vcpkg.json` yourself — ask the integrator (all expected deps are already listed).
- Write a short module note in `docs/dev/modules/<module>.md`: what exists, public API entry points, known limits.
