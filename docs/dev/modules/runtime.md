# runtime (`Oxwald::runtime`)

The `Engine` that wires everything together: services, game loop and render thread, input, platform (GLFW),
projects & user settings, save games, console, and `apps/player` (OxwaldPlayer). Umbrella header
`<oxwald/runtime/runtime.hpp>`.

Hard deps: `core`, `scene`, Threads (+ `glfw` if found → `OX_HAS_GLFW`). Every other module is **optional**: after the
whole tree is configured (`cmake_language(DEFER)`), runtime links `physics audio script ai net animation spline world
assets async gameplay` when their targets exist and defines `OX_HAS_<MODULE>=1`; `ox_rhi_glfw` → `OX_HAS_RHI_GLFW`.
`render`/`ui` are **not** linked by runtime (they implement runtime interfaces and depend on it); apps link them.

## Engine lifecycle

```cpp
ox::Engine engine;
engine.addModule(std::make_unique<MyGameModule>());   // IEngineModule: registerTypes/init/registerSystems/
engine.setRenderer(std::make_unique<MyRenderer>());   //   preUpdate/onWorldUnloading/onWorldChanged/shutdown
engine.setPlatform(glfwPlatform.get());               // optional (IPlatform: events, surface, window settings)
ox::EngineConfig cfg;  cfg.projectPath = "samples/OxwaldShowcase";   // or cfg.projectSettings = ProjectSettings{...}
engine.init(cfg);
engine.run();            // or engine.tick() (measured dt) / engine.tick(dt) (editor, tests, lockstep)
engine.shutdown();       // also from ~Engine
```

**Init order** (= `Services` registration order; destroyed in reverse):
JobSystem → EventBus → FileWatcher → Vfs (`engine://` engine dir, `project://` project root (writable in editor),
`user://` `paths::userDataDir(project name)` or `cfg.userDir`) → DebugDraw → Settings (project defaults → user
settings → `--quality`/`--cvar`) → InputSystem (project mappings + user rebinds) → SaveGameSystem → Console →
modules in order: built-ins (assets — see below —, physics `PhysicsWorld`, audio `AudioEngine` (offline when headless), script `ScriptVM`
+ Lua `input` table + `AsyncBridge`, async `JobSystemExecutor` + `CoroutineScheduler`, gameplay
`addGameplaySystems`) then user modules → SystemScheduler (+TransformSystem, module systems) → startup scene →
IRenderer (`NullRenderer` by default) `init` → RenderPipeline start.
Built-ins are skipped when the project disables them (`"modules": {"physics": false}`).

**Shutdown**: render thread joined (drains queued frames) → `renderer.shutdown()` → pending level load / async
saves waited → user settings captured from cvars + saved → `onWorldUnloading` → scheduler detach → worlds destroyed
(before module services: component hooks may still use them) → modules `shutdown` (reverse) → services destroyed
(reverse: renderer … job system).

**Frame** (`Engine::tick`): platform events (+resize → render thread) → main-thread job queue → FileWatcher poll
(editor) → EventBus dispatch → level loading → `InputSystem::update` → time (`dt = realDt*timeScale`; paused → 0;
`stepFrames(n)` → exactly one fixed step per frame) → `IEngineModule::preUpdate` → `SystemScheduler::tick`
(PreUpdate, FixedUpdate×N at `fixedRate` (project, 60 Hz) with `maxSubsteps` guard, Update, PostUpdate, Extract,
flush destroyed) → `SaveGameSystem::update` (async completions, play time, autosave) → `RenderPipeline::submit`
→ frame pacing (`targetFps` / `t.MaxFPS`: sleep to deadline-1 ms then yield). `EngineStats`: frame/game/simulation/
extract/render/wait/sleep ms, fixed steps this frame and total, alpha, dropped time, fps.

