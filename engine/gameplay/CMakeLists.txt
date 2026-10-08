# ECS glue: components + systems binding the CPU modules (physics, animation, spline, audio, ai, net, script)
# to the scene World. All module dependencies are public because the component headers expose their types
# (enums, handles, settings structs).
set(_ox_gameplay_deps
    Oxwald::scene Oxwald::physics Oxwald::animation Oxwald::spline Oxwald::audio Oxwald::ai Oxwald::net
    Oxwald::script)
if(TARGET Oxwald::async)
    list(APPEND _ox_gameplay_deps Oxwald::async)
endif()

ox_add_module(gameplay
    PUBLIC_DEPS ${_ox_gameplay_deps})

if(TARGET Oxwald::async)
    target_compile_definitions(ox_gameplay PUBLIC OX_GAMEPLAY_HAS_ASYNC=1)
endif()

# Optional source sets (outside src/ so the module glob does not pick them up):
#   assets/  providers (IMeshColliderProvider, IPrefabProvider, ...) implemented on top of assets::AssetManager.
#            Direction: gameplay -> assets (the asset database knows nothing about gameplay; gameplay already owns
#            the provider interfaces). `assets` is configured before `gameplay` (alphabetical), so the target
#            exists here when enabled.
#   world/   ECS components + systems of the world module (terrain, vegetation, sky, time of day, water, wind,
#            buoyancy, streaming) and the WorldRenderData service. `world` is configured after `gameplay`, so the
#            check is deferred until the whole tree is configured.
if(TARGET ox_assets)
    file(GLOB_RECURSE _ox_gameplay_assets_src CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/assets/src/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/include/*.hpp)
    target_sources(ox_gameplay PRIVATE ${_ox_gameplay_assets_src})
    target_include_directories(ox_gameplay PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/assets/include>)
    target_link_libraries(ox_gameplay PUBLIC Oxwald::assets)
    target_compile_definitions(ox_gameplay PUBLIC OX_GAMEPLAY_HAS_ASSETS=1)
    if(TARGET ox_gameplay_tests)
        file(GLOB_RECURSE _ox_gameplay_assets_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/tests/*.cpp)
        target_sources(ox_gameplay_tests PRIVATE ${_ox_gameplay_assets_tests})
    endif()
endif()

function(ox_gameplay_link_world)
    if(NOT TARGET ox_world OR NOT TARGET ox_gameplay)
        return()
    endif()
    set(_dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    file(GLOB_RECURSE _src CONFIGURE_DEPENDS ${_dir}/world/src/*.cpp ${_dir}/world/src/*.hpp ${_dir}/world/include/*.hpp)
    if(NOT _src)
        return()
    endif()
    target_sources(ox_gameplay PRIVATE ${_src})
    target_include_directories(ox_gameplay PUBLIC $<BUILD_INTERFACE:${_dir}/world/include>
                                           PRIVATE ${_dir}/world/src)
    target_link_libraries(ox_gameplay PUBLIC ox_world)
    target_compile_definitions(ox_gameplay PUBLIC OX_GAMEPLAY_HAS_WORLD=1)
    if(TARGET ox_gameplay_tests)
        file(GLOB_RECURSE _tests CONFIGURE_DEPENDS ${_dir}/world/tests/*.cpp)
        target_sources(ox_gameplay_tests PRIVATE ${_tests})
        target_include_directories(ox_gameplay_tests PRIVATE ${_dir}/world/src ${_dir}/world/tests)
    endif()
    message(STATUS "Oxwald gameplay: world integration enabled")
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL ox_gameplay_link_world)
