# render — renderer, GPU scene, extract, features (clustered forward+, shadows, IBL, picking, debug draw, stats,
# quality settings). Configured before engine/rhi alphabetically, so pull in rhi's runtime helpers explicitly.
include(${OX_ENGINE_SOURCE_DIR}/engine/rhi/cmake/OxwaldVulkanRuntime.cmake)
find_package(Stb REQUIRED)
set(VOLK_PULL_IN_VULKAN OFF)
find_package(volk CONFIG REQUIRED)

set(_ox_render_public_deps Oxwald::rhi Oxwald::scene)
# The CPU asset structs (MeshData, MaterialAsset, TextureData) are consumed through their public headers only, so
# the renderer works while the assets module is still being implemented.
if(TARGET ox_assets)
    list(APPEND _ox_render_public_deps Oxwald::assets)
endif()

ox_add_module(render
    PUBLIC_DEPS  ${_ox_render_public_deps}
    PRIVATE_DEPS volk::volk)

target_compile_definitions(ox_render PRIVATE OX_RENDER_SHADER_DIR="${CMAKE_SOURCE_DIR}/engine/shaders")
if(TARGET ox_assets)
    target_compile_definitions(ox_render PUBLIC OX_RENDER_HAS_ASSETS=1)
else()
    # Without the assets library asset_provider.cpp (AssetManager glue) cannot link.
    get_target_property(_render_srcs ox_render SOURCES)
    list(FILTER _render_srcs EXCLUDE REGEX "asset_provider.cpp$")
    set_target_properties(ox_render PROPERTIES SOURCES "${_render_srcs}")
    # Header-only use of the asset structs even when the module is excluded via OX_MODULES.
    target_include_directories(ox_render PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/engine/assets/include>)
endif()

if(OX_BUILD_TESTS AND TARGET ox_render_tests)
    # tests/*.cpp -> ox_render_tests (CPU); tests/gpu/*.cpp -> ox_render_gpu_tests (labels render;gpu).
    get_target_property(_srcs ox_render_tests SOURCES)
    list(FILTER _srcs EXCLUDE REGEX "/tests/gpu/")
    set_target_properties(ox_render_tests PROPERTIES SOURCES "${_srcs}")

    file(GLOB _gpu_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/*.cpp)
    if(_gpu_tests)
        add_executable(ox_render_gpu_tests ${_gpu_tests})
        target_link_libraries(ox_render_gpu_tests PRIVATE ox_render GTest::gtest_main)
        target_include_directories(ox_render_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                   ${CMAKE_CURRENT_SOURCE_DIR}/tests ${Stb_INCLUDE_DIR})
        target_compile_definitions(ox_render_gpu_tests PRIVATE
            OX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
            OX_RENDER_SHADER_DIR="${CMAKE_SOURCE_DIR}/engine/shaders")
        ox_set_warnings(ox_render_gpu_tests)
        ox_add_gpu_test_env(ox_render_gpu_tests)
        gtest_discover_tests(ox_render_gpu_tests
            DISCOVERY_TIMEOUT 60
            PROPERTIES LABELS "render\;gpu"
                       ENVIRONMENT "OXWALD_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
        # `cmake --build --target ox_render_tests` builds both executables.
        add_dependencies(ox_render_tests ox_render_gpu_tests)
    endif()
endif()

# ox::IRenderer implementation for the runtime (engine/runtime is configured after render: resolve at the end).
function(ox_render_link_runtime)
    if(TARGET ox_runtime)
        target_sources(ox_render PRIVATE ${OX_ENGINE_SOURCE_DIR}/engine/render/runtime/runtime_renderer.cpp)
        target_link_libraries(ox_render PUBLIC ox_runtime)
        target_compile_definitions(ox_render PUBLIC OX_RENDER_HAS_RUNTIME=1)
        message(STATUS "Oxwald render: runtime IRenderer adapter enabled")
    endif()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL ox_render_link_runtime)