PreUpdate order with all modules: `Gameplay.Net.Poll` (-900) → script VM (`bridge.update(); vm.update(dt)` — runtime
system `Runtime.ScriptVM`, or `Gameplay.Script.PreUpdate` when gameplay is linked; order 0, play mode only) →
`Runtime.Coroutines` (order 10: `scheduler.tick(realDt, frame)` with `setPaused(paused || !playing)` /
`setTimeScale(timeScale)`). FixedUpdate: physics step (0) → `Runtime.Coroutines.Fixed` (10, `fixedTick`).
Coroutine owners are `ox::entityRuntimeId(e)` (scene `runtime_id.hpp`, identical to `gameplay::coroutineOwner`):
entity coroutines are cancelled when the entity is scheduled for destruction / destroyed immediately (gameplay's
`CoroutineRuntime`, or the async module's own registry hooks when gameplay is disabled); leaving editor play mode
cancels only the play world's entity coroutines; a level change / shutdown (`onWorldUnloading` of the level world)
→ `CoroutineScheduler::cancelAll()`. Audio: `AudioEngine::update` in PostUpdate (1000).

**Worlds & modes**: `world()` (active), `loadScene(uri|path)` (sync replace), `loadSceneAdditive` / `unloadAdditive`
(UUIDs kept), `requestLevelChange(uri)` (file read + decode on the job system, `LoadingScreenHooks{begin,end}`,
`FrameContext::loading`, autosave of the level being left, swap at a later frame start; failure keeps the old level),
`setWorld`. Signals `levelUnloading`, `levelLoaded`, `modeChanged`, `frameEnded`. Editor configs (`cfg.editor`) start
in Edit; `enterPlayMode()` clones the edit world (`World::clone`) and simulates the copy, `exitPlayMode()` drops it.
`setPaused`, `setTimeScale`, `stepFrames`, `requestQuit`. Console commands: `quit pause step timescale level save load
stats help find history clear`.

## Assets (`OX_HAS_ASSETS`, built-in module "assets", first in the module order)

* **Dev/editor** (project directory): `AssetRegistry` service over `<project>/<assetDirs[0]>` (default `Assets`;
  further asset dirs are ignored with a warning), gameplay importers registered, `scan()` at init; watching +
  `poll()` every frame when `cfg.editor` or `cfg.fileWatching` (hot reimport).
* **Cooked** (`EngineConfig::pakPath` + optional `patchPaks`, later override earlier): `PakAssetSource` service only
  (no importers). Without `projectPath` the project settings come from the pak's root `<Name>.oxproj` (cookProject
  copies it in). VFS: raw pak entries mounted on `project://` (priority 10) and an asset-path view
  (priority 20): `project://<assetDir>/<asset path>` reads the cooked artifact, so `loadScene` /
  `requestLevelChange("project://Assets/Levels/a.oxscene")` and the startup scene work unchanged from a pak.
  A missing/broken pak fails `Engine::init`.
* Both: `assets::IAssetSource` + `assets::AssetManager` services (`cfg.assetMemoryBudget`), `update()` in the
  module's `preUpdate` (game thread, every frame, before the scheduler tick), `waitAll()` at shutdown. With gameplay:
  `gameplay::AssetProviders` registered for every provider interface + `GameplayAssetEvents` (hot reload into
  running scripts/trees/prefabs, see gameplay.md). Toggle with `"modules": {"assets": false}`.
* The gameplay module also registers `gameplay::ICharacterInputSource` (InputSystem actions of
  `PredictedCharacter.moveAction/jumpAction`, yaw of the primary camera, jump presses latched per frame) and passes
  `"world"` as a module toggle.

## Threading (game thread + render thread)

```
game  : |input sim N|extract N→slot0|input sim N+1|extract N+1→slot1|input sim N+2|wait slot0|extract N+2→slot0|
render:                             |render N (slot0)              |render N+1 (slot1)        |render N+2 ...
```

`RenderPipeline` has `kRenderSnapshotSlots = 2` renderer-owned snapshot slots. Slot states Free → Extracting (game) →
Ready → Rendering (render) → Free change under one mutex/condvar, so extract writes happen-before the render that
reads them; the game thread is at most one frame ahead; renders are in frame order; `resize`/`settingsChanged` are
applied on the render thread right before a `render()`. `Mode::SingleThreaded` (`cfg.threadedRendering = false`)
runs extract+render inline (editor/debugging). Dedicated servers do not run the pipeline. Verified with a recording
mock renderer and under ThreadSanitizer.

