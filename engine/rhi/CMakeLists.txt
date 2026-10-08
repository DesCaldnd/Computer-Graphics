# rhi — Vulkan device, resources, bindless, pipelines, shader compiler/hot reload, render graph.
# Everything goes through volk (VK_NO_PROTOTYPES); the loader is dlopen'ed at runtime, so nothing here links
# Vulkan::Vulkan.
set(VOLK_PULL_IN_VULKAN OFF)
find_package(volk CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(unofficial-shaderc CONFIG REQUIRED)
find_package(unofficial-spirv-reflect CONFIG REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/OxwaldVulkanRuntime.cmake)

ox_add_module(rhi
    PUBLIC_DEPS  Oxwald::core volk::volk_headers Vulkan::Headers
    PRIVATE_DEPS volk::volk GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap
                 unofficial::shaderc::shaderc unofficial::spirv-reflect)

target_compile_definitions(ox_rhi
    PUBLIC  VK_NO_PROTOTYPES
    PRIVATE OX_RHI_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/engine/shaders"
            OX_RHI_VALIDATION_DEFAULT=$<IF:$<CONFIG:Release>,0,1>
            OX_RHI_SHADER_OPTIMIZE=$<IF:$<CONFIG:Debug>,0,1>
            OX_RHI_SHADER_DEBUG_INFO=$<IF:$<CONFIG:Release>,0,1>)

# Dev fallback when the executable has no vulkan/ folder next to it (e.g. tools run from anywhere).
set(_ox_vk_dev_dir ${CMAKE_BINARY_DIR}/vulkan-dev)
ox_deploy_vulkan_runtime_to(${_ox_vk_dev_dir})
if(EXISTS ${_ox_vk_dev_dir}/${OX_VK_RUNTIME_LOADER_NAME})
    target_compile_definitions(ox_rhi PRIVATE OX_RHI_DEV_LOADER_LIB="${_ox_vk_dev_dir}/${OX_VK_RUNTIME_LOADER_NAME}")
endif()
if(EXISTS ${_ox_vk_dev_dir}/MoltenVK_icd.json)
    target_compile_definitions(ox_rhi PRIVATE OX_RHI_DEV_ICD_JSON="${_ox_vk_dev_dir}/MoltenVK_icd.json")
endif()
if(EXISTS ${_ox_vk_dev_dir}/VkLayer_khronos_validation.json)
    target_compile_definitions(ox_rhi PRIVATE OX_RHI_DEV_LAYER_DIR="${_ox_vk_dev_dir}")
endif()

if(OX_ENABLE_TRACY)
    find_package(Tracy CONFIG QUIET)
    if(TARGET Tracy::TracyClient)
        target_link_libraries(ox_rhi PRIVATE Tracy::TracyClient)
        target_compile_definitions(ox_rhi PRIVATE OX_RHI_TRACY=1)
    endif()
endif()

# GLFW surface provider: separate target so the core rhi library stays window-system agnostic.
find_package(glfw3 CONFIG QUIET)
if(TARGET glfw)
    add_library(ox_rhi_glfw STATIC glfw/src/glfw_surface.cpp glfw/include/oxwald/rhi/glfw_surface.hpp)
    add_library(Oxwald::rhi_glfw ALIAS ox_rhi_glfw)
    target_include_directories(ox_rhi_glfw PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/glfw/include>)
    target_link_libraries(ox_rhi_glfw PUBLIC ox_rhi glfw)
    target_compile_definitions(ox_rhi_glfw PUBLIC GLFW_INCLUDE_NONE)
    ox_set_warnings(ox_rhi_glfw)
    set_target_properties(ox_rhi_glfw PROPERTIES FOLDER "engine")
endif()

if(OX_BUILD_TESTS AND TARGET ox_rhi_tests)
    # tests/*.cpp -> ox_rhi_tests (CPU only); tests/gpu/*.cpp -> ox_rhi_gpu_tests (label gpu, needs a device).
    get_target_property(_srcs ox_rhi_tests SOURCES)
    list(FILTER _srcs EXCLUDE REGEX "/tests/gpu/")
    set_target_properties(ox_rhi_tests PROPERTIES SOURCES "${_srcs}")
    target_compile_definitions(ox_rhi_tests PRIVATE OX_RHI_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/engine/shaders")

    file(GLOB _gpu_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/*.cpp)
    if(_gpu_tests)
        add_executable(ox_rhi_gpu_tests ${_gpu_tests})
        target_link_libraries(ox_rhi_gpu_tests PRIVATE ox_rhi GTest::gtest_main)
        target_include_directories(ox_rhi_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_compile_definitions(ox_rhi_gpu_tests PRIVATE OX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
        ox_set_warnings(ox_rhi_gpu_tests)
        ox_add_gpu_test_env(ox_rhi_gpu_tests)
        gtest_discover_tests(ox_rhi_gpu_tests
            DISCOVERY_TIMEOUT 60
            PROPERTIES LABELS "rhi\;gpu"
                       ENVIRONMENT "OXWALD_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
    endif()
endif()
