find_package(glm CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(enkiTS CONFIG REQUIRED)

ox_add_module(core
    PUBLIC_DEPS glm::glm nlohmann_json::nlohmann_json
    PRIVATE_DEPS enkiTS::enkiTS)

target_compile_definitions(ox_core PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL
    OX_ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Tracy is optional: zones compile to no-ops when the package is missing or the option is OFF.
set(OX_TRACY_ACTIVE OFF)
if(OX_ENABLE_TRACY)
    find_package(Tracy CONFIG QUIET)
    if(TARGET Tracy::TracyClient)
        target_link_libraries(ox_core PUBLIC Tracy::TracyClient)
        target_compile_definitions(ox_core PUBLIC OX_TRACY=1 TRACY_ENABLE)
        set(OX_TRACY_ACTIVE ON)
        # vcpkg installs the debug TracyClient.dll under debug/bin/Debug, where its applocal deployment does not
        # look: copy the DLL of the active configuration next to the executables.
        get_target_property(_tracy_type Tracy::TracyClient TYPE)
        if(WIN32 AND _tracy_type STREQUAL "SHARED_LIBRARY" AND CMAKE_RUNTIME_OUTPUT_DIRECTORY)
            add_custom_command(TARGET ox_core POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Tracy::TracyClient>
                        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
                VERBATIM)
        endif()
    else()
        message(STATUS "Oxwald: Tracy package not found - profiling macros are no-ops")
    endif()
endif()
set(OX_TRACY_ACTIVE ${OX_TRACY_ACTIVE} PARENT_SCOPE)

