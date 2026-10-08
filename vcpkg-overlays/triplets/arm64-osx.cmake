# Same as vcpkg's arm64-osx, but always builds with AppleClang + the macOS SDK,
# regardless of CC/CXX/LDFLAGS in the user's shell (e.g. Homebrew LLVM), so that every
# dependency and the engine itself share one compiler and one libc++.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/apple-clang-toolchain.cmake")