### IRenderer contract (render module implements this)

```cpp
class IRenderer {
    std::string_view name() const;
    Status init(Services&, const RenderSurface&);   // main thread, before the render thread starts
    void shutdown();                                 // main thread, after the render thread joined
    void extract(const World&, const FrameContext&); // game thread: copy into snapshot slot ctx.slot; keep no World refs
    void render(const FrameContext&);                // render thread: read only slot ctx.slot + private GPU state
    void resize(glm::uvec2 framebufferSize);         // render thread, between renders
    void settingsChanged();                          // render thread: graphics cvars/scalability changed, rebuild
    RenderStats stats() const;                       // any thread
};
```
`RenderSurface{rhi::ISurfaceProvider* provider, void* nativeWindow, uvec2 framebufferSize, f32 dpiScale}` (provider
null = headless). `FrameContext{frameIndex, slot, time, realTime, dt, realDt, alpha, timeScale, paused, editMode,
loading, viewportSize}`. Interpolate transforms with `alpha` between `WorldTransformComponent::previous`/`matrix`.

## Input

```cpp
InputMappingConfig m;                                                  // usually ProjectSettings::input (.oxproj)
m.actions = {{"Move", InputValueType::Axis2D}, {"Jump", InputValueType::Bool}};
m.contexts = {{"OnFoot", 0, {
    {"Move", "Key.D"}, {"Move", "Key.A", {InputModifier::makeNegate()}},
    {"Move", "Key.W", {InputModifier::makeSwizzle()}}, {"Move", "Key.S", {InputModifier::makeNegate(), InputModifier::makeSwizzle()}},
    {"Move", "Gamepad.LeftStick", {InputModifier::makeDeadZone(0.2f)}},
    {"Jump", "Key.Space", {}, {InputTrigger::pressed()}}}}};             // (InputBinding{action, source, modifiers, triggers})
m.activeContexts = {"OnFoot"};
input.setMappings(m);
input.inject(InputEvent::key(Key::W, true));   // any thread (GlfwPlatform, Qt viewport, tests)
input.update(dt);                              // engine, once per frame
glm::vec2 move = input.axis2D("Move"); bool jump = input.triggered("Jump");
input.bindAction("Jump", ActionEvent::Started, [](const ActionState&) {});
```
Sources: `Key.<Name>`, `Mouse.Left|Right|Middle|Button4|Button5|X|Y|XY|Wheel|WheelX`, `Gamepad.<Button>`,
`Gamepad.LeftX|LeftY|RightX|RightY|LeftTrigger|RightTrigger|LeftStick|RightStick` (any pad, largest magnitude; stick
+Y = up). Modifiers: DeadZone (radial/axial, remapped), Negate, Swizzle, Scale. Triggers: Down, Pressed, Released,
Hold (one-shot or repeating), Tap, Chord (implicit; requires another action Triggered). No triggers = active while
non-zero. Action events Started/Ongoing/Triggered/Completed/Canceled; accumulation HighestAbsolute or Cumulative.
Contexts sorted by priority; sources bound in a higher one (with `consume`) are hidden from lower ones.
Rebinding: `rebind(ctx, action, index, "Key.J")`, `captureNextInput(cb)`, `rebinds()` (persisted as
`UserSettings::inputRebinds`). Cursor modes via `setCursorMode` (platform applies). Lua (script linked): global
`input` table (`keyDown`, `keyPressed`, `action`, `triggered`, `mouseDelta`, `setCursorMode`, ...).

`GlfwPlatform::create(WindowDesc, &input)`: GLFW_NO_API window (+`rhi::initGlfwVulkan` and a
`GlfwSurfaceProvider` when rhi_glfw is linked), key/char/mouse/scroll/focus callbacks, gamepad polling (GLFW gamepad
mappings), window/borderless/fullscreen with resolution + monitor, monitors & video modes, content scale, cursor
modes, clipboard.

