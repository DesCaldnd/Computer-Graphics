# editor (`OxwaldEditor`, `ox_editor`)

Qt 6 Widgets editor. `editor/` is a top-level dir: static library `ox_editor` (everything; tests link it), the macOS
app bundle `OxwaldEditor` (`src/app/main.cpp`) and `ox_editor_tests` (QtTest, label `editor`).

```sh
M="core;scene;rhi;assets;physics;animation;spline;audio;ai;net;script;async;world;gameplay;runtime;render;editor"
cmake --preset dev -B build/editor -DVCPKG_MANIFEST_INSTALL=OFF -DOX_MODULES="$M"
cmake --build build/editor --target OxwaldEditor ox_editor_tests
ctest --test-dir build/editor -L editor --output-on-failure          # QT_QPA_PLATFORM=offscreen is set by ctest
QT_QPA_PLATFORM=offscreen build/editor/bin/OxwaldEditor.app/Contents/MacOS/OxwaldEditor --screenshots docs/guide/images/editor
OxwaldEditor [--project X.oxproj|dir] [--browser] [--smoke-seconds S [--grab out.png]]
```
Hard deps: core, scene, Qt (Core/Gui/Widgets/Svg/Concurrent, Test for tests). Optional modules are linked when their
target exists and get `OX_EDITOR_HAS_<MODULE>=1` (rhi, assets, gameplay, runtime, async, render, physics, script, ai,
world, spline, audio, animation). The editor builds and its tests pass with `core;scene;editor` only (fallback paths:
plain World, file-system content browser, standalone play session) and without `runtime`.

Env: `OX_EDITOR_PREFS_DIR` (preferences dir), `OX_EDITOR_NO_VULKAN` (force the software viewport),
`OX_EDITOR_USER_DIR` (root of the engine's `user://` per project — tests/screenshots), `OX_EDITOR_HEADLESS` (engine
headless: offline audio), `OX_EDITOR_OFFSCREEN_GPU=1` (offscreen QPA: render the viewport with the GPU renderer on a
surface-less device; `--screenshots` sets it).

macOS build notes: vcpkg's applocal step rewrites dylib paths and breaks the linker's ad-hoc signature, so the app
and the test binary are re-signed POST_BUILD. Static Qt Gui links `libMoltenVK.dylib`; the bundle's Vulkan ICD
manifest is rewritten (`cmake/ShareMoltenVK.cmake`) to use that same copy (two copies crash on duplicate ObjC classes).
The test binary uses a private signed MoltenVK copy.

## Layout

