include_guard(GLOBAL)

set(OX_ENGINE_SOURCE_DIR ${CMAKE_SOURCE_DIR} CACHE INTERNAL "")
set(OX_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/engine/shaders CACHE INTERNAL "")

# --- Platform-wide compile settings (this file is included from the root CMakeLists.txt, so they reach every target).
if(WIN32)
    # NOMINMAX / WIN32_LEAN_AND_MEAN: <windows.h> arrives through third-party headers (enet, GLFW native, Tracy) and
    # its min/max macros break std::min/std::max and glm. Defined empty ("NAME=") on purpose: identical to a plain
    # `#define NOMINMAX` in a source file, so such a line is not a C4005 macro redefinition.
    # _CRT_SECURE_NO_WARNINGS / _CRT_NONSTDC_NO_WARNINGS: portable code uses std::getenv, fopen, strncpy, ...;
    # MSVC flags each as C4996, which is fatal with OX_WARNINGS_AS_ERRORS.
    add_compile_definitions(NOMINMAX= WIN32_LEAN_AND_MEAN= _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    # gtest_discover_tests() runs the test executable. With the x64-windows triplet every executable needs its vcpkg
    # DLLs next to it (copied by vcpkg's applocal POST_BUILD step) and a single executable that fails to start would
    # fail the whole build, so discover when ctest runs instead of at build time.
    if(NOT DEFINED CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE)
        set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
    endif()
endif()
if(MSVC)
    # /utf-8            sources and string literals are UTF-8 (also for targets that skip ox_set_warnings)
    # /bigobj           sol2 / EnTT / reflection-heavy translation units exceed the default COFF section limit
    # /Zc:preprocessor  conforming preprocessor: the engine's variadic macros use __VA_OPT__ and `, ##__VA_ARGS__`
    add_compile_options(/utf-8 /bigobj /Zc:preprocessor)
endif()

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
        # Keep /W4 comparable to the -Wall -Wextra set used with clang/gcc (which has no -Wconversion / -Wshadow and
        # switches unused parameters off):
        #   C4100 unused parameter                      C4127 conditional expression is constant
        #   C4201 nameless struct/union                 C4324 structure padded due to alignas
        #   C4244 C4267 C4305 narrowing conversions     C4245 signed constant converted to unsigned
        #   C4456-C4459 declaration hides an outer one
        target_compile_options(${target} PRIVATE /wd4100 /wd4127 /wd4201 /wd4324 /wd4244 /wd4267 /wd4305 /wd4245
                                                 /wd4456 /wd4457 /wd4458 /wd4459)
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