## Projects and settings files

* `<Name>.oxproj` (plain JSON of `ProjectSettings`): name, version, company, startupScene, assetDirs, modules,
  saveVersion, input, physics (gravity, fixedRate, maxSubsteps), audio (sampleRate, maxVoices, busVolumes), rendering
  (cvars, upscaler), defaultQuality, scalability, packaging (alwaysIncludeAssets, targetPlatforms, outputDir).
  `Project::load(dirOrFile)`, `Project::create(dir, name)`, `save()`. Missing fields keep defaults.
* `user://settings.json` (`UserSettings`): graphics (resolution, windowMode, monitor, vsync, maxFps, quality +
  per-group levels, rayTracing, upscaler + quality, fov), audio (master + bus volumes), inputRebinds,
  mouseSensitivity, invertY, language, other persisted cvars (incl. Custom scalability overrides).
* `Settings` service: `apply()`, `setGraphics()`, `setAudio()` → cvars/scalability + `changed` signal → engine
  applies window settings and `renderer.settingsChanged()` (render thread, no restart); console edits of `r.*`/`sg.*`
  do the same. `captureFromCVars()` + `save()` on shutdown. Runtime-owned cvars: `r.VSync r.WindowMode
  r.ResolutionX r.ResolutionY r.Monitor t.MaxFPS g.FOV a.MasterVolume` (render must not redeclare them);
  `r.RayTracing r.Upscaler r.Upscaler.Quality` are set by name and stay pending until render registers them.
* `ox::json::toPlain/fromPlain` — reflection-driven plain JSON (no `$type`) used for both files.

## Save games

```cpp
SaveGameSystem& saves = engine.saves();             // user://saves/<slot>.oxsave (+ .bak), OXB1 binary
saves.setThumbnailProvider([&] { return pngBytes(); });
saves.registerSaveable(questLog);                   // ISaveable: saveId(), save(Writer&), load(Reader&), onMissing()
saves.registerMigration(1, [](serial::Document& d) -> Status { /* edit d.root (v1 -> v2) */ return {}; });
saves.save("slot1", engine.world());                // sync; engine.saveGame("slot1")
SaveHandle h = saves.saveAsync("slot1", engine.world()); // snapshot now, encode + write on the job system
saves.load("slot1");                                // reloads the save's level, then applies; engine.loadGame()
saves.loadAsync("slot1");                           // read/decode/migrate in background, applied in update()
saves.quickSave(); saves.quickLoad(); saves.autosave(); saves.setAutosaveInterval(300);
for (auto& s : saves.listSlots()) { s.header.displayName; s.header.playTimeSeconds; s.corrupted; }
saves.exportJson("slot1", "slot1.oxsave.json");     // same as `oxdump`; JSON -> binary is byte-identical
```
Document kind `savegame`, version = project `saveVersion`: `header` (slot, displayName, timestamp, play time, level,
game/engine version, data version, kind, entity count), `thumbnail` blob, `world` {level, entities, destroyed},
`sections` (id → object). Entities with `SaveGameComponent` are saved whole (`saveTransform` toggles Transform) and
re-created with the same UUID if missing (parent restored, components added later removed); other entities save only
components/fields marked `attr::SaveGame` and are matched by UUID. On apply: level entities destroyed before the save
(recorded via `trackLevelEntities`) and SaveGame entities spawned after it are destroyed. EntityRefs stay valid
because UUIDs are preserved (values pass through `remapEntityRefs`). Writes: temp file + fsync → previous save renamed
to `.bak` → rename → directory fsync; failures roll back. Reads fall back to `.bak` on CRC/decode errors
(`LoadResult::fromBackup`). Per-slot mutex serialises concurrent writes. Autosaves rotate `autosave0..N-1`
(continuing after the newest file across sessions), on interval of play time and on level change.

## OxwaldPlayer

