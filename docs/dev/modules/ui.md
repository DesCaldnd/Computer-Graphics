# ui (`Oxwald::ui`)

In-game debug UI / profiler (Dear ImGui 1.92, docking) and game UI (RmlUi 6.2), both drawn by one rhi render
feature at the renderer's `Overlay` injection point. No ImGui Vulkan backend and no RmlUi GL backend: input comes from
`ox::InputEvent`, rendering is our own pipeline. Namespace `ox::ui`, umbrella header `<oxwald/ui/ui.hpp>`.

Deps: `render`, `runtime` (required), imgui, RmlUi (freetype), stb (images). Optional, detected at configure time:
`script` (`OX_UI_HAS_SCRIPT`: Lua `ui`/`debug` tables), `async` (`OX_UI_HAS_ASYNC`: coroutine list), `gameplay`
(`OX_UI_HAS_GAMEPLAY`: debug-draw toggles). Shaders: `engine/shaders/ui/`. Resources: `engine/ui/resources/`
(fonts: Inter Regular/Bold + JetBrains Mono, both OFL with Cyrillic, licenses next to them; sample documents).

## Engine integration

```cpp
ox::Engine engine;
engine.addModule(std::make_unique<ox::ui::UiModule>());                 // UiSystem service, input, Lua, console
engine.setRenderer(ox::ui::withUi(ox::render::createRenderer()));       // UI render feature + "Auto" benchmark
```

* `UiModule::init` registers `ui::UiSystem` in `Services` and attaches it: `InputSystem::setEventFilter` (routing),
  debug windows, the `settings` data model over `Engine::settings()`, Lua APIs (when a `ScriptVM` service exists),
  console commands `ui.debug`, `ui.open <window>`, `ui.load <doc>`, `ui.hide <doc>`, `ui.reload`. ImGui layout is
  persisted in `user://imgui.ini`.
* Frame: `preUpdate` → `UiSystem::beginFrame` (RmlUi update + hot reload poll, ImGui `NewFrame`, debug windows);
  scripts/systems may call ImGui (or Lua `debug.*`) during the frame; `Engine::frameEnded` → `endFrame` (RmlUi
  render, ImGui render, publish the `UiFrame`). The render thread draws the latest published frame (≤ 1 frame latency).
* `withUi()` wraps the render module's `IRenderer`: after `init` it adds `UiOverlayFeature` to its `Renderer`
  (`render::rendererOf`), and in `render()` — between two device frames on the render thread — runs
  `render::autoDetectQuality` when the settings menu asked for "Auto". The result is applied on the game thread
  (`render::applyQuality`, `Settings::captureFromCVars`, signal `UiSystem::qualityAutoDetected`).
* The render module cannot link ui, so its `// [feature-area: ui]` marker is a comment; tools/tests call
  `ui::attachRenderer(renderer, ui.bridgeShared())` themselves.

## Threading / data flow

```
game thread                                              render thread
ImGuiLayer::endFrame ┐                                   UiOverlayFeature::setup (Overlay, order 10000)
GameUI::render       ┴─► UiFrame ─► UiRenderBridge ──────► latest() → upload vertices/indices/transforms (frame memory)
UiTextureStore (CPU pixels, versioned) ────────────────► sync rhi textures when the store version changed
DebugTools ◄── RenderInfo (RenderStats, GPU memory, DeviceCaps, upscaler availability, render-graph capture) ◄──┘
```

`UiFrame`: `UiVertex {vec2 pos, vec2 uv, u32 rgba8}` (= `ImDrawVert`), u32 indices, commands with clip rect, texture
reference, `xform` (scale/offset, ImGui display pos × framebuffer scale or RmlUi translation), optional transform index
(RmlUi `transform`) and flags (`kUiPremultiplied` for RmlUi). Texture references: `0` = white, a `UiTextureStore` id,
or `ui::bindlessTexture(index)` (bit 63) for any sampled bindless image — `ImGui::Image(ImTextureRef(
ImGuiLayer::textureId(device.sampledIndex(tex))), size)` shows render targets/asset textures (keep them in
`SHADER_READ_ONLY` layout).

## Rendering (`UiOverlayFeature`, `engine/shaders/ui/`)

