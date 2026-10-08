# OxwaldShowcase tests (ctest label `showcase`):
#   Showcase.GeneratorCheck     the committed content equals what Tools/generate_showcase.cpp produces (idempotent)
#   Showcase.Scene.<name>       every scene runs 120 frames in the headless player without errors/validation messages
#   Showcase.Tour               the guided tour visits every station and reaches its end
#   ox_showcase_tests (gtest)   all Lua scripts compile; save/load round trip in the save station
set(_showcase_user ${CMAKE_CURRENT_BINARY_DIR}/user)
set(_showcase_fail "\\[error\\]|\\[fatal\\]|Validation Error|VUID-|assertion")

add_test(NAME Showcase.GeneratorCheck COMMAND oxshowcase_generate --project ${OX_SHOWCASE_DIR} --check)
set_tests_properties(Showcase.GeneratorCheck PROPERTIES LABELS "showcase" TIMEOUT 600)

if(TARGET OxwaldPlayer)
    set(_scenes MainMenu Hub)
    file(GLOB _stations RELATIVE ${OX_SHOWCASE_DIR}/Assets/Scenes/Stations ${OX_SHOWCASE_DIR}/Assets/Scenes/Stations/*.oxscene)
    list(SORT _stations)
    foreach(st ${_stations})
        string(REPLACE ".oxscene" "" st ${st})
        list(APPEND _scenes "Stations/${st}")
    endforeach()
    foreach(scene ${_scenes})
        get_filename_component(_name ${scene} NAME)
        add_test(NAME Showcase.Scene.${_name}
                 COMMAND OxwaldPlayer --project ${OX_SHOWCASE_DIR} --headless --frames 120 --quality high
                         --scene project://Assets/Scenes/${scene}.oxscene --user-dir ${_showcase_user}/${_name} -- --photo)
        set_tests_properties(Showcase.Scene.${_name} PROPERTIES LABELS "showcase;gpu" TIMEOUT 180
            PASS_REGULAR_EXPRESSION "frames=120" FAIL_REGULAR_EXPRESSION "${_showcase_fail}")
    endforeach()
    add_test(NAME Showcase.Tour
             COMMAND OxwaldPlayer --project ${OX_SHOWCASE_DIR} --headless --frames 60000 --quality medium
                     --user-dir ${_showcase_user}/tour -- --tour --quit)
    set_tests_properties(Showcase.Tour PROPERTIES LABELS "showcase;gpu" TIMEOUT 600
        PASS_REGULAR_EXPRESSION "Tour finished" FAIL_REGULAR_EXPRESSION "${_showcase_fail}")
endif()

add_executable(ox_showcase_tests showcase_tests.cpp)
target_link_libraries(ox_showcase_tests PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,ox_showcase_game>" GTest::gtest_main)
target_compile_definitions(ox_showcase_tests PRIVATE OX_SHOWCASE_DIR="${OX_SHOWCASE_DIR}")
ox_set_warnings(ox_showcase_tests)
set_target_properties(ox_showcase_tests PROPERTIES FOLDER "samples/showcase")
if(COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_showcase_tests)
endif()
gtest_discover_tests(ox_showcase_tests DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS "showcase" ENVIRONMENT "OXWALD_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