`apps/player`: `OxwaldPlayer [--project dir] [--scene uri] [--headless] [--server] [--frames N] [--width W --height H]
[--fullscreen|--borderless|--windowed] [--monitor I] [--quality low|medium|high|ultra] [--cvar name=value]... [--fps N]
[--fixed-rate HZ] [--single-thread] [--user-dir DIR]` (`parseLaunchOptions`, `LaunchOptions::toEngineConfig`).
Opens a GLFW window unless headless; NullRenderer until the render module provides one. Player-only flags: `--screenshot
file.png`, `--stats` (or `OX_FRAME_STATS=1`: per-second frame timing averages — game/render thread, frame-slot wait,
acquire, CPU, present, GPU — from `EngineStats` + `IRenderer::stats()`). With `fileWatching`/editor configs the
engine's `FileWatcher` and the asset registry's hot-reload watcher scan on background threads (250 ms); `poll()`
only delivers changes on the game thread. CTest:
`OxwaldPlayer.HeadlessSmoke` (`--headless --frames 10`), `ServerSmoke`, `BadArgument` (add `apps` to `OX_MODULES`).

## Game modules, game arguments, screenshots

* **Game modules** (`<oxwald/runtime/game_module.hpp>`): native project code registers itself with
  `OX_GAME_MODULE("Name", MyModule)` (static registration) and is linked into the hosts with WHOLE_ARCHIVE
  (`samples/OxwaldShowcase/CMakeLists.txt` links `ox_showcase_game` into OxwaldPlayer and OxwaldEditor). `Engine::init`
  adds a registered module only when the project lists it explicitly: `"modules": {"Name": true}`; it runs after the
  built-ins and the host's own modules (UiModule), so it can use every service.
* **Game arguments**: everything after `--` on the command line ends up in `LaunchOptions::gameArgs` →
  `EngineConfig::gameArgs` (`OxwaldPlayer --project X -- --tour`).
* **Screenshots**: `IRenderer::requestScreenshot(path, done)` (default: unsupported). The render module's headless
  renderer reads the offscreen target back after the next frame and writes a PNG; `withUi()` forwards it. Player:
  `--screenshot file.png` with `--frames N` writes the last frame (headless or windowed: the render module renders
  the screenshot frame once more into a capture texture of the swapchain size).

## Latency (r.Reflex, NVIDIA Reflex + engine limiter)

`r.Reflex` (Persist, `GraphicsSettings::reflex`): `Off`, `On` (default, alias `Auto`), `OnBoost`. `r.Reflex.Fallback`
(Persist): `Off`, `JustInTime` (default), `Strict`. Per frame, on the game thread, `Engine::latencyBeginFrame()` runs
before input is polled:

1. `IRenderer::latencySleep(frameIndex, mode)` — the Vulkan renderer applies the mode to the swapchain
   (`vkSetLatencySleepModeNV`, low latency + boost) and, when on, calls `vkLatencySleepNV` and waits on its timeline
   semaphore (the driver decides when the CPU should start). Returns the ms slept, or < 0 when the renderer did not
   pace the frame (no `VK_NV_low_latency2`: AMD, Intel, Apple, Android, old drivers; the FSR 3 frame generation
   proxy owns the swapchain; `NullRenderer`).
