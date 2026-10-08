# OxwaldPlayer — standalone game runtime (no editor).
if(NOT TARGET ox_runtime)
    message(STATUS "Oxwald: runtime module not configured, skipping OxwaldPlayer")
    return()
endif()

add_executable(OxwaldPlayer main.cpp)
target_link_libraries(OxwaldPlayer PRIVATE Oxwald::runtime)
ox_set_warnings(OxwaldPlayer)
set_target_properties(OxwaldPlayer PROPERTIES FOLDER "apps")

# The render module (when configured) provides the Vulkan IRenderer. engine/ is configured before apps/,
# so its targets already exist here (custom commands must be added in this directory).
if(TARGET ox_render)
    target_link_libraries(OxwaldPlayer PRIVATE ox_render)
    target_compile_definitions(OxwaldPlayer PRIVATE OX_HAS_RENDER=1)
endif()
if(COMMAND ox_add_gpu_test_env AND TARGET ox_rhi)
    ox_add_gpu_test_env(OxwaldPlayer)
endif()

if(OX_BUILD_TESTS)
    add_test(NAME OxwaldPlayer.HeadlessSmoke
             COMMAND OxwaldPlayer --headless --frames 10 --user-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke_user
                     --quality low --cvar t.MaxFPS=0)
    add_test(NAME OxwaldPlayer.ServerSmoke
             COMMAND OxwaldPlayer --server --frames 5 --fps 240 --user-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke_user)
    add_test(NAME OxwaldPlayer.BadArgument COMMAND OxwaldPlayer --bogus)
    set_tests_properties(OxwaldPlayer.HeadlessSmoke OxwaldPlayer.ServerSmoke PROPERTIES
        LABELS "runtime;player" TIMEOUT 60 ENVIRONMENT "OXWALD_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        PASS_REGULAR_EXPRESSION "frames=10|frames=5")
    set_tests_properties(OxwaldPlayer.BadArgument PROPERTIES LABELS "runtime;player" WILL_FAIL TRUE)
endif()
