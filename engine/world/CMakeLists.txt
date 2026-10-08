find_package(Stb REQUIRED)
find_package(tinyexr CONFIG REQUIRED)

# The world module is ECS-agnostic and does not link physics: terrain/vegetation produce plain
# collider data (PhysicsHeightfieldTile, VegetationCollider). <oxwald/world/physics_bridge.hpp> is a
# header-only adapter to ox::physics::ShapeDesc for consumers that link Oxwald::physics themselves.
ox_add_module(world
    PUBLIC_DEPS  Oxwald::core
    PRIVATE_DEPS unofficial::tinyexr::tinyexr)
target_include_directories(ox_world PRIVATE ${Stb_INCLUDE_DIR})

if(TARGET ox_world_tests)
    target_compile_definitions(ox_world_tests PRIVATE OX_WORLD_SHADER_DIR="${OX_SHADER_SOURCE_DIR}")
    if(TARGET Oxwald::physics)
        target_link_libraries(ox_world_tests PRIVATE Oxwald::physics)
        target_compile_definitions(ox_world_tests PRIVATE OX_WORLD_TEST_PHYSICS=1)
    endif()
    find_package(unofficial-shaderc CONFIG QUIET)
    if(TARGET unofficial::shaderc::shaderc)
        target_link_libraries(ox_world_tests PRIVATE unofficial::shaderc::shaderc)
        target_compile_definitions(ox_world_tests PRIVATE OX_HAS_SHADERC=1)
    endif()
endif()