2. Without a driver sleep and `mode > 0`: the **fallback limiter** (`LatencyLimiter`, `oxwald/runtime/latency.hpp`).
   *JustInTime* first waits until the render thread took the previous frame (`RenderPipeline::waitUntilTaken`,
   bounded to 250 ms) — the **phase anchor**: at most one frame is in the pipeline and the game thread never blocks
   on a snapshot slot. It then measures the queueing of the last frames — game thread waiting for a snapshot slot
   (`EngineStats::waitForRenderMs`) + the extracted snapshot waiting for the render thread
   (`RenderPipeline::Stats::lastQueuedMs`, `EngineStats::snapshotQueuedMs`) + swapchain acquire + GPU queueing
   behind the previous frame (`LatencyStats::renderQueueWaitMs`; the runtime renderer measures previous frame's
   timeline completion − this frame's submit with a watcher thread, *signed*: negative when the GPU sat idle, so
   starvation backs the limiter off at once) — and sleeps until
   *anchor + delay*, the delay settling at the queueing minus 1 ms of headroom. Relative to the anchor the queueing
   is a static function of the delay (queue = queue₀ − delay), so the proportional controller converges; a delay
   counted from "now" (the first version) only turned slot waits into sleep: with two snapshot slots a GPU-bound game
   keeps a whole frame queued without ever blocking, and a per-frame sleep controls the frame *period*, an integrator
   that drifted (8 ms GPU frame: 31 → 20 ms instead of → 1.5 ms). The window minimum (4 frames) ignores single
   hitches; the delay halves every frame when nothing is queued (CPU bound), and the controller is proportional in
   both directions around the headroom (a one-sided version had a ~1 ms limit cycle). The render thread's frame-slot
   wait is deliberately *not* an input: it has a CPU floor (~0.8 ms of `beginFrame` work at 1440p) that no sleep
   removes, and with it the limiter overslept until the GPU starved (Showcase station 4 at 1440p: 162 → 94 fps); a
   real slot wait also shows up as positive GPU queueing.
   *Strict* flushes the render pipeline and waits until the GPU retired frame N-1 (`IRenderer::waitForFrameGpu`)
   before frame N samples input: nothing is ever queued, CPU and GPU no longer overlap.
3. Markers (`IRenderer::latencyMarker`, present id = frame index + 1): `SimulationStart` (after the sleep),
   `InputSample` (after `InputSystem::update`), `SimulationEnd` (before `RenderPipeline::submit`); the renderer adds
   `RenderSubmitStart/End` around recording + submission and `rhi::Swapchain::present()` adds `PresentStart/End`.
   Markers are sent with the mode off too, so the driver's timing reports exist for comparisons.

**With FSR 3 frame generation** (`r.FrameGeneration FSR3`, runtime player): the proxy owns the swapchain and Reflex
cannot drive it (reasons: `docs/dev/modules/render_postprocess.md` "Frame generation"), so `latencySleep` returns
< 0, `LatencyStats::reflexBehindFrameGeneration` is set and `r.Reflex On` runs the fallback limiter. The proxy's
back-pressure (its `vkQueuePresentKHR` blocks the render thread until a replacement buffer is free) reaches the
limiter as the game thread's wait for the render thread, so the limiter needs no special case; `engineLatencyMs`
still measures simulation start → GPU done of the rendered frame, and the frame generator adds its hold
(`RenderStats::frameGeneration.addedLatencyMs`) on top. Measured (RTX 3080, Showcase station 4, 1440p Ultra, vsync
off): engine latency 45.8 → 21.4 ms with JustInTime (−9 % rendered fps), 12.5 ms with Strict (−2 %), the hold
24 → 23 / 13 ms.

**Known weakness of JustInTime when GPU bound** (same scene, no frame generation, `OX_DISABLE_REFLEX=1`): 46.9 →
22.6 ms but 104 → 83 fps, while Strict reaches 10.9 ms at 92 fps and Reflex 14.0 ms at 101 fps — the controller
oversleeps (the GPU idles ~3 ms per frame; Windows timer wake-ups and the per-frame CPU floor exceed the 1 ms
headroom). CPU bound (station 7, 540p Low) JustInTime is the better fallback: 2.05 → 1.44 ms at unchanged 1520 fps,
Strict 1.21 ms at 832 fps. Open: a tighter controller (larger headroom when the GPU starves, high-resolution waitable
timer) before making Strict the GPU-bound default. With vsync (FIFO) and no frame cap neither Reflex nor the limiter
helps much (48.6 → 46.5 ms; the queue sits in the presentation engine); a frame cap just under the refresh rate
(`t.MaxFPS 97` at 100 Hz) removes it (6.4 ms). Tables: `docs/dev/perf.md` "Latency". `OX_DISABLE_REFLEX=1` (read by the runtime renderer at device
creation) creates the device without `VK_NV_low_latency2`, to measure the limiter on NVIDIA hardware.

