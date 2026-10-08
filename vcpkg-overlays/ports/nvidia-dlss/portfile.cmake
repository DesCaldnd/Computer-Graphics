# DLSS runtime exists only for Windows and Linux (x64/aarch64) on NVIDIA RTX GPUs.
# On other platforms only headers are installed so the engine's DLSS backend can be
# syntax-checked; at runtime the engine reports DLSS as unsupported.
set(VCPKG_POLICY_SKIP_COPYRIGHT_CHECK enabled)
set(VCPKG_POLICY_DLLS_WITHOUT_EXPORTS enabled)
set(VCPKG_POLICY_SKIP_ARCHITECTURE_CHECK enabled)
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
set(VCPKG_POLICY_MISMATCHED_NUMBER_OF_BINARIES enabled)
set(VCPKG_POLICY_ALLOW_RESTRICTED_HEADERS enabled)

set(DLSS_HAS_RUNTIME OFF)
if(VCPKG_TARGET_IS_WINDOWS OR VCPKG_TARGET_IS_LINUX)
    set(DLSS_HAS_RUNTIME ON)
    # Binaries are stored in Git LFS, so a git checkout with LFS is required. REF is the commit of tag v${VERSION};
    # no FETCH_REF: `git lfs fetch <url> <tag>` fails because the fetched tag is not a local ref, a SHA resolves.
    vcpkg_from_git(OUT_SOURCE_PATH SOURCE_PATH
        URL https://github.com/NVIDIA/DLSS.git
        REF 374959484e79a640feaba44c93ac8cfb0a03f5b5
        # Explicit LFS remote: a value-less `LFS` keyword only works with CMake >= 3.31 (CMP0174); older CMake drops
        # it and the checkout would silently contain LFS pointer files instead of the libraries.
        LFS https://github.com/NVIDIA/DLSS.git)
else()
    vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH
        REPO NVIDIA/DLSS
        REF "v${VERSION}"
        SHA512 41d878d6296500f07b498e00be5bdebd0c55af6e5ba7c777e2964217c4c185debc947573aade7bf164c30c5807abc5a922a59843fad9e39892383b725c038f0a
        HEAD_REF main)
endif()

file(INSTALL "${SOURCE_PATH}/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include/nvidia-dlss")

if(DLSS_HAS_RUNTIME)
    if(VCPKG_TARGET_IS_WINDOWS)
        file(INSTALL "${SOURCE_PATH}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
        file(INSTALL "${SOURCE_PATH}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
        file(GLOB _dlls "${SOURCE_PATH}/lib/Windows_x86_64/rel/*.dll")
        file(INSTALL ${_dlls} DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
        file(GLOB _dlls_dev "${SOURCE_PATH}/lib/Windows_x86_64/dev/*.dll")
        file(INSTALL ${_dlls_dev} DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
    else()
        if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
            set(_dir "${SOURCE_PATH}/lib/Linux_aarch64")
        else()
            set(_dir "${SOURCE_PATH}/lib/Linux_x86_64")
        endif()
        file(INSTALL "${_dir}/libnvsdk_ngx.a" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
        file(INSTALL "${_dir}/libnvsdk_ngx.a" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
        file(GLOB _so "${_dir}/rel/*.so.*")
        file(INSTALL ${_so} DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
        file(GLOB _so_dev "${_dir}/dev/*.so.*")
        file(INSTALL ${_so_dev} DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
    endif()
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/nvidia-dlss-config.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/nvidia-dlss-config.cmake" @ONLY)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
