# OxwaldPlayer — standalone game runtime (no editor).
if(NOT TARGET ox_runtime)
    message(STATUS "Oxwald: runtime module not configured, skipping OxwaldPlayer")
    return()
endif()

if(ANDROID)
    # Android: the player is a shared library (liboxwald_player.so) loaded by OxwaldActivity (GameActivity,
    # apps/android). android_main() replaces main(); the platform layer is the engine/android module.
    add_library(OxwaldPlayer SHARED android/android_main.cpp)
    set_target_properties(OxwaldPlayer PROPERTIES OUTPUT_NAME oxwald_player)
    target_link_libraries(OxwaldPlayer PRIVATE Oxwald::runtime ox_android)
    # GameActivity's JNI entry points live in a static library: keep them although nothing references them.
    target_link_options(OxwaldPlayer PRIVATE "LINKER:-u,Java_com_google_androidgamesdk_GameActivity_initializeNativeCode"
                                             "LINKER:--build-id=sha1")
    if(OX_ANDROID_REMOTE_CONSOLE_PORT)
        target_compile_definitions(OxwaldPlayer PRIVATE OX_ANDROID_DEFAULT_CONSOLE_PORT=${OX_ANDROID_REMOTE_CONSOLE_PORT})
    endif()
else()
    add_executable(OxwaldPlayer main.cpp)
    target_link_libraries(OxwaldPlayer PRIVATE Oxwald::runtime)
endif()
ox_set_warnings(OxwaldPlayer)
set_target_properties(OxwaldPlayer PROPERTIES FOLDER "apps")

# The render module (when configured) provides the Vulkan IRenderer. engine/ is configured before apps/,
# so its targets already exist here (custom commands must be added in this directory).
if(TARGET ox_render)
    target_link_libraries(OxwaldPlayer PRIVATE ox_render)
    target_compile_definitions(OxwaldPlayer PRIVATE OX_HAS_RENDER=1)
endif()
if(TARGET ox_ui)
    target_link_libraries(OxwaldPlayer PRIVATE ox_ui)
    target_compile_definitions(OxwaldPlayer PRIVATE OX_HAS_UI=1)
endif()
if(COMMAND ox_add_gpu_test_env AND TARGET ox_rhi AND NOT ANDROID)
    ox_add_gpu_test_env(OxwaldPlayer)
endif()

if(OX_BUILD_TESTS AND NOT ANDROID)
    add_test(NAME OxwaldPlayer.HeadlessSmoke
             COMMAND OxwaldPlayer --headless --frames 10 --user-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke_user
                     --quality low --cvar t.MaxFPS=0)
    add_test(NAME OxwaldPlayer.ServerSmoke
             COMMAND OxwaldPlayer --server --frames 5 --fps 240 --user-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke_user)
    add_test(NAME OxwaldPlayer.BadArgument COMMAND OxwaldPlayer --bogus)
    set_tests_properties(OxwaldPlayer.HeadlessSmoke OxwaldPlayer.ServerSmoke PROPERTIES
        LABELS "runtime;player" TIMEOUT 60 ENVIRONMENT "OXWALD_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        PASS_REGULAR_EXPRESSION "frames=10|frames=5")
    set_tests_properties(OxwaldPlayer.BadArgument PROPERTIES LABELS "runtime;player" WILL_FAIL TRUE)

    # Android on-device smoke test (label android): runs the installed APK on the connected device / emulator for
    # 300 frames with Vulkan validation and checks the log. Skipped (77) without a device or installed APK:
    #   ctest -L android --output-on-failure
    find_program(OX_POWERSHELL NAMES pwsh powershell)
    if(OX_POWERSHELL)
        add_test(NAME Android.DeviceSmoke
                 COMMAND ${OX_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                         -File ${CMAKE_SOURCE_DIR}/tools/android/smoke-test.ps1 -Frames 300)
        set_tests_properties(Android.DeviceSmoke PROPERTIES LABELS "android" TIMEOUT 900 SKIP_RETURN_CODE 77)
    endif()
endif()