`EngineStats::latency` / `RenderStats::latency` (`LatencyStats`): mode, `reflexSupported/Active`,
`reflexBehindFrameGeneration`, `limiterActive`,
`sleepMs`, `renderQueueWaitMs`, `engineLatencyMs` (simulation start → GPU done measured by the engine: the
`SimulationStart` marker time and the watcher thread's wake-up on the frame's timeline value, 16-frame average, any
vendor, also through the frame generation proxy) and the averages of the last driver reports (`vkGetLatencyTimingsNV`, refreshed every
8 frames): `pcLatencyMs` (simulation start → GPU render end), `gameToRenderMs`, `renderQueueMs`, `gpuRenderMs`. The
debug overlay (stats HUD + "Latency" section) and `OxwaldPlayer --stats` print them. Settings UI: player settings
menu "Low latency" (`reflexModes`, hint when Reflex is unsupported), editor Preferences → Display and Project
Settings → Rendering → Display ("Low Latency", "Latency Limiter"). The editor viewport (Qt UI thread) is not
paced by either. Numbers: `docs/dev/perf.md` "Latency: NVIDIA Reflex and the engine limiter".

## Tests

`latency_tests.cpp`: marker order and one sleep per frame (recording renderer), `Off` never limits, the limiter
controller (convergence without a limit cycle, hitch), the JustInTime limiter on a simulated GPU-bound pipeline
(a virtual GPU timeline with absolute deadlines on the high-resolution timer, so a loaded machine does not
accumulate oversleep; medians: 8 ms render → ~16.8 ms latency without, ~1.5 ms with the limiter, required < 0.4× and
< ½ render time; frame interval ≤ 1.25× render time or 1.1× the run without limiter), Strict waits for the previous
frame, `r.Reflex` as a managed setting. `pipeline_tests.cpp` covers `waitUntilTaken` / `lastQueuedMs`.

`ctest --test-dir build/<you> -L runtime` — 44 tests core+scene only, 47 with physics/audio/script/async (+Lua
input, coroutine pause/time-scale/cancel-on-unload, module toggles), plus `assets_integration_tests.cpp`: asset
database service + providers + scripts from assets, cooked game running from the pak alone (project file, startup
scene, async level change, prefab), missing pak, `--pak` mapping, coroutine owner ids on destroy/unload paths (with
and without the gameplay module). Passes under ThreadSanitizer with **no suppressions**: core's JobSystem carries
`__tsan_acquire/__tsan_release` annotations for the enkiTS hand-off (enkiTS is prebuilt without TSan, so its
synchronisation is invisible; verified by relinking against a TSan-built enkiTS — 0 reports). `tools/sanitizers/
tsan.supp` documents this and has no active entries.

## Limits / TODO

* One asset database root per project (first `assetDirs` entry).
* `listSlots()`/`slotInfo()` decode only the header and thumbnail of binary saves (the whole file is still read for
  its CRC check).
* `registerSaveableScoped(saveable)` returns a `SaveableRegistration` that unregisters on destruction (safe if the
  SaveGameSystem dies first). `validateInputMappings()` (warned by `setMappings`) and
  `ProjectSettings::unknownModules()` (warned by `Engine::init`) report typos instead of failing silently.
* `r.*`/`sg.*` cvar changes with `CVarSource::Console` (console, `CVarRegistry::set/execute`, Lua `cvar.set`) are
  captured into the user settings and re-applied at the start of the next frame.
* The audio module pushes only bus volumes whose user setting changed (game-driven bus volumes survive unrelated
  settings changes).
* Header timestamps have 1 s resolution; autosave rotation uses file mtimes.
* Gamepad sources read "any pad"; per-player device assignment (local multiplayer) is not implemented.
* Net/AI have no engine-level service (gameplay owns their runtimes); no dedicated-server net loop wiring yet.
* `ProjectSettings::assetDirs` default changed to `{"Assets"}` (asset registry convention); docs/guide still shows
  `"assets"` (case-insensitive on macOS only).
