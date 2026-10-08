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

## 2. Repository layout

```
engine/<module>/                one static library per module, auto-discovered
    CMakeLists.txt              calls ox_add_module(<module> PUBLIC_DEPS ... PRIVATE_DEPS ...)
    include/oxwald/<module>/    public headers  → #include <oxwald/<module>/foo.hpp>
    src/                        implementation (+ private headers)
    tests/                      GoogleTest sources → ox_<module>_tests ; tests/data for fixtures
engine/shaders/<area>/          GLSL shaders, include root is engine/shaders (e.g. #include "common/pbr.glsl")
editor/                         Qt editor application (OxwaldEditor)
apps/player/                    standalone game runtime (GLFW window, no editor)
tools/                          command line tools (asset packer, shader compiler, ...)
samples/OxwaldShowcase/         sample project demonstrating every feature
docs/guide/                     user guide (Russian) with code examples
docs/dev/                       developer docs (this file, module notes)
vcpkg-overlays/ports/           custom vcpkg ports
```

Module targets: `ox_<name>` with alias `Oxwald::<name>`. Modules:

| Module | Depends on | Purpose |
| --- | --- | --- |
| `core` | glm, nlohmann-json, enkiTS, Tracy | types, log, assert, math, hashing, UUID, time, jobs, services (DI), events, reflection, serialization, cvars & scalability, paths/VFS, file watching, frame allocator, profiling macros, debug-draw collector |
| `scene` | core, EnTT | ECS world, entities, core components, hierarchy/transforms, component registry, systems & phases, scene (de)serialization, prefabs |
| `rhi` | core, volk, VMA, vk-bootstrap, shaderc, spirv-reflect | Vulkan device, swapchain, resources, bindless, command lists, render graph, shader compiler & hot reload, DeviceCaps |
| `assets` | core, assimp, fastgltf, meshoptimizer, ktx, stb, tinyexr | asset database (UUID + .meta), importers, CPU mesh/texture/material data, async loading, pak archives |
| `render` | rhi, scene, assets | renderer, GPU scene, materials, lighting/shadows, render features, upscalers, quality settings |
| `physics` | core (+scene for ECS glue) , Jolt | physics world, shapes, queries, character controller |
| `animation` | core, assimp | skeletons, clips, blending, state machines, IK, root motion |
| `spline` | core | Bézier, Catmull-Rom, B-spline, arc-length parametrisation, path following |
| `audio` | core, miniaudio | 3D audio, buses/mixer, occlusion hooks |
| `ai` | core, Recast/Detour | navmesh, path finding, behaviour trees, perception |
| `net` | core, ENet | client/server transport, replication, prediction/interpolation |
| `script` | core, Lua 5.4, sol2 | Lua VM, bindings, hot reload |
| `async` | core | C++20 coroutines for gameplay: `Task<T>`, `Future<T>`, frame/time awaiters, thread hops, `whenAll/Any`, cancellation bound to entities, Lua `await` bridge |
| `ui` | render, imgui, RmlUi | in-game debug UI/profiler (ImGui), game UI (RmlUi) |
| `world` | core, scene | terrain heightfields, vegetation placement, day/night, chunk streaming (CPU side) |
| `gameplay` | scene + physics/animation/spline/audio/ai/net/script | ECS components & systems binding the CPU modules to the world |
| `runtime` | everything above | `Engine`, game loop, render thread, projects, settings, save games, packaging |

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
- Upscaler and ray tracing are cvars too: `r.Upscaler` (`Off, FSR1, DLSS`), `r.Upscaler.Quality`
  (`UltraPerformance, Performance, Balanced, Quality, DLAA/Native`), `r.RayTracing` (bool; only settable when
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