| Dir | What |
| --- | --- |
| `core/` | `EditorContext` (engine host, edit world, play session, undo stack, selection, tool state, asset inspection, project, preferences, prefabs, clipboard, hidden/locked sets), `commands` (undo), `Selection`, `PlaySession`, `Project` (.oxproj), `ToolState`, `EditorPreferences`, `ActionRegistry`, `LogCapture`, `scene_templates` |
| `integration/` | `RuntimeHost` (the editor's `ox::Engine`), `integrations` (module wiring), `GameplayPlayRuntime`, `gameplay_tools` (debug-draw flags, navmesh bake, BT snapshot, collider fit, script properties, spline/terrain editing), `gameplay_inspector` (component extensions + showcase template), `render_integration` (GPU viewport renderer, thumbnails, benchmark), `input_bridge` (Qt → `InputSystem`), `runtime_settings` (live project settings, `UserSettings`), `EditorServices`, `RenderingCaps` |
| `content/` | `IAssetBackend`, `FileSystemAssetBackend`, `RegistryAssetBackend` (assets `AssetRegistry`), `ThumbnailCache` |
| `inspector/` | `PropertyEditorFactory` + leaf editors, `ComponentCard` (reflection grid + `ComponentExtensions` footers), `InspectorPanel` (entities / asset page), `AssetInspector`, `ReflectedObjectEditor`, `AddComponentPopup` |
| `panels/` | Outliner, Content Browser, Console, Stats, Coroutines, Behavior Tree |
| `viewport/` | `ViewportPanel` (tools, play input capture), `IViewportRenderer` contract, `PainterViewportRenderer` (software), `TransformGizmo`, CPU picking, `VulkanViewportWindow` (rhi) |
| `settings/` | `SettingsDialog` framework, `ProjectSettingsDialog`, `PreferencesDialog` (+ Game User Settings), `ScalabilityWidget`, `render_cvars` |
| `dialogs/` | splash, about, wordmark, project browser, `SaveGameInspector` |
| `theme/`, `widgets/`, `shell/`, `i18n/` | palette/QSS/icons, custom widgets, `MainWindow`/`EditorApp`/screenshots, Russian table |

## The editor's Engine (`RuntimeHost`)

`EditorContext` owns a `RuntimeHost` which owns one `ox::Engine` with `EngineConfig{editor = true,
threadedRendering = false, startupScene = "-", loadUserSettings = false, saveUserSettingsOnShutdown = false,
fileWatching = true}` (headless + offline audio under the offscreen QPA / `OX_EDITOR_HEADLESS`). The engine is
recreated on `EditorContext::setProject` (`cfg.projectPath` = the `.oxproj`); the current edit world moves over as a
`World::clone` (same UUIDs: selection and undo history stay valid). `EditorContext::editWorld()` is
`Engine::editWorld()`; scene open/new installs worlds with `Engine::setWorld` (level = `project://` URI for scenes
inside the project). Panels use `ctx.engine()` / `ctx.engineServices()` (AssetRegistry, AssetManager, PhysicsWorld,
AudioEngine, ScriptVM + AsyncBridge, CoroutineScheduler, InputSystem, Settings, SaveGameSystem, Console, gameplay
runtimes). Without the runtime module `RuntimeHost` keeps a plain World and `engine()` is null.

The `PlaySession` timer ticks the engine in edit mode too (`setEditTicking`, enabled by the main window): edit-mode
gameplay systems (debug draw, terrain, animation preview), asset hot reload (`AssetRegistry::poll` +
`AssetManager::update`). After every engine frame the engine's `DebugDraw` is flushed; the viewport copies its lines
into the editor overlay `DebugDraw` (`ViewportFrame::lines`), so gameplay debug draw is stable between engine ticks.
Console `quit` stops play; an engine-driven world swap (console `level`, `load`) resets the editor state.

## Projects (`.oxproj`)

`Project` is the runtime project file: `<root>/<Name>.oxproj` = plain JSON of `ox::ProjectSettings` (name, version,
company, `startupScene` URI, `assetDirs` (`Assets`), `modules`, `saveVersion`, `input` (InputMappingConfig),
`physics`, `audio`, `rendering.cvars`, `defaultQuality` + `scalability`, `packaging`) plus an `"editor"` object the
runtime ignores (description, maps, network, scripting, collision layers/matrix, build configuration). `save()`
normalises the runtime part through `json::toPlain(ProjectSettings)` so OxwaldPlayer/Engine read exactly what the
editor wrote. `contentDir()` = `<root>/assetDirs[0]`; `uriForPath`/`pathForUri` convert to `project://Assets/...`.
Legacy editor projects (`.oxproject` + `Content/` + `Config/ProjectSettings.json`) are converted on open
(`assetDirs = ["Content"]`). `applyCVarSettings` = `Settings::applyProjectDefaults`; `captureCVarSettings` writes the
overall level (or `Custom` + per-group levels) and non-scalability cvar overrides.

Project Settings dialog → `.oxproj`: General (+ engine module toggles), Maps, Packaging (target platforms, always
include), Rendering/Scalability (cvars), Physics (gravity/rate/substeps/max bodies; layers in `editor`), Audio
(`busVolumes`), Input (actions with value types + bindings of the default context, key capture → `Key.<Name>`),
Networking/Scripting (`editor`). Apply saves and pushes input mappings + gravity into the running engine
(`applyProjectSettingsLive`). Editor Preferences stay in `EditorPreferences.json`; the **Game User Settings** page
(Preferences → Play) edits the runtime `UserSettings` (`user://settings.json`: resolution, window mode, vsync, fps
limit, quality, RT, upscaler, FOV, volume, mouse, language) with "Apply to Editor Session" (`Settings::apply`).

## Play-in-editor

`PlaySession::start` → `Engine::enterPlayMode()` (World::clone) → every `IPlayRuntime::begin(playWorld, scheduler,
services, mode)`. `GameplayPlayRuntime` (gameplay): the engine already registered `addGameplaySystems`; Simulate
disables script/AI/perception/navigation/net/spline-follower systems and script components of the clone (physics,
animation and world keep running; `simulatesPhysics()` keeps `SystemContext::playing` true). Pause =
`Engine::setPaused` (frames still run with dt 0), Step = `stepFrames(1)` + one fixed step, Stop = `exitPlayMode()`
(the edit world is never touched; tested with a falling rigid body). Fallback without runtime: own clone + scheduler +
`addGameplaySystems` from the play runtime.

Input (Play, not Simulate): clicking into the viewport captures the mouse (hidden, re-centred; raw deltas), keys/
buttons/wheel/motion are injected into `InputSystem` (`input_bridge`: Qt key → `ox::Key`); **Shift+F1** releases the
mouse (editor camera works again, click to recapture), **Esc** stops play. Lua errors are logged with `chunk:line`;
the Console renders `file.lua:42` as links (resolved against Assets/, the project root and `scripts/`) that open the
code editor from Preferences → Tools (`%f`/`%l`) or the system default.

## Content browser on the AssetRegistry

`RegistryAssetBackend` (installed whenever the engine starts with an `AssetRegistry`; `FileSystemAssetBackend`
otherwise): types from the database (`Model` for glTF/FBX/OBJ…, `Texture`, `Material`, `Script`, `BehaviorTree`,
`AnimatorController`, `Audio`, …; sub-assets `Mesh/n`, `Material/n` of models in pickers), Finder drops / Import… copy
+ `scan()` + `import()` (errors in the status bar), Reimport, rename/move with the `.meta` (UUID kept; `scan()`
re-maps), delete with a dependency-aware warning (`dependents()` incl. sub-assets), new assets (Folder, Scene,
Prefab, Material `.oxmat`, Lua script template with declared properties, Behavior Tree `.oxbt`) get metas at once.
Thumbnails (`ThumbnailCache`): disk cache `.oxcache/thumbnails/<uuid>.png`; textures via Qt readers or the decoded
`.oxtex` artifact (BC → RGBA8, half floats tonemapped); meshes/models/prefabs/materials via `IThumbnailRenderer`.
Selecting an asset shows the **asset inspector**: header, Reimport, import settings through reflection
(`TextureImportSettings`, `ModelImportSettings`, `HeightmapImportSettings`; raw JSON otherwise) with Apply &
Reimport, `.oxmat` values (`MaterialAsset`) with Save, importer stats, sub-assets, dependencies, used by.
Drag & drop: Model/Prefab → viewport/outliner spawns the prefab (model prefab artifact via `loadPrefabDocument`),
Mesh → MeshRenderer entity, Material → first material slot of the object under the cursor, Script → Script component;
asset fields accept compatible drops (`resolveReference`: a Model on a Mesh field uses its first mesh). Drops onto the
Vulkan viewport window are handled too.

## Panels and tools

* **Coroutines** (dock): `CoroutineScheduler::coroutines()` every 250 ms — name, owner entity (runtime id → entity
  name; double-click selects), state, waiting on, age (game s / frames), children nested; Cancel via
  `handlesForOwner` (owner-less coroutines can't be cancelled from the editor).
* **Behavior Tree** (dock): selected entity's tree — live `BehaviorTree::trace()` statuses (Running/Success/Failure,
  bold = ticked this frame) + blackboard (entity ids shown by name) while playing, the definition's structure in edit
  mode.
* **Viewport → Show → Gameplay Debug**: physics colliders/contacts, navigation mesh, splines, skeletons, audio sources
  (pushed into the gameplay runtimes' debug flags); **Bake Navigation Mesh** (also Tools menu and the NavMeshSurface
  card) = `AIRuntime::bake` recorded as an undoable `bakedData` edit.
* **Tools → Save Game Inspector**: `SaveGameSystem::listSlots()` (name, level, play time, date, size, flags), selected
  slot decoded with `serial::binaryToJson` (oxdump), read-only, find/copy/export; "Save Play World…" while playing.
* **Component extensions** (`ComponentExtensions::add(name, {hiddenFields, footer})`): Script → declared
  `properties = {...}` (sandboxed introspection VM, cached by source) as typed fields with ranges/tooltips; edits are
  overrides in `Script.properties` (accent marker, "Reset to Script Default"). Collider → **Fit to Mesh** (mesh bounds
  via AssetManager, primitives built in). Spline → **Edit Points** tool: click selects a point, the gizmo moves it,
  Ctrl+click inserts after the selection, Del removes, Esc exits (all undoable). Terrain → **Sculpt** (Raise/Lower/
  Smooth/Flatten, radius/strength, Shift smooths, Ctrl lowers, Ctrl+wheel radius) through
  `WorldRuntime::applyBrush`, drawn as a terrain wireframe + brush ring. BehaviorTree → opens the debugger.
* Gameplay component icons (`Meta{"icon"}` names have SVGs) and category icons in Add Component.

## Rendering (render module)

`RenderIntegration` registers `EditorServices::setViewportRendererFactory([](rhi::Device&) {...})`: the viewport
creates the GPU renderer (`render::EditorViewportAdapter`, wrapped by `GpuViewportRenderer` for offscreen frames) once
its Vulkan device exists, and feeds `AssetManager` meshes/textures/materials (`makeAssetManagerProvider`, synchronous
loads on the UI thread; re-wired when the engine restarts) + hot reload. Picking = renderer ID buffer (Uuid), Stats
panel pass timings = `passTimings()`, thumbnails = offscreen `renderToImage` of a preview scene, Auto-Detect =
`render::autoDetectQuality` on the viewport device (or a short-lived headless one) via `IQualityBenchmark`. The editor
supplies grid, gizmo, light/camera shapes and gameplay debug lines as `frame.lines`. RTX/DLSS states come from the
device's `DeviceCaps` (`RhiCapsProvider`). Editor primitives use the renderer's ids (`ox.render.primitive.<Name>`) and
`assets::builtin::defaultMaterial()`; older scenes' ids are still recognised by the software renderer.

## Editing model
Panels never touch the World directly. `EditorContext::setProperty(ids, "Light", "intensity", value, phase)`,
`createEntity/createEntities(factory)`, `deleteEntities`, `duplicateEntities`, `reparentEntities`,
`addComponent/removeComponent/resetComponent`, `copy/cut/paste`, prefab calls. Everything is keyed by UUID and goes
through reflection, so components of any module work without code. Undo commands (`core/commands.hpp`):
`SetPropertyCommand`, `CreateEntitiesCommand`, `DeleteEntitiesCommand`, `ReparentCommand`, `ComponentCommand`,
`ReplaceSubtreesCommand`. While playing, edits go straight to the play world and are not recorded.

## How to add…
**A panel**: `QWidget` taking `EditorContext*`; listen to `structureChanged` / `propertiesChanged` / `worldReset` /
`Selection::changed`; register it in `MainWindow::createDocks` via `makeDock(objectName, title, icon, widget)`.
**A component footer**: `ComponentExtensions::add("MyComponent", {{"hiddenField"}, [](ctx, ids, parent) { return new
MyFooter(...); }})` with `MyFooter : ComponentExtensionWidget` (`refresh()` follows the card).
**A property editor**: `PropertyEditorFactory::registerEditor(priority, predicate, creator)`.
**A settings page**: `SettingsPage` + `addToggle/addCombo/addNumber/...` with `cvarBinding`, `projectBinding`
(".oxproj" paths, editor-only data under `editor.`) or a custom `SettingBinding`.
**An icon**: 24×24 SVG with `stroke="currentColor"` in `resources/icons/`. **A translation**: `tr()` + `i18n/translations_ru.cpp`.

## Interfaces for other modules
| Interface | Implemented by | Status |
| --- | --- | --- |
| `IViewportRenderer` (`render`, `pick`, `passTimings`, `renderOffscreen`) via `setViewportRendererFactory(fn(rhi::Device&))` | render (`EditorViewportAdapter`) | wired (`render_integration.cpp`) |
| `IThumbnailRenderer` | render integration (offscreen preview scenes) | wired; textures decoded by the editor |
| `IQualityBenchmark` | `render::autoDetectQuality` | wired (heuristic fallback without render/device) |
| `IRenderingCapsProvider` | rhi `DeviceCaps` | done |
| `IAssetBackend` | `RegistryAssetBackend` (assets) | done |
| `IPlayRuntime` | `GameplayPlayRuntime` (gameplay + runtime Engine) | done |

## Tests (`editor/tests`, 4 ctest entries)
Inspector, Command, Settings (as before: editors, undo, play clone, scenes, prefabs, gizmo, scalability, RT/DLSS
greying, preferences, theme, translation, console, screenshots) and **IntegrationTests**: `.oxproj` create/open
round trip read back by `ox::Project::load` + Engine started on it + legacy conversion; Play/Simulate/Pause/Step
through the Engine with a falling rigid body while the edit world stays unchanged and logic systems are restored;
Finder drop of a PNG → `.meta` + imported texture + thumbnail (memory + disk cache), import settings edit +
reimport, material dependency → dependents, rename keeps the UUID, created script/BT assets; script properties as
typed inspector fields with override + undo; coroutine listed (owner name, wait) and cancelled from the panel;
Project Settings writing `ProjectSettings` (fixed rate, module toggle, quality, input, editor section); console
`file:line` links and engine console commands; spline point insert/move/remove + undo, collider fit, behaviour tree
snapshot (edit/live) + panel, save game listed and decoded.

## Limits / TODO
- Renderer: the render module is being extended by several teams; when it does not compile the editor builds without
  it (software viewport). Terrain/vegetation/sky (`WorldRenderData`) are not drawn by the editor renderer yet — the
  sculpt tool shows a wireframe. Asset loads for the GPU renderer are synchronous on the UI thread.
- Terrain brush edits are runtime-only (WorldRuntime); saving them needs a heightmap asset writer.
- Coroutines without an owner cannot be cancelled from the panel (no id → handle lookup in `CoroutineScheduler`).
- Script property introspection runs top-level code in a sandbox without engine APIs (errors are logged).
- Input page edits the first context's bindings; modifiers/triggers are kept but not editable in the UI.
- Vulkan surfaces only on macOS (xcb/wayland/win32 TODO); single viewport; `QWindow` drops rely on the platform
  delivering drag events to the native window.
- rhi/vk-bootstrap: creating a surface device after a headless device in the same process crashed in
  `get_present_queue_index` — the editor never probes with a headless device while a Vulkan viewport exists.
