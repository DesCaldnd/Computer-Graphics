include_guard(GLOBAL)

set(OX_ENGINE_SOURCE_DIR ${CMAKE_SOURCE_DIR} CACHE INTERNAL "")
set(OX_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/engine/shaders CACHE INTERNAL "")

# OX_WARNINGS_AS_ERRORS applies to engine modules, editor, apps and tools. Code under samples/ (guide examples, the
# showcase project) still gets the warnings but they stay non-fatal: samples are written against the engine by other
# teams and must not break an engine build over a new warning.
function(ox_set_warnings target)
    set(_werror OFF)
    if(OX_WARNINGS_AS_ERRORS AND NOT CMAKE_CURRENT_SOURCE_DIR MATCHES "/samples(/|$)")
        set(_werror ON)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(_werror)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
        if(_werror)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ox_add_module(<name>
#     [PUBLIC_DEPS  targets...]   # e.g. Oxwald::core glm::glm
#     [PRIVATE_DEPS targets...]
#     [TEST_DEPS    targets...]   # extra deps for the module's tests
#     [TEST_LABELS  labels...])   # e.g. gpu — for tests needing a Vulkan device
#
# Layout:  include/oxwald/<name>/*.hpp  — public headers
#          src/**.cpp|hpp|mm             — implementation
#          tests/*.cpp                   — GoogleTest sources → ox_<name>_tests
# Creates  ox_<name> (static library) and alias Oxwald::<name>.
function(ox_add_module name)
    cmake_parse_arguments(M "" "" "PUBLIC_DEPS;PRIVATE_DEPS;TEST_DEPS;TEST_LABELS;EXTRA_SOURCES" ${ARGN})
    file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp)
    if(APPLE)
        file(GLOB_RECURSE _mm CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm)
        list(APPEND _srcs ${_mm})
    endif()
    add_library(ox_${name} STATIC ${_srcs} ${M_EXTRA_SOURCES})
    add_library(Oxwald::${name} ALIAS ox_${name})
    target_include_directories(ox_${name}
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ox_${name} PUBLIC ${M_PUBLIC_DEPS} PRIVATE ${M_PRIVATE_DEPS})
    target_compile_features(ox_${name} PUBLIC cxx_std_20)
    ox_set_warnings(ox_${name})
    set_target_properties(ox_${name} PROPERTIES FOLDER "engine")

    if(OX_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        file(GLOB_RECURSE _tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
        if(_tests)
            add_executable(ox_${name}_tests ${_tests})
            target_link_libraries(ox_${name}_tests PRIVATE ox_${name} ${M_TEST_DEPS} GTest::gtest_main)
            target_include_directories(ox_${name}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
            target_compile_definitions(ox_${name}_tests PRIVATE
                OX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
            ox_set_warnings(ox_${name}_tests)
            set(_labels ${name} ${M_TEST_LABELS})
            string(REPLACE ";" "\\;" _labels "${_labels}")
            gtest_discover_tests(ox_${name}_tests
                DISCOVERY_TIMEOUT 60
                PROPERTIES LABELS "${_labels}"
                           ENVIRONMENT "OXWALD_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
        endif()
    endif()
endfunction()

# ox_add_gpu_test_env(<target>) — runtime env (MoltenVK ICD, validation layers) for executables
# that create a Vulkan device. Implemented by the rhi module (see engine/rhi/CMakeLists.txt).
