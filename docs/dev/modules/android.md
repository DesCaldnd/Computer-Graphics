# android (`Oxwald::android`, Android only)

Platform layer of the Android port: GameActivity (androidx.games `games-activity`, prefab) driving the engine from
`android_main` in `apps/player/android/android_main.cpp`. Built only when `ANDROID` (the Gradle project
`apps/android` configures the repository's root `CMakeLists.txt` with vcpkg chained to the NDK toolchain). User
guide: [docs/guide/37-android.md](../../guide/37-android.md).

The logic that does not need Android APIs lives in the regular modules and is unit tested on the host:

| Piece | Where | Tests |
| --- | --- | --- |
| Touch -> InputEvents (virtual stick, look, tap, buttons, pinch) | `runtime/touch_input.hpp` (`TouchInputMapper`) | `ox_runtime_tests` `TouchInput.*` |
| Lifecycle state machine (surface create/destroy, audio, save) | `runtime/app_lifecycle.hpp` (`AppLifecycle`) | `AppLifecycle.*` |
| AKEYCODE / AMOTION axis tables | `runtime/android_keymap.hpp` | `AndroidKeymap.*` |
| Intent-extra command lines | `runtime/launch.hpp` (`splitCommandLine`) | `LaunchCommandLine.*` |
| APK assets as VFS source (`IAssetBundle`, `AssetIndex`, `BundleMount`, `extractBundle`) | `core/asset_bundle.hpp` | `ox_core_tests` `AssetIndex.*`, `BundleMount.*` |
| Mobile quality profile from `DeviceCaps` | `render/mobile_quality.hpp` | `ox_render_tests` `MobileQuality.*` |
| Surface loss / re-creation in the swapchain | `rhi` `ISurfaceProvider::surfaceGeneration`, `Device::releaseSurface/recreateSurface` | on device |

## Verified

* AVD `Oxwald_API35_x86_64` (API 35 x86_64, Vulkan 1.3 on the host RTX 3080 through gfxstream, 2026-10-14):
  `tools/android/smoke-test.ps1` runs the Showcase from the APK's `.oxpak` for 120–600 frames, RelWithDebInfo and
  Debug, validation on and off: 0 `[vulkan]` messages, no crashes, full lifecycle (Pause → WindowDestroyed → Stop →
  Destroy), exit code 0. `tools/android/debug.ps1 -Batch` stops in `ox::Engine::tick` with a stack down to
  `android_main`. Details and logs: guide 37, "Проверено на AVD".
* One run of the 2026-10-14 build (`E:\Development\OxwaldEngine\build\android\smoke-final`) exited with code 3: the
  player's own `--screenshot` was not written (`screenshot … smoke.png FAILED`, no render log line), while
  `adb screencap` worked. Cause: a race in `RuntimeRenderer` with threaded rendering, not the path or permissions.
  The player queues the request on the game thread before its last frame; the render thread looked at the request
  queue twice per frame (mid-frame: "is a capture texture needed?", end of frame: "what to write?"), so a request
  queued in between was taken at the end without a capture texture and failed silently. That build's faster game
  thread (53.7 fps vs 12–35 in the earlier runs) hit the window. Fix: requests are taken once per frame before
  rendering (a later one waits for the next frame; the render thread drains its queue on stop), the no-texture path
  logs an error, and `shutdown()` fails (logs + `done(false)`) requests no rendered frame took. Windows tests:
  `UiEngine.WindowedScreenshotsWhileRenderingOnTheRenderThread`, `UiEngine.ScreenshotRequestedAfterTheLastFrameFailsAtShutdown`.
* 2026-10-15 rebuild from the current tree (pak re-cooked with the current `oxpack`): four consecutive
  `smoke-test.ps1 -Frames 300` runs (`smoke-v2-1` … `smoke-v2-4`), validation on: 300 frames, `screenshot … written`,
  0 `[vulkan]` messages, no crashes, exit code 0 each. `debug.ps1 -Batch` (`lldb-v2.log`): stop in
  `ox::Engine::tick` (engine.cpp:578), stack to `android_main` (android_main.cpp:211). Script fixes: the batch
  breakpoint-thread selection uses the target's `SBProcess` and selects by index id (it once printed the stdout pipe
  thread), `adb push` stderr no longer aborts a redirected `debug.ps1` run, `smoke-test.ps1` matches only
  `[rhi] Device:` lines (a system line printed "device: -1") and prints the last stats line.
* arm64-v8a: the phone APK (132.9 MB) builds (`tools/android/build.ps1 -Abi arm64-v8a`) but has not run on a device
  yet (target: OnePlus 12, checklist in guide 37). Nothing ran on real Android hardware so far.

## Module contents (`engine/android`)