One graphics pass "UI" after `EditorOverlays`/`DebugLines`, `color(SceneColorLDR, LOAD)` at output resolution.
Vertex *and index* pulling through buffer device addresses (`draw(indexCount)`, no index buffer binding), bindless
texture + `LinearClamp` sampler, per-command scissor, premultiplied blending (`ONE, ONE_MINUS_SRC_ALPHA`; ImGui's
straight-alpha colours are premultiplied in the shader). `SceneColorLDR` is UNORM and already display-encoded, so UI
colours authored in sRGB are written/blended as is (verified: 50 % white over (230,90,40) gives (242,172,147)). UI
frames whose size differs from the output (resize in flight) are scaled to the full target. Views with
`ViewFlags::overlays == false` are skipped; editor views only with `bridge.drawInEditorViews`. Textures are uploaded
synchronously on change (font atlas growth, image loads) and destroyed when their store entry disappears.

Cost (Apple M4 Pro, 1080p, 4 debug windows incl. a 200-line console + RmlUi settings menu: 71 draws, 8.1 K vertices):
**UI pass ≈ 0.11–0.34 ms GPU** (varies run to run).

## ImGui layer + debug tools

`ImGuiLayer` (own context, docking, 1.92 dynamic textures via `ImGuiBackendFlags_RendererHasTextures`, fonts Inter +
JetBrains Mono rasterised on demand → Cyrillic works without glyph ranges). DPI: ImGui works in points
(`framebuffer / dpiScale`) with `DisplayFramebufferScale = dpiScale`. The overlay (menu bar + pass-through dock
space + tool windows) toggles with **F1** or **`~`** (`~` types into a focused text field instead). Windows submitted
by game code / scripts are always drawn. `addWindow(path, fn)`, `addAlwaysOnTop(fn)`, `addMenu(fn)` extend it.

`DebugTools` windows: **Stats** (FPS + frame-time graph, game/render thread CPU, GPU per pass, draws, triangles,
instances visible/total, lights/shadow maps, VRAM bar; `ui.ShowStats 1` = HUD also when the overlay is hidden),
**Console** (runtime `Console`: log view with level/text filters, Tab completion with value preview, Up/Down history),
**Settings & CVars** (Low/Medium/High/Ultra/Auto, per-group levels, RT checkbox greyed with
`whyRayTracingUnavailable()`, upscaler combo with disabled entries + reasons, every cvar editable with filter;
edits go through the console so the engine reacts like to typed commands), **Entity Inspector** (hierarchy, name,
active, reflection-driven editing of all registered components incl. enums, colours, quats as Euler, arrays, maps,
optionals; `notifyChanged` after edits), **Render Graph** (passes in execution order with queue/batch/barriers/GPU ms,
culled passes, resources with alias slots/sizes/lifetimes, transient memory, copy Graphviz), **Coroutines**
(`CoroutineScheduler::coroutines()`, cancel by owner), **Debug Draw** (physics/AI/animation/audio/spline runtime
toggles, `r.DebugView`, wireframe, RmlUi debugger, reload UI).

## Game UI (RmlUi)

`GameUI`: one context, documents by name (the path given to `load`; relative paths resolve against
`GameUIConfig::root`, default `project://UI/`), files through the VFS (native paths as fallback),
`show/hide/close/isVisible`, `addEventListener(doc, id, event, fn)` (re-attached after reloads), data models
(`createModel/model/removeModel`), localisation (`setTranslator(fn)` for every text, `setTranslations(table)` for
`#key` texts; both reload documents), RmlUi debugger. Hot reload: every `hotReloadInterval` the `.rml/.rcss` files
below the loaded documents' directories are polled (VFS mtimes); a change clears RmlUi's style sheet/template caches
and reloads all documents keeping visibility (`documentReloaded` signal). Render interface: compiled geometry,
textures (PNG/JPG/TGA via stb, premultiplied), scissor, transforms. Not implemented (RmlUi optional features): clip
masks (clipping inside transformed elements), layers/filters/shaders (`filter`, `box-shadow`, gradients decorators
that need `CompileShader`).

**Input routing** (`UiSystem::processEvent`, installed as the `InputSystem` filter): ImGui first (consumes presses
while `WantCaptureMouse/Keyboard/TextInput`, toggle keys), then RmlUi (presses over interactive elements, keys while
a text field has focus or an element stops propagation), then the game. Mouse moves and releases are never consumed.
HUD documents use `pointer-events: none` on `body` so clicks pass through except on opted-in elements. On macOS
cursor positions (points) are scaled by the DPI factor (`UiConfig::cursorScale`).

### Settings menu (data model `settings`)

