# ui — in-game debug UI / profiler (Dear ImGui) and game UI (RmlUi), both drawn by one rhi render feature at the
# renderer's Overlay injection point (own renderer: no imgui Vulkan backend, no RmlUi GL backend).
# Needs render + runtime (Engine integration, InputSystem events, Console, Settings). script / async / gameplay are
# optional (Lua bindings, coroutine list, debug-draw toggles). Configured after render/runtime/script alphabetically.
if(NOT TARGET ox_render OR NOT TARGET ox_runtime)
    message(WARNING "Oxwald ui: needs the render and runtime modules (add them to OX_MODULES); skipping")
    return()
endif()

include(${OX_ENGINE_SOURCE_DIR}/engine/rhi/cmake/OxwaldVulkanRuntime.cmake)
find_package(glfw3 CONFIG REQUIRED)   # imgui::imgui links the imported `glfw` target (glfw binding is built in)
find_package(imgui CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)
find_package(Stb REQUIRED)

ox_add_module(ui
    PUBLIC_DEPS  Oxwald::render Oxwald::runtime imgui::imgui RmlUi::Core
    PRIVATE_DEPS RmlUi::Debugger)

target_include_directories(ox_ui PRIVATE ${Stb_INCLUDE_DIR})
target_compile_definitions(ox_ui PRIVATE OX_UI_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")

foreach(_m script async gameplay)
    if(TARGET ox_${_m})
        string(TOUPPER ${_m} _M)
        target_link_libraries(ox_ui PUBLIC ox_${_m})
        target_compile_definitions(ox_ui PUBLIC OX_UI_HAS_${_M}=1)
    endif()
endforeach()

if(OX_BUILD_TESTS AND TARGET ox_ui_tests)
    # tests/*.cpp -> ox_ui_tests (CPU, label ui); tests/gpu/*.cpp -> ox_ui_gpu_tests (labels ui;gpu).
    get_target_property(_srcs ox_ui_tests SOURCES)
    list(FILTER _srcs EXCLUDE REGEX "/tests/gpu/")
    set_target_properties(ox_ui_tests PROPERTIES SOURCES "${_srcs}")

    file(GLOB _gpu_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/*.cpp)
    if(_gpu_tests)
        # The render module's offscreen fixture (device, renderer, golden comparison) is reused as is.
        set(_fixture ${OX_ENGINE_SOURCE_DIR}/engine/render/tests/gpu/render_fixture.cpp)
        add_executable(ox_ui_gpu_tests ${_gpu_tests} ${_fixture})
        target_link_libraries(ox_ui_gpu_tests PRIVATE ox_ui GTest::gtest_main)
        target_include_directories(ox_ui_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                   ${CMAKE_CURRENT_SOURCE_DIR}/tests
                                   ${OX_ENGINE_SOURCE_DIR}/engine/render/tests/gpu ${Stb_INCLUDE_DIR})
        target_compile_definitions(ox_ui_gpu_tests PRIVATE
            OX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
            OX_RENDER_SHADER_DIR="${CMAKE_SOURCE_DIR}/engine/shaders")
        ox_set_warnings(ox_ui_gpu_tests)
        ox_add_gpu_test_env(ox_ui_gpu_tests)
        gtest_discover_tests(ox_ui_gpu_tests
            DISCOVERY_TIMEOUT 60
            PROPERTIES LABELS "ui\;gpu"
                       ENVIRONMENT "OXWALD_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
        add_dependencies(ox_ui_tests ox_ui_gpu_tests)
    endif()
endif()
