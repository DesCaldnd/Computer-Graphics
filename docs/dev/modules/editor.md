# editor (`OxwaldEditor`, `ox_editor`)

Qt 6 Widgets editor. `editor/` is a top-level dir: static library `ox_editor` (everything; tests link it), the macOS
app bundle `OxwaldEditor` (`src/app/main.cpp`) and `ox_editor_tests` (QtTest, label `editor`).

```sh
cmake --preset dev -B build/editor -DVCPKG_MANIFEST_INSTALL=OFF -DOX_MODULES="core;scene;rhi;editor"
cmake --build build/editor --target OxwaldEditor ox_editor_tests
ctest --test-dir build/editor -L editor --output-on-failure          # QT_QPA_PLATFORM=offscreen is set by ctest
QT_QPA_PLATFORM=offscreen build/editor/bin/OxwaldEditor.app/Contents/MacOS/OxwaldEditor --screenshots docs/guide/images/editor
OxwaldEditor [--project X.oxproject] [--browser] [--smoke-seconds S [--grab out.png]]
```
Hard deps: core, scene, Qt (Core/Gui/Widgets/Svg/Concurrent, Test for tests). Optional modules are linked when their
target exists and get `OX_EDITOR_HAS_<MODULE>=1` (rhi, assets, gameplay, runtime, async, render, physics).
Env: `OX_EDITOR_PREFS_DIR` (preferences dir override), `OX_EDITOR_NO_VULKAN` (force the software viewport).

macOS build notes: vcpkg's applocal step rewrites dylib paths and breaks the linker's ad-hoc signature, so the app
and the test binary are re-signed POST_BUILD. Static Qt Gui links `libMoltenVK.dylib`; the bundle's Vulkan ICD
manifest is rewritten (`cmake/ShareMoltenVK.cmake`) to use that same copy (two copies crash on duplicate ObjC classes).
The test binary uses a private signed MoltenVK copy because the shared `vcpkg_installed/.../libMoltenVK.dylib`
currently has an invalid signature.

## Layout

| Dir | What |
| --- | --- |
| `core/` | `EditorContext` (edit world, play session, undo stack, selection, project, preferences, prefabs, clipboard, editor-only hidden/locked sets), `commands` (undo), `Selection`, `PlaySession`, `Project`, `EditorPreferences`, `ActionRegistry`, `LogCapture`, `scene_templates` |
| `theme/` | `Theme` (dark/light palette + accent, token-substituted `resources/theme/editor.qss`, QPalette, generated check/arrow images), `Icons` (own SVG set, recoloured per QIcon mode) |
| `widgets/` | `NumberField` (type or drag-scrub, Shift ×10 / Alt ×0.1, `2*3` expressions), `VectorField`, `ColorButton`, `SearchField`, `SegmentedControl`, `ToggleSwitch`, `CollapsibleSection`, `DockTitleBar` |
| `inspector/` | `PropertyEditorFactory` + leaf editors, `ComponentCard` (reflection grid), `InspectorPanel`, `AddComponentPopup` |
| `panels/` | Outliner, Content Browser, Console, Stats |
| `viewport/` | `ViewportPanel`, `IViewportRenderer` contract, `PainterViewportRenderer` (software), `TransformGizmo`, CPU picking, `VulkanViewportWindow` (rhi) |
| `settings/` | `SettingsDialog` framework, `ProjectSettingsDialog`, `PreferencesDialog`, `ScalabilityWidget`, `render_cvars` |
| `dialogs/` | splash, about, wordmark, project browser |
| `integration/` | `EditorServices`, `RenderingCaps` (+ rhi caps provider), heuristic quality benchmark |
| `content/` | `IAssetBackend` + `FileSystemAssetBackend` |
| `shell/` | `MainWindow`, `EditorApp` (bootstrap, window rebuild on language change), screenshot generator |
| `i18n/` | in-code translator (`translations_ru.cpp`, ~590 strings) |

