find_package(assimp CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(tinyexr CONFIG REQUIRED)
find_package(zstd CONFIG QUIET)

set(_ox_assets_private_deps
    Oxwald::scene assimp::assimp fastgltf::fastgltf meshoptimizer::meshoptimizer KTX::ktx
    unofficial::tinyexr::tinyexr)
set(_ox_assets_public_deps Oxwald::core)
# Skeletons/clips are imported through the animation module when it is configured (OX_MODULES).
if(TARGET ox_animation)
    list(APPEND _ox_assets_public_deps Oxwald::animation)
endif()
if(TARGET zstd::libzstd)
    list(APPEND _ox_assets_private_deps zstd::libzstd)
elseif(TARGET zstd::libzstd_static)
    list(APPEND _ox_assets_private_deps zstd::libzstd_static)
endif()

ox_add_module(assets
    PUBLIC_DEPS  ${_ox_assets_public_deps}
    PRIVATE_DEPS ${_ox_assets_private_deps}
    TEST_DEPS    Oxwald::scene)

target_include_directories(ox_assets SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
if(TARGET ox_animation)
    target_compile_definitions(ox_assets PUBLIC OX_ASSETS_HAS_ANIMATION=1)
endif()
if(TARGET zstd::libzstd OR TARGET zstd::libzstd_static)
    target_compile_definitions(ox_assets PRIVATE OX_ASSETS_HAS_ZSTD=1)
endif()

if(TARGET ox_assets_tests)
    target_link_libraries(ox_assets_tests PRIVATE KTX::ktx)
    target_compile_definitions(ox_assets_tests PRIVATE
        OX_LEGACY_ASSETS_DIR="${CMAKE_SOURCE_DIR}/samples/OxwaldShowcase/Assets/Legacy")
endif()
