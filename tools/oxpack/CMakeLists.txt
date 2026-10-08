if(NOT TARGET ox_assets)
    return()
endif()

add_executable(oxpack main.cpp)
target_link_libraries(oxpack PRIVATE Oxwald::assets)
ox_set_warnings(oxpack)
set_target_properties(oxpack PROPERTIES FOLDER "tools")

if(OX_BUILD_TESTS)
    add_test(NAME oxpack.smoke
             COMMAND ${CMAKE_COMMAND} -DOXPACK=$<TARGET_FILE:oxpack>
                     -DLEGACY=${CMAKE_SOURCE_DIR}/samples/OxwaldShowcase/Assets/Legacy
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.cmake)
    set_tests_properties(oxpack.smoke PROPERTIES LABELS "tools;assets")
    add_test(NAME oxpack.missing_project COMMAND oxpack ${CMAKE_CURRENT_BINARY_DIR}/does-not-exist -o x.oxpak)
    set_tests_properties(oxpack.missing_project PROPERTIES LABELS "tools;assets" WILL_FAIL TRUE)
endif()