## Editing model
Panels never touch the World directly. `EditorContext::setProperty(ids, "Light", "intensity", value, phase)`,
`createEntity/createEntities(factory)`, `deleteEntities`, `duplicateEntities`, `reparentEntities`,
`addComponent/removeComponent/resetComponent`, `copy/cut/paste`, prefab calls. Everything is keyed by UUID and goes
through reflection (`ComponentRegistry` + `reflect::resolvePath`), so components of any module work without code.
Undo commands (`core/commands.hpp`): `SetPropertyCommand` (per-entity before/after, prefab override bookkeeping, merges
`EditPhase::Begin/Update` into one command until `End`), `CreateEntitiesCommand` (factory on first redo, then subtree
snapshots with original UUIDs/sibling index), `DeleteEntitiesCommand`, `ReparentCommand` (keeps world transform, cycle
check), `ComponentCommand`, `ReplaceSubtreesCommand` (create/revert prefab). Dirty state = `QUndoStack::isClean()`.
While playing, edits go straight to the play world and are not recorded.

Play-in-editor: `PlaySession` clones the edit world (`World::clone`), ticks a `SystemScheduler` (TransformSystem +
`IPlayRuntime`s from `EditorServices`), Pause / Step (one fixed step) / Stop (drops the clone). Simulate =
`setPlaying(false)` so `playModeOnly()` systems are skipped. Viewport shows a coloured border + pill.

Scenes: binary `.oxscene` by default, "Save as JSON" writes `.oxscene.json`. Projects: `<Name>.oxproject`,
`Content/`, `Config/ProjectSettings.json` (sections + `cvars` + `scalability` preset), `Saved/` (EditorLayout.ini,
Autosaves/, thumbnail.png). Note: runtime's `.oxproj`/`ProjectSettings` landed after this; see TODO.

## How to add…
**A panel**: write a `QWidget` taking `EditorContext*`; listen to `structureChanged` / `propertiesChanged` /
`worldReset` / `Selection::changed` (coalesce with a 0 ms timer); register it in `MainWindow::createDocks` via
`makeDock(objectName, title, icon, widget)` — it gets the title bar, View/Window menu entry and layout persistence.

**A property editor**: subclass `PropertyEditor` (`setValues(values-per-entity)`, call `commit(value(s), phase,
subPath)`), then
```cpp
PropertyEditorFactory::registerEditor(100,
    [](const reflect::TypeInfo& t, const reflect::Attributes& a) { return a.getMeta("editor") == "curve"; },
    [](const PropertyContext& c, QWidget* p) -> PropertyEditor* { return new CurveEditor(c, p); });
```
Mixed values: the editor receives one value per selected entity; return per-entity values from `commit` to edit
vectors/quaternions component-wise without flattening other components.

**A settings page**: in a `SettingsDialog` subclass create `new SettingsPage(id, title, icon, description, this)`,
add `addSection`, `addToggle/addCombo/addNumber/addText/addColor/addPath(title, description, binding)` or
`addFullWidth(customWidget, keywords)`, then `addPage(page)`. Bindings: `cvarBinding("r.Foo")`,
`projectBinding(project, "section.key")`, or any `SettingBinding{key, get, set}`. Search, undo (in-dialog stack),
Revert/Apply come for free; refreshers re-sync dependent controls (`page->addRefresher`).

**An icon**: drop a 24×24 SVG with `stroke="currentColor"` into `resources/icons/` (globbed into the qrc), use
`Icons::get("name")`.

**A translation**: wrap UI text in `tr()`; add the Russian string to `i18n/translations_ru.cpp`.

