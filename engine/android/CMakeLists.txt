# android — Android platform layer (GameActivity): window/surface lifecycle, touch + gamepad + keyboard input into
# the InputSystem, logcat sink, crash handler, APK assets (AAssetManager) as a VFS source, ADPF performance/thermal
# hints, remote console over `adb forward`. Android only; the platform-agnostic logic it is built on (touch mapping,
# lifecycle state machine, key tables, bundle mounts, mobile quality profile) lives in runtime/core/render and is
# unit tested on the host.
if(NOT ANDROID)
    return()
endif()

# GameActivity (androidx.games:games-activity) arrives as a prefab package from Gradle (buildFeatures.prefab).
find_package(game-activity REQUIRED CONFIG)

file(GLOB_RECURSE _ox_android_src CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp)
add_library(ox_android STATIC ${_ox_android_src})
add_library(Oxwald::android ALIAS ox_android)
target_include_directories(ox_android PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# ox_runtime / ox_rhi_android are defined later (modules configure alphabetically); target names resolve at
# generate time.
target_link_libraries(ox_android
    PUBLIC  ox_runtime ox_rhi_android game-activity::game-activity_static
    PRIVATE android log)
target_compile_features(ox_android PUBLIC cxx_std_20)
ox_set_warnings(ox_android)
set_target_properties(ox_android PROPERTIES FOLDER "engine")

function(ox_android_link_optional)
    if(TARGET ox_audio)
        target_link_libraries(ox_android PRIVATE ox_audio)
        target_compile_definitions(ox_android PRIVATE OX_ANDROID_HAS_AUDIO=1)
    endif()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL ox_android_link_optional)
