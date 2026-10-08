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
    else()
        message(STATUS "Oxwald: Tracy package not found - profiling macros are no-ops")
    endif()
endif()
set(OX_TRACY_ACTIVE ${OX_TRACY_ACTIVE} PARENT_SCOPE)

