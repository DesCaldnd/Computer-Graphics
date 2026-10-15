# Only the ffx-api headers and AMD's prebuilt, signed Vulkan DLL of FidelityFX SDK v1.1.4 (the whole SDK archive is
# ~470 MB and building the DLL from source needs the SDK's shader compiler toolchain). Files are fetched one by one
# from the tagged commit.
set(VCPKG_POLICY_SKIP_ARCHITECTURE_CHECK enabled)
set(VCPKG_POLICY_DLLS_WITHOUT_EXPORTS enabled)

set(_commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55) # tag v1.1.4
set(_base "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${_commit}")
set(_files
    "LICENSE.txt|8ef8a7384ffc3d38a51e20bdc85da339c551fe73bd4c85b91628442a88e6b215363786336932fa3bf4b68670fcfae7422808cd9393ed386955237e5a0db677c1"
    "ffx-api/include/ffx_api/ffx_api.h|dee6f99e5ae8a019d4a7ee5d45454c6ab5f464746682f346976e0ed2f63e8afd1d07c49a34bb6adfb9360e61369a98277bd3fecc10ebe87904d7609877b92670"
    "ffx-api/include/ffx_api/ffx_api.hpp|9d5a009ac2f928e5a880b93202c97a612808a59a771c729ab248310ea50e4d09bf0a3d9c935b2e49aacdb8d0eea5b136060d2e8b28e006337e9deba562e51fbe"
    "ffx-api/include/ffx_api/ffx_api_loader.h|867be80a90cd0ab455a67703c43682adb8cec52d3b794e44f8e168b7db07652b9ccc4c4cdcd1b533a6afafaa04e7dc0e2db1355af08e294d736d0b3a62386f74"
    "ffx-api/include/ffx_api/ffx_api_types.h|6bddf5bd35faedf70f5418e8cb79efe329e7960638d9f7e65f541aaa59bb6e7b1e425f40228eba2305439ea03875b0c283fbe41233db85cc176fca52bd14b769"
    "ffx-api/include/ffx_api/ffx_framegeneration.h|b1187ef36dbdf3a9a7aae923664155ad66211ad81b9bc610e941b2ed6461cde9762dd5fdce3ca437dfec36b313b3453f7bdaa814a0296d04d14be0cd8a19e16c"
    "ffx-api/include/ffx_api/ffx_framegeneration.hpp|820dc9be615241274624d6f953b4f4a2cc331932a379ae4280e08611f4d6d60bb026d5e4ff30cefb4d3c31de1c4d20e3bf8461632702453885212a37e401b1bf"
    "ffx-api/include/ffx_api/ffx_upscale.h|7203e3261b5fecf406917264c1a193d5c221da408870e68dd9f5a3f41954255a89dcc618ef0b002259345e6d371e639ce7c61ea087c620a8cb467a4836982af6"
    "ffx-api/include/ffx_api/ffx_upscale.hpp|9eec9697e06a69fe0ee74f8c39c7cb2f7ac673266a7332aa24c62467a011da0cca1de1f4f7105d5fa9334964782ce497afa1686e34bda918dbac59e40f5537db"
    "ffx-api/include/ffx_api/vk/ffx_api_vk.h|4a9cb28a369ffead2c559d6bac4b4513579338ea14ce7cefa19d84c1e719bf10460b7bf500493d9aacaa7cf2f6c36afa99b6443669894c0268e1ff1841709a82"
    "ffx-api/include/ffx_api/vk/ffx_api_vk.hpp|7ddc60bffca1c3d7cd6b63acb11bb3dabb1657fab56e208ab4d2f08e81f8aaec19e6b8677e317de87a40955dcb82eb5cb9847bf0d9c7b54645b6ea20b78decb4"
    "PrebuiltSignedDLL/amd_fidelityfx_vk.dll|ecb2c551894447ec463abcd62a400f6e08467f5e559074465bcfbcd9a67c513d3d212e4e96ef3bce4fde0f8664b31ffadba5183b628adcfe3a2120edfd2dcb23"
    "PrebuiltSignedDLL/amd_fidelityfx_vk.lib|bdeecf919149fa19b973f21c5de8a71f52ff66c3af286e824837c152ac0b59e8fd8145f78562f8d9b6887e97e0ab92fd87a629266645636595ee42fd84bf5665"
)
foreach(_entry IN LISTS _files)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _path)
    list(GET _parts 1 _sha)
    string(REPLACE "/" "-" _name "${_path}")
    vcpkg_download_distfile(_file URLS "${_base}/${_path}" FILENAME "fidelityfx-${VERSION}-${_name}" SHA512 ${_sha})
    if(_path MATCHES "^ffx-api/include/(.*)$")
        get_filename_component(_dir "${CURRENT_PACKAGES_DIR}/include/${CMAKE_MATCH_1}" DIRECTORY)
        get_filename_component(_fname "${_path}" NAME)
        file(INSTALL "${_file}" DESTINATION "${_dir}" RENAME "${_fname}")
    elseif(_path MATCHES "\.dll$")
        file(INSTALL "${_file}" DESTINATION "${CURRENT_PACKAGES_DIR}/bin" RENAME amd_fidelityfx_vk.dll)
        file(INSTALL "${_file}" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin" RENAME amd_fidelityfx_vk.dll)
    elseif(_path MATCHES "\.lib$")
        file(INSTALL "${_file}" DESTINATION "${CURRENT_PACKAGES_DIR}/lib" RENAME amd_fidelityfx_vk.lib)
        file(INSTALL "${_file}" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib" RENAME amd_fidelityfx_vk.lib)
    else()
        set(_license "${_file}")
    endif()
endforeach()

configure_file("${CMAKE_CURRENT_LIST_DIR}/amd-fidelityfx-vk-config.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/amd-fidelityfx-vk-config.cmake" @ONLY)
vcpkg_install_copyright(FILE_LIST "${_license}")
