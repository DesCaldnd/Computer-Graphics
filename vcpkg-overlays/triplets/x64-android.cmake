# OxwaldEngine overlay of the community x64-android triplet (Android emulator, x86_64): release libraries only.
# The Android player is built RelWithDebInfo/Release (Gradle); Debug native builds link these release libraries
# too, which halves the dependency build time on the shared build machine.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 28)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=x86_64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=x86_64)
set(VCPKG_BUILD_TYPE release)