`GameUI::bindSettingsMenu(settings, hooks)` (done by `UiSystem::attach`): `qualities` (Low/Medium/High/Ultra/Auto),
`quality`, `autoStatus`, `windowModes`, `windowMode`, `resolutions` (GLFW monitor modes), `resolution`, `vsync`,
`rayTracing`, `rtAvailable`, `rtReason`, `upscalers[] {name, available, reason}`, `upscaler`, `upscalerReason`,
`masterVolume`, `musicVolume`, `sfxVolume`, `status`; events `set_quality(q)`, `set_upscaler(n)`,
`set_window_mode(i)`, `toggle_vsync`, `toggle_rt`, `save`, `revert`. Every change goes through `Settings::setGraphics/
setAudio` (cvars + scalability + `changed` signal → window/renderer apply). Availability comes from the render info
(`DeviceCaps`; `ui::upscalerAvailability(caps)`: FSR1 everywhere, DLSS only NVIDIA RTX on Windows/Linux — switch to
render's own query once the post-process team exposes one).

### Samples (`engine/ui/resources/sample/`)

`base.rcss` (RmlUi has no user-agent sheet), `theme.rcss`, `main_menu.rml`, `hud.rml` (model `hud`: health,
maxHealth, ammo, reserve, objective), `settings.rml` (model `settings`), `menu.lua` (menu → settings → HUD flow +
a Lua debug window). Copy them into `<project>/UI/`.

## Lua (script module)

```lua
-- game UI
local hud = ui.createModel("hud", { health = 100, objective = "Найдите выход" },
                           { hit = function(args) print("hit", args[1]) end })   -- before ui.load
ui.load("hud.rml", true)          -- ui.show / ui.hide / ui.close / ui.isVisible
hud:set("health", 42); hud:get("health"); hud:on("hit", fn)
local id = ui.on("main_menu.rml", "play", "click", function(ev) print(ev.type, ev.target) end); ui.off(id)
ui.setText(doc, id, rml); ui.getText(doc, id); ui.setClass(doc, id, "low", true); ui.setTranslations({ play = "Играть" })

-- immediate-mode debug UI (ImGui subset)
debug.window("Player", function()
    debug.text("hp", hp)
    if debug.button("Heal") then hp = 100 end
    god = debug.checkbox("God mode", god)
    speed = debug.sliderFloat("Speed", speed, 0, 20)
    name = debug.inputText("Name", name)
    tint = debug.colorEdit("Tint", tint)        -- vec3 / vec4 / {r,g,b,a}
    debug.plotLines("fps", history, "fps", 0, 120, 60)
end)
-- also debug.beginWindow/endWindow (unbalanced windows are closed at frame end), separator, sameLine, overlayVisible
```

Second return value of the editing widgets is `changed`. Calls outside a UI frame are no-ops. The `ScriptVM` must
outlive the bindings (`UiSystem::unbindLua()`; automatic in the engine).

## Tests

`ox_ui_tests` (label `ui`, CPU): ImGui draw data + font textures + bindless ids, toggle keys, data binding updates
text, `.rcss` hot reload changes the computed colour, RmlUi frame (premultiplied), input routing (UI consumes the
click → `InputSystem` doesn't see it; click-through; ImGui window before game UI), console window executes a cvar
command typed through injected key/text events, all debug windows draw without an engine, settings menu changes the
quality cvars (Low/Ultra/Auto, DLSS greyed with reason, FSR1 selectable, RT disabled, vsync checkbox), sample
documents load without RmlUi errors, Lua `debug.*` + `ui.createModel` + data events.
`ox_ui_gpu_tests` (labels `ui;gpu`, reuse the render module's offscreen fixture): goldens `ui_imgui` (text incl.
Cyrillic, filled/rounded rects, 50 % blend) and `ui_rmlui` (styled panel, button, Cyrillic text, translucent box,
rotated element, monospace font), debug overlay + render info + render-graph capture, texture lifetime, 1080p perf
report, full runtime integration (UiModule + `withUi(createRenderer())`, F1 through the engine input, `ui.debug`,
"Auto" benchmark between frames). Goldens: `engine/ui/tests/data/golden/`.

## Known limits / TODO

* RmlUi clip masks, layers, filters and shader decorators are not implemented (transformed elements are scissored
  only by their untransformed rect; `filter`/`box-shadow`/gradient decorators are ignored).
* ImGui user draw callbacks are skipped (they would run on the game thread). One ImGui viewport (no multi-viewport).
* Textures upload synchronously on the render thread when they change (font atlas growth: a few frames at startup).
* UI frames are "latest wins" (not tied to snapshot slots); fine for UI, documented ≤ 1 frame latency.
* One `GameUI` per process (RmlUi global state); one RmlUi context.
* `apps/player` does not install the module yet: add the two lines from "Engine integration" (owner: player).
