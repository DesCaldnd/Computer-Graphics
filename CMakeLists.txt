cmake_minimum_required(VERSION 3.25)

# On macOS all vcpkg dependencies are built with AppleClang (see vcpkg-overlays/triplets/arm64-osx.cmake).
# Use the same compiler and libc++ for the engine, even if the shell exports another toolchain
# (e.g. Homebrew LLVM in CC/CXX/LDFLAGS). Opt out with -DOX_USE_APPLE_CLANG=OFF.
option(OX_USE_APPLE_CLANG "Force AppleClang on macOS to match vcpkg-built dependencies" ON)
if(CMAKE_HOST_APPLE AND OX_USE_APPLE_CLANG AND NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_C_COMPILER /usr/bin/clang)
    set(CMAKE_CXX_COMPILER /usr/bin/clang++)
    set(CMAKE_OBJC_COMPILER /usr/bin/clang)
    set(CMAKE_OBJCXX_COMPILER /usr/bin/clang++)
    foreach(var CPPFLAGS CFLAGS CXXFLAGS LDFLAGS)
        if("$ENV{${var}}" MATCHES "homebrew/opt/llvm")
            set(ENV{${var}} "")
        endif()
    endforeach()
endif()

# Android (Gradle passes ANDROID_ABI per ABI): vcpkg.cmake does not derive Android triplets itself.
if(ANDROID_ABI AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(VCPKG_TARGET_TRIPLET arm64-android CACHE STRING "vcpkg triplet")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(VCPKG_TARGET_TRIPLET x64-android CACHE STRING "vcpkg triplet")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(VCPKG_TARGET_TRIPLET arm-neon-android CACHE STRING "vcpkg triplet")
    elseif(ANDROID_ABI STREQUAL "x86")
        set(VCPKG_TARGET_TRIPLET x86-android CACHE STRING "vcpkg triplet")
    endif()
endif()
# One vcpkg install root per Android triplet (<OX_VCPKG_ANDROID_ROOT>/<triplet>/<triplet>): a manifest-mode install
# keeps only the triplet it installs, so in a shared root arm64-android would remove x64-android
# (tools/android/vcpkg-install.ps1 installs each triplet into its own root; Gradle passes OX_VCPKG_ANDROID_ROOT).
if(ANDROID_ABI AND OX_VCPKG_ANDROID_ROOT AND VCPKG_TARGET_TRIPLET)
    set(VCPKG_INSTALLED_DIR "${OX_VCPKG_ANDROID_ROOT}/${VCPKG_TARGET_TRIPLET}" CACHE PATH "vcpkg install root" FORCE)
endif()

project(OxwaldEngine VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(OX_BUILD_TESTS  "Build unit/integration tests" ON)
option(OX_BUILD_EDITOR "Build the Qt editor" ON)
option(OX_ENABLE_TRACY "Enable Tracy profiler zones" ON)
option(OX_ENABLE_DLSS  "Build the NVIDIA DLSS upscaler backend where the SDK runtime exists" ON)
option(OX_WARNINGS_AS_ERRORS "Treat warnings as errors in engine code" OFF)
# Optional allow-list of modules/top-level dirs to configure, e.g. -DOX_MODULES="core;rhi".
# Empty = everything. Lets several people/agents build their part while others are mid-edit.
set(OX_MODULES "" CACHE STRING "Restrict configured engine modules and top-level dirs (editor, apps, tools, samples)")

# Android (Gradle project apps/android, toolchain = vcpkg chained to the NDK): only the runtime player is built, as
# liboxwald_player.so. No Qt editor, no host tools, no host tests, no DLSS (NGX is desktop only).
if(ANDROID)
    set(OX_BUILD_EDITOR OFF CACHE BOOL "Build the Qt editor" FORCE)
    set(OX_BUILD_TESTS OFF CACHE BOOL "Build unit/integration tests" FORCE)
    set(OX_ENABLE_DLSS OFF CACHE BOOL "Build the NVIDIA DLSS upscaler backend where the SDK runtime exists" FORCE)
    # address = ASan (x86_64 emulator / arm64, needs wrap.sh + the runtime in the APK: Gradle -PoxSanitizer=address),
    # hwaddress = HWASan (arm64 only, Android 14+ devices such as the OnePlus 12).
    set(OX_ANDROID_SANITIZER "" CACHE STRING "Android sanitizer: '', address, hwaddress")
    if(OX_ANDROID_SANITIZER)
        add_compile_options(-fsanitize=${OX_ANDROID_SANITIZER} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${OX_ANDROID_SANITIZER})
        message(STATUS "Oxwald: Android sanitizer ${OX_ANDROID_SANITIZER}")
    endif()
    # Remote console port compiled in as the default (0 = off unless --remote-console <port>).
    set(OX_ANDROID_REMOTE_CONSOLE_PORT "0" CACHE STRING "Default remote console port of the Android player")
    # Gradle runs ninja without -j: cap compile/link parallelism (shared build machine, link steps are memory heavy).
    set(OX_ANDROID_JOBS "4" CACHE STRING "Parallel compile jobs of the Android build")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS ox_compile=${OX_ANDROID_JOBS} ox_link=2)
    set(CMAKE_JOB_POOL_COMPILE ox_compile)
    set(CMAKE_JOB_POOL_LINK ox_link)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(OxwaldHelpers)

# Static libraries reach executables through several dependency paths (engine modules depend on each other and on
# the same vcpkg archives, some imported through two spellings of the same path), and CMake keeps static libraries
# repeated on the link line so single-pass linkers resolve back references. Apple's ld (Xcode 15+) warns about every
# repeated archive although the result is identical; silence just that warning.
if(APPLE)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries" OX_LINKER_HAS_NO_WARN_DUPLICATE_LIBRARIES)
    if(OX_LINKER_HAS_NO_WARN_DUPLICATE_LIBRARIES)
        add_link_options("LINKER:-no_warn_duplicate_libraries")
    endif()
endif()

if(OX_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
endif()

add_subdirectory(engine)
foreach(dir editor apps tools samples)
    if(OX_MODULES AND NOT dir IN_LIST OX_MODULES)
        continue()
    endif()
    if(ANDROID AND dir MATCHES "^(editor|tools)$")
        continue()
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt)
        add_subdirectory(${dir})
    endif()
endforeach()
