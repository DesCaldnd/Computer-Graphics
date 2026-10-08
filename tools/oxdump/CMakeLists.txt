if(NOT TARGET ox_core)
    return()
endif()

add_executable(oxdump main.cpp)
target_link_libraries(oxdump PRIVATE Oxwald::core)
ox_set_warnings(oxdump)
set_target_properties(oxdump PROPERTIES FOLDER "tools")

# CLI tests on a scene written by the scene module's tests (needs ox_scene_tests).
if(OX_BUILD_TESTS AND TARGET ox_scene_tests)
    set(_sample ${CMAKE_CURRENT_BINARY_DIR}/sample.oxscene)
    add_test(NAME oxdump.write_sample
             COMMAND ox_scene_tests --gtest_filter=SceneSerialization.WriteSampleScene)
    set_tests_properties(oxdump.write_sample PROPERTIES
        ENVIRONMENT "OX_SAMPLE_SCENE_OUT=${_sample}" FIXTURES_SETUP oxdump_sample LABELS "tools;core")

    add_test(NAME oxdump.to_json COMMAND oxdump ${_sample})
    set_tests_properties(oxdump.to_json PROPERTIES FIXTURES_REQUIRED oxdump_sample LABELS "tools;core"
        PASS_REGULAR_EXPRESSION "\"kind\": \"scene\".*\"Light\"")

    add_test(NAME oxdump.info COMMAND oxdump --info ${_sample})
    set_tests_properties(oxdump.info PROPERTIES FIXTURES_REQUIRED oxdump_sample LABELS "tools;core"
        PASS_REGULAR_EXPRESSION "DATA.*crc ok")

    add_test(NAME oxdump.check COMMAND oxdump --check ${_sample})
    set_tests_properties(oxdump.check PROPERTIES FIXTURES_REQUIRED oxdump_sample LABELS "tools;core")

    add_test(NAME oxdump.cli_roundtrip
             COMMAND ${CMAKE_COMMAND} -DOXDUMP=$<TARGET_FILE:oxdump> -DINPUT=${_sample}
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/roundtrip -P ${CMAKE_CURRENT_SOURCE_DIR}/roundtrip_test.cmake)
    set_tests_properties(oxdump.cli_roundtrip PROPERTIES FIXTURES_REQUIRED oxdump_sample LABELS "tools;core")

    add_test(NAME oxdump.rejects_garbage COMMAND oxdump ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
    set_tests_properties(oxdump.rejects_garbage PROPERTIES WILL_FAIL TRUE LABELS "tools;core")
endif()
