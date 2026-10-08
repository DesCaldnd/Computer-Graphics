# Installs the official prebuilt MoltenVK dylib + ICD manifest.
# The engine points VK_DRIVER_FILES at share/vulkan/icd.d/MoltenVK_icd.json at startup.
set(VCPKG_POLICY_SKIP_ARCHITECTURE_CHECK enabled)
set(VCPKG_POLICY_DLLS_WITHOUT_EXPORTS enabled)
set(VCPKG_POLICY_SKIP_COPYRIGHT_CHECK enabled)
set(VCPKG_POLICY_ALLOW_RESTRICTED_HEADERS enabled)

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/KhronosGroup/MoltenVK/releases/download/v${VERSION}/MoltenVK-macos.tar"
    FILENAME "MoltenVK-macos-${VERSION}.tar"
    SHA512 985f483e832ae3605b62b78798989f79c8833c13b568ff3615fb9c1f2780c3e9960cb13f79f2560b01563694249520d23d4f93d284580c13f6c842eff0c6b99d
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}" NO_REMOVE_ONE_LEVEL)

set(MVK "${SOURCE_PATH}/MoltenVK/MoltenVK/dynamic/dylib/macOS")
# MoltenVK-specific headers only (vulkan/*.h come from vulkan-headers). CMake's FindVulkan
# requires them for the MoltenVK component, which Qt's Vulkan support asks for on Apple.
file(INSTALL "${SOURCE_PATH}/MoltenVK/MoltenVK/include/MoltenVK" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${MVK}/libMoltenVK.dylib" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
file(INSTALL "${MVK}/libMoltenVK.dylib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")

# ICD manifest with a path relative to the json: share/vulkan/icd.d -> ../../../lib
file(WRITE "${CURRENT_PACKAGES_DIR}/share/vulkan/icd.d/MoltenVK_icd.json" [=[
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../../lib/libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
]=])
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/moltenvk-config.cmake" [=[
get_filename_component(_MVK_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(MOLTENVK_DYLIB "${_MVK_PREFIX}/lib/libMoltenVK.dylib")
set(MOLTENVK_ICD_JSON "${_MVK_PREFIX}/share/vulkan/icd.d/MoltenVK_icd.json")
set(moltenvk_FOUND TRUE)
]=])
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/MoltenVK/LICENSE")
