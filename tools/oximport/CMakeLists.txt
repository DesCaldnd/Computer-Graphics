if(NOT TARGET ox_assets)
    return()
endif()

add_executable(oximport main.cpp)
target_link_libraries(oximport PRIVATE Oxwald::assets)
ox_set_warnings(oximport)
set_target_properties(oximport PROPERTIES FOLDER "tools")

if(OX_BUILD_TESTS)
    set(_legacy ${CMAKE_SOURCE_DIR}/samples/OxwaldShowcase/Assets/Legacy)
    add_test(NAME oximport.mesh COMMAND oximport ${_legacy}/sphere.obj)
    set_tests_properties(oximport.mesh PROPERTIES LABELS "tools;assets"
        PASS_REGULAR_EXPRESSION "Mesh.*triangles")
    add_test(NAME oximport.texture COMMAND oximport ${_legacy}/fiit.jpg --settings "{\"maxSize\": 64}")
    set_tests_properties(oximport.texture PROPERTIES LABELS "tools;assets"
        PASS_REGULAR_EXPRESSION "BC7_SRGB")
    add_test(NAME oximport.rejects_unknown COMMAND oximport ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
    set_tests_properties(oximport.rejects_unknown PROPERTIES LABELS "tools;assets" WILL_FAIL TRUE)
endif()
