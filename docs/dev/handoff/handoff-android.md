# Android port: handoff (2026-10-14 ~17:40)

## Done
- Toolchain in E:\Development\Android: JDK 17 (`jdk`), SDK (`sdk`: platform-tools, build-tools 35, platforms 34/35, NDK 28.2.13676358, CMake 3.31.6, emulator 37.2, image API35 google_apis x86_64), AVD `Oxwald_API35_x86_64` (`avd`, gpu host).
- User env vars: JAVA_HOME, ANDROID_HOME, ANDROID_SDK_ROOT, ANDROID_NDK_HOME/ROOT, ANDROID_USER_HOME, ANDROID_EMULATOR_HOME, ANDROID_AVD_HOME, GRADLE_USER_HOME (all on E:). Gradle 8.11.1 dist is in Android\gradle-dist. Validation layers 1.4.363 are in Android\vvl.
- Emulator verified: guest Vulkan 1.3.0 "NVIDIA GeForce RTX 3080" (gfxstream) with every feature the engine needs (Android\vkjson-avd.json). The emulator is currently STOPPED, to save E: IO.
- The emulator's adb popup is fixed: tools/android/configure-emulator-adb.ps1 writes Emulator.conf; start-emulator.ps1 defaults to -no-window (use -NoWait to launch detached).
- Code (all of it syntax-checked with NDK clang, Windows targets build with /WX):
  - New: engine/android/** (platform, logcat, crash handler, assets, ADPF, remote console, JNI utils), engine/rhi/android/**, apps/player/android/android_main.cpp, runtime touch_input/app_lifecycle/android_keymap (.hpp/.cpp), core asset_bundle, render mobile_quality.
  - Tests: core asset_bundle_tests, runtime mobile_platform_tests, render mobile_quality_tests. All 27 pass on Windows.
  - Edited: root CMakeLists (Android block, ABI→triplet), apps/player/CMakeLists (shared lib, ctest Android.DeviceSmoke label android), rhi swapchain/device (surface generation, SUBOPTIMAL), rhi CMakeLists, ui CMakeLists (glfw optional), audio setDeviceSuspended, runtime launch splitCommandLine, runtime_renderer (mobile profile), samples/OxwaldShowcase/CMakeLists, vcpkg.json (!android), vcpkg-overlays/triplets/{x64,arm64}-android.cmake (release-only).
  - Gradle project: apps/android (wrapper generated; AGP 8.7.3, games-activity 4.0.0).
  - Scripts: tools/android/*.ps1 + debug.sh.
  - Docs: docs/guide/37-android.md (the "Проверено на AVD" section is still to be filled in), docs/dev/modules/android.md, README, CHANGELOG [Unreleased].
- Showcase pak cooked: E:\Development\OxwaldEngine\build\android\content\OxwaldShowcase.oxpak (55 MB).
- Windows hostcheck: E:\Development\OxwaldEngine\build\android-hostcheck (tools/android/hostcheck.ps1 -KeepGoing). The only failures are other agents' in-progress files (vrs tests, framegen link). The full ctest CPU run is still pending.

## Running
- vcpkg x64-android: log E:\Development\vcpkg-android-x64.log, at 5/41. Buildtrees and packages are on F:\Development (user-approved), install root E:\Development\OxwaldEngine\vcpkg_installed_android. If it died with my session, rerun: `tools/android/vcpkg-install.ps1 -Triplet x64-android -TempRoot F:\Development` (finished ports are reused from the binary cache).
- Hostcheck full build/ctest: log E:\Development\hostcheck-full.log.

## Next
1. Finish the x64 deps, then: `tools/android/start-emulator.ps1`, `tools/android/build.ps1 -Pak E:\Development\OxwaldEngine\build\android\content\OxwaldShowcase.oxpak`, fix compile/link issues (expect prefab/vcpkg toolchain details), `tools/android/smoke-test.ps1`, screencap + Read it, `tools/android/debug.ps1 -Batch` (backtrace).
2. Physics: for -NativeConfig Debug, map the Jolt Debug config to Release (release-only triplet).
3. arm64: `vcpkg-install.ps1 -Triplet arm64-android -TempRoot F:\Development`, `build.ps1 -Abi arm64-v8a`.
4. Fill the AVD results into guide 37 and the CHANGELOG; run the full hostcheck ctest.
5. When the deps are done, DELETE F:\Development\vcpkg-buildtrees and F:\Development\vcpkg-packages (user request). Do NOT touch F:\Development\AI (pre-existing).
 535/1358 Test  #525: GoldenTest.ManyPointLightsClustered .............................................................***Failed   34.08 sec
 539/1358 Test  #540: GpuDrivenTest.SteadyStateExtractAndDrawListsDoNotAllocate .......................................***Failed    3.05 sec
 541/1358 Test  #542: GpuDrivenTest.TextureStreamingRespectsBudget ....................................................***Failed    3.33 sec
 550/1358 Test  #552: NeuralTexturesTest.InferenceOnSampleMatchesRegularTextures ......................................***Failed    4.94 sec
 575/1358 Test  #543: GpuDrivenTest.AsyncPipelineCompileUsesPlaceholderUntilReady .....................................***Failed   87.41 sec
 576/1358 Test  #578: RayReconstructionTest.RuntimeToggleWithoutLeaks .................................................***Failed   18.00 sec
 591/1358 Test  #577: RayReconstructionTest.ImageQualityAndStabilityVersusSvgfPlusDlss ................................***Failed   35.79 sec
 603/1358 Test  #604: ReflectionsTest.SsrMirrorFloor ..................................................................***Failed    2.81 sec
 607/1358 Test  #608: ReflectionsTest.GtaoCorner ......................................................................***Failed    3.82 sec
 634/1358 Test  #636: TranslucencyTest.RefractiveSphereAbsorption .....................................................***Failed    2.99 sec
 664/1358 Test  #664: WorldSkinningTest.TerrainSplatLayersAndCsm ......................................................***Failed    5.53 sec
 732/1358 Test  #733: LowLatencyTest.MarkersReportedInFrameOrderAndOneSleepPerFrame ...................................***Failed    1.84 sec
 734/1358 Test  #735: LowLatencyTest.ConcurrentGameThreadSleepWhileTheRenderThreadRecreates ...........................***Failed    1.78 sec
 735/1358 Test  #734: LowLatencyTest.TogglingModesAndRecreatingTheSwapchainIsClean ....................................***Failed    2.05 sec
 780/1358 Test  #780: Input.RebindsPersistInUserSettings ..............................................................***Failed    0.78 sec

## Update: x64-android vcpkg install FINISHED (exit 0). arm64-android not started. F:\Development\vcpkg-buildtrees and vcpkg-packages are kept for the arm64 build; delete them after it (user request).

## Update (coordinator, 2026-10-14 ~19:30)
F:\Development\vcpkg-buildtrees and vcpkg-packages were DELETED after the x64-android install. For arm64-android rerun vcpkg-install.ps1 with -TempRoot F:\Development and delete those folders again afterwards.