## Interfaces for other modules (`integration/`, `viewport/viewport_renderer.hpp`)
| Interface | Implemented by | Status |
| --- | --- | --- |
| `IViewportRenderer` (`render(ViewportFrame, ViewportTarget)`, `pick(frame, pixel, Uuid&)` (ID buffer), `passTimings()`, `supportsViewMode`) — register with `EditorServices::setViewportRendererFactory` | render | pending. `usesPainter()=false` renderers get `ViewportTarget{device, swapchain, commandList}` from the Vulkan viewport; editor overlays (grid, gizmo, selection, light/camera shapes, gameplay debug draw) arrive as `frame.lines` (DebugDraw depth-tested + overlay lines). Must leave the backbuffer ready for `transition(Present)`. |
| `IThumbnailRenderer::render(ThumbnailRequest)` → QImage | render/assets | pending (textures get real thumbnails already; others show typed tiles) |
| `IQualityBenchmark::run(RenderingCaps)` → `BenchmarkResult{cpuIndex, gpuIndex, levels[12]}` | render (`render::benchmark`) | heuristic in place (CPU micro-benchmark + caps) |
| `IRenderingCapsProvider` | rhi | **done**: viewport device `DeviceCaps` (`rayTracingSupported`, `whyRayTracingUnavailable`), DLSS reason (Windows/Linux + NVIDIA RTX + NGX) |
| `IAssetBackend` | assets | file-system backend in place (UUID from `.meta` `{"uuid"}` or path hash); AssetRegistry adapter pending |
| `IPlayRuntime::begin(world, scheduler, services, mode)` | gameplay/runtime | pending (would call `addGameplaySystems`) |

Vulkan viewport (rhi): `VulkanViewportWindow` (QWindow, `MetalSurface`, embedded with `createWindowContainer`)
implements `rhi::ISurfaceProvider` (VK_EXT_metal_surface on Qt's CAMetalLayer), owns the editor's single
`rhi::Device` (2 frames in flight) + `Swapchain` (recreated on resize). Without a GPU renderer each frame is drawn by
the software renderer into a QImage, uploaded through a per-frame staging buffer, `copyBufferToTexture` into the
swapchain image and presented. Falls back to the software canvas on headless platforms, `Software` preference or
device failure.

## Rendering cvars
`ensureRenderingCVars()` registers stand-ins only for names the renderer has not registered: `r.RayTracing`,
`r.RayTracing.{Shadows,Reflections,AmbientOcclusion,GlobalIllumination,Translucency}`, `r.AntiAliasing`,
`r.Upscaler`, `r.Upscaler.Quality`, `r.Upscaler.Sharpness`, `r.Tonemapper`, `r.Exposure.Default/Auto`,
`r.Shadows.Method`, `r.GI.Method`, `r.Reflections.Method`, `r.VSync`, `r.MaxFPS` and 2–4 scalability-bound cvars
per group (`r.Shadows.Resolution`, `r.Textures.MaxAnisotropy`, …). Runtime owns `r.VSync`/`t.MaxFPS`; when linked,
its cvars take precedence.

## Tests (`editor/tests`, 3 ctest entries)
Inspector editors for a struct with every reflected kind + write-back + undo, multi-select mixed values; undo/redo of
property edits, drag merging, create/delete (UUID + sibling order), reparent, duplicate/paste, outliner sync and
drag-drop, play clone isolation + pause/step, scene save/load (binary/JSON) + dirty, prefab override + revert,
gizmo math; settings: scalability Overall=Low → group cvars, Revert/Apply, auto-detect, RT toggle disabled with
tooltip / DLSS item disabled, project settings binding, preferences persistence + dialog revert, theme without QSS
warnings/unresolved tokens, Russian translation, console cvars, screenshot generation.

## Limits / TODO
- Integrate runtime's `.oxproj`/`ProjectSettings`/`UserSettings` and `InputSystem` (play-mode input injection), the
  gameplay systems (`IPlayRuntime`), assets `AssetRegistry` backend and an async Coroutines panel — modules finished
  after the editor shell; hooks exist, adapters not written.
- Vulkan surfaces only on macOS (xcb/wayland/win32 TODO). Drag-and-drop onto the Vulkan viewport is not wired.
- rhi/vk-bootstrap: creating a surface device after a headless device in the same process crashed in
  `get_present_queue_index` — the editor therefore never probes with a headless device while a Vulkan viewport exists.
- Single viewport (multi-viewport later), no marquee for planes, CPU picking uses primitive bounds/icon discs.
- Live language switch rebuilds the main window; open dialogs keep their language until reopened.
