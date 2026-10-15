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
    # Neural texture compression of loose images (small and short: the full report is in docs/dev/perf.md).
    add_test(NAME oximport.ntc COMMAND oximport --ntc --albedo ${_legacy}/beige_leather_5_diffuse.png
             --normal ${_legacy}/beige_leather_5_normal.png
             --settings "{\"neuralPreset\": \"Low\", \"neuralSteps\": 150, \"neuralMaxSize\": 128, \"neuralFallbackSize\": 32}")
    set_tests_properties(oximport.ntc PROPERTIES LABELS "tools;assets" PASS_REGULAR_EXPRESSION "NeuralTextureSet 128x128")
    add_test(NAME oximport.rejects_unknown COMMAND oximport ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
    set_tests_properties(oximport.rejects_unknown PROPERTIES LABELS "tools;assets" WILL_FAIL TRUE)
endif()
