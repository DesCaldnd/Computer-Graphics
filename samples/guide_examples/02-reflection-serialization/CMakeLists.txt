ox_guide_example(reflection_types SOURCES reflect_types.cpp DEPS Oxwald::core)
ox_guide_example(serialization SOURCES serialize.cpp versioning.cpp DEPS Oxwald::core)
ox_guide_example(reflection_component SOURCES component_scene.cpp DEPS Oxwald::scene)

# oxdump из командной строки: JSON -> OXB1 -> проверка CRC и обратимости -> снова JSON.
if(TARGET oxdump)
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/sword.oxitem)
    add_test(NAME GuideSerialization.oxdump_to_binary
             COMMAND oxdump --to-binary ${CMAKE_CURRENT_SOURCE_DIR}/sword.json ${_out})
    add_test(NAME GuideSerialization.oxdump_check COMMAND oxdump --check ${_out})
    add_test(NAME GuideSerialization.oxdump_info COMMAND oxdump --info ${_out})
    add_test(NAME GuideSerialization.oxdump_to_json COMMAND oxdump ${_out})
    set_tests_properties(GuideSerialization.oxdump_to_binary PROPERTIES
        LABELS guide FIXTURES_SETUP guide_oxdump_sword)
    set_tests_properties(GuideSerialization.oxdump_check GuideSerialization.oxdump_info
                         GuideSerialization.oxdump_to_json PROPERTIES
        LABELS guide FIXTURES_REQUIRED guide_oxdump_sword)
    set_tests_properties(GuideSerialization.oxdump_info PROPERTIES PASS_REGULAR_EXPRESSION "kind +item.*DATA.*crc ok")
    set_tests_properties(GuideSerialization.oxdump_to_json PROPERTIES
        PASS_REGULAR_EXPRESSION "\"name\": \"Sword of Testing\"")
endif()