| Header | |
| --- | --- |
| `android_platform.hpp` | `AndroidPlatform : IPlatform`. `pumpCommands(block)` runs APP_CMD_* through `AppLifecycle`; `pollEvents()` (called by `Engine::frame`) drains GameActivity input buffers: touch -> `TouchInputMapper` (pad slot 3), gamepads (`AINPUT_SOURCE_GAMEPAD/JOYSTICK`, slots 0..2 by device id, axes Z/RZ or RX/RY, hat -> dpad), keys (+`Text`). Motion/key filters are cleared so all sources arrive; extra pointer axes are enabled with `GameActivityPointerAxes_enableAxis`. |
| `android_assets.hpp` | `AndroidAssetBundle` (AAssetManager), `prepareContent()`: reads `oxindex.txt`, extracts `oxwald/` (engine shaders + UI resources) and `project/` (cooked `.oxpak`) to the files dir when `oxbuild.txt` changed, sets `OXWALD_SOURCE_DIR` / `OXWALD_SHADER_DIR` / `OXWALD_UI_RESOURCE_DIR`. The player also mounts `apk://` (`BundleMount` over the whole APK). |
| `android_log.hpp` | logcat sink for `ox::log` (tag `Oxwald`, 3.8 KB chunks), stdout/stderr pipe -> `OxwaldStdout`. |
| `crash_handler.hpp` | SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT/SIGSYS on an alternate stack: `_Unwind_Backtrace` + `dladdr` to tag `OxwaldCrash`, then chains to debuggerd (tombstone). |
| `perf_hints.hpp` | ADPF via `dlsym`: `APerformanceHint` session for the game thread (target 16.67 ms, actual = `EngineStats::gameMs`), `AThermal` status/headroom logged on change. |
| `remote_console.hpp` | TCP console on `127.0.0.1:<port>` (debug APKs: 27015) executing `ox::Console` lines on the game thread; streams Info+ log lines. `adb forward` + `tools/android/console.ps1`. |
| `android_app_utils.hpp` | Intent extras over JNI, `waitForDebugger()` (TracerPid poll), ABI name. |

`ox_rhi_android` (`engine/rhi/android`): `AndroidSurfaceProvider` — `VK_KHR_android_surface`, one ANativeWindow
reference per `VkSurfaceKHR` (released in `destroySurface`), `surfaceGeneration()` bumped by `setWindow()`,
`waitForRelease()` used by APP_CMD_TERM_WINDOW.

## Window loss

APP_CMD_TERM_WINDOW -> `AppLifecycle` `destroySurface` -> `AndroidPlatform::releaseWindow()`:
`provider.setWindow(nullptr)` (framebuffer 0x0, new generation) and up to three `Engine::tick()`s until the render
thread's next `Swapchain::acquire()` has run `handleSurfaceChange()` (waitIdle, destroy images + swapchain,
`Device::releaseSurface()` -> provider releases the window) — bounded by 3 x 100 ms so a stuck GPU cannot ANR the
activity. APP_CMD_INIT_WINDOW sets the new window; the next acquire sees a non-zero size without a surface and calls
`Device::recreateSurface()` (present support re-checked) and rebuilds the swapchain. `VK_ERROR_SURFACE_LOST_KHR`
from acquire/present goes the same way.

Rotation: the swapchain keeps `preTransform = IDENTITY` (compositor rotates); with a ROTATE_90/270 display Android
returns `VK_SUBOPTIMAL_KHR` on every present, which `Swapchain::suboptimalNeedsRecreate()` ignores while the
transform is the only mismatch (size changes still arrive through `framebufferSize()`). Real pre-rotation
(rotating the projection and using `currentTransform`) is not implemented yet.

## Build integration

* Root `CMakeLists.txt`: `ANDROID` forces `OX_BUILD_EDITOR/OX_BUILD_TESTS/OX_ENABLE_DLSS` off, skips `editor/` and
  `tools/`, adds `OX_ANDROID_SANITIZER` (`address`/`hwaddress`), `OX_ANDROID_REMOTE_CONSOLE_PORT`, ninja job pools
  (`OX_ANDROID_JOBS`, links 2 at a time).
* `apps/player`: `OxwaldPlayer` is a SHARED library `liboxwald_player.so` on Android (keeps
  `Java_com_google_androidgamesdk_GameActivity_initializeNativeCode` with `-u`).
* `samples/OxwaldShowcase`: only the game module (linked WHOLE_ARCHIVE into the player) on Android.
* `vcpkg.json`: `vulkan-loader`, `vulkan-validationlayers`, `glfw3`, `nvidia-dlss` and imgui's `glfw-binding` are
  `!android`; the Android install uses `--x-no-default-features` (no Qt). Triplets `x64-android` (emulator) and
  `arm64-android` (phones), API 28 libraries, static.
* `engine/ui`: `glfw3` optional on Android.
* `engine/render`: `RuntimeRenderer::init` uses `mobileQualityProfile()` instead of the GPU benchmark on first launch.
* `engine/audio`: `AudioEngine::setDeviceSuspended()` (miniaudio `ma_engine_stop/start`) for pause/resume.
