# OxwaldShowcase — the sample project that walks through every engine feature.
#
#   Assets/            content (scenes, prefabs, materials, Lua, RmlUi, audio, generated meshes); committed
#   Source/            native game module "Showcase" (ox_showcase_game): Lua `showcase` API, guided tour, net demo
#   Tools/             oxshowcase_generate — rebuilds every scene/material/prefab through the engine API
#   Tests/             ctest label `showcase`
#
# The game module is linked into OxwaldPlayer and OxwaldEditor (WHOLE_ARCHIVE: it registers itself with
# OX_GAME_MODULE) and is enabled only for projects that list it: "modules": {"Showcase": true}.
set(_ox_showcase_deps ox_runtime ox_gameplay ox_render ox_ui ox_assets ox_net ox_async ox_script ox_world)
foreach(dep ${_ox_showcase_deps})
    if(NOT TARGET ${dep})
        message(STATUS "OxwaldShowcase: ${dep} is not configured — showcase targets skipped")
        return()
    endif()
endforeach()

set(OX_SHOWCASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ---- native game module -------------------------------------------------------------------------------------
file(GLOB _showcase_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.hpp)
add_library(ox_showcase_game STATIC ${_showcase_srcs})
target_link_libraries(ox_showcase_game PUBLIC ${_ox_showcase_deps})
target_include_directories(ox_showcase_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_compile_features(ox_showcase_game PUBLIC cxx_std_20)
ox_set_warnings(ox_showcase_game)
set_target_properties(ox_showcase_game PROPERTIES FOLDER "samples/showcase")

foreach(host OxwaldPlayer OxwaldEditor)
    if(TARGET ${host})
        target_link_libraries(${host} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,ox_showcase_game>")
    endif()
endforeach()

# ---- scene generator ----------------------------------------------------------------------------------------
file(GLOB _gen_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Tools/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Tools/*.hpp)
add_executable(oxshowcase_generate ${_gen_srcs})
target_link_libraries(oxshowcase_generate PRIVATE ox_runtime ox_gameplay ox_render ox_assets ox_world)
target_compile_definitions(oxshowcase_generate PRIVATE OX_SHOWCASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
ox_set_warnings(oxshowcase_generate)
set_target_properties(oxshowcase_generate PROPERTIES FOLDER "samples/showcase")
add_custom_target(oxshowcase_regenerate
    COMMAND oxshowcase_generate --project ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS oxshowcase_generate
    COMMENT "Regenerating OxwaldShowcase scenes, materials and prefabs"
    VERBATIM)

if(OX_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMakeLists.txt)
    add_subdirectory(Tests)
endif()
