# Compilable versions of the code snippets from the user guide (docs/guide/*.md).
# One subdirectory per chapter; each calls ox_guide_example(). Examples whose engine modules are not
# configured (see OX_MODULES) are skipped. All tests carry the ctest label `guide`.
#
#   cmake --build <build> --target ox_guide_examples
#   ctest --test-dir <build> -L guide
if(NOT OX_BUILD_TESTS)
    return()
endif()

add_custom_target(ox_guide_examples)
set(OX_GUIDE_EXAMPLES_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ox_guide_example(<name> SOURCES files... DEPS targets... [LABELS extra-labels...])
# Builds ox_guide_<name> (GoogleTest, gtest_main) and registers its tests with label `guide`.
# OX_GUIDE_DIR (compile definition) points to the chapter directory (for .lua and data files).
function(ox_guide_example name)
    cmake_parse_arguments(G "" "" "SOURCES;DEPS;LABELS" ${ARGN})
    foreach(dep ${G_DEPS})
        if(NOT TARGET ${dep})
            message(STATUS "Guide example '${name}' skipped: ${dep} is not configured")
            return()
        endif()
    endforeach()
    add_executable(ox_guide_${name} ${G_SOURCES})
    target_link_libraries(ox_guide_${name} PRIVATE ${G_DEPS} GTest::gtest_main)
    target_compile_definitions(ox_guide_${name} PRIVATE OX_GUIDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    ox_set_warnings(ox_guide_${name})
    set_target_properties(ox_guide_${name} PROPERTIES FOLDER "samples/guide")
    add_dependencies(ox_guide_examples ox_guide_${name})
    set(_labels guide ${G_LABELS})
    string(REPLACE ";" "\\;" _labels "${_labels}")
    gtest_discover_tests(ox_guide_${name}
        DISCOVERY_TIMEOUT 60
        PROPERTIES LABELS "${_labels}"
                   ENVIRONMENT "OXWALD_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
endfunction()

file(GLOB _ox_guide_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _ox_guide_dirs)
foreach(dir ${_ox_guide_dirs})
    if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/CMakeLists.txt)
        add_subdirectory(${dir})
    endif()
endforeach()
