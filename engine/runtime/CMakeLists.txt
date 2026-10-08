# runtime — Engine (services, game loop, render thread), input, platform (GLFW), projects & settings,
# save games, console. Depends on core + scene; every other module is optional and linked when its target
# exists (OX_HAS_<MODULE>=1), so runtime builds with whatever subset of the engine is configured.
ox_add_module(runtime
    PUBLIC_DEPS Oxwald::core Oxwald::scene)

find_package(Threads REQUIRED)
target_link_libraries(ox_runtime PUBLIC Threads::Threads)

# GLFW player platform (window, events, gamepads). Optional: headless builds compile without it.
find_package(glfw3 CONFIG QUIET)
if(TARGET glfw)
    target_link_libraries(ox_runtime PRIVATE glfw)
    target_compile_definitions(ox_runtime PUBLIC OX_HAS_GLFW=1 PRIVATE GLFW_INCLUDE_NONE)
endif()

# render/ui are not linked here: they implement runtime interfaces (IRenderer) and depend on runtime; apps link
# them. Modules are configured in alphabetical order, so script/spline/world/... do not exist yet when this file
# runs. Resolve the optional links once the whole tree has been configured.
function(ox_runtime_link_optional_modules)
    foreach(m physics audio script ai net animation spline world assets async gameplay)
        if(TARGET ox_${m})
            string(TOUPPER ${m} _M)
            target_link_libraries(ox_runtime PUBLIC ox_${m})
            target_compile_definitions(ox_runtime PUBLIC OX_HAS_${_M}=1)
            message(STATUS "Oxwald runtime: integrating optional module '${m}'")
        endif()
    endforeach()
    # Vulkan surface glue for GLFW windows lives in rhi (ox_rhi_glfw).
    if(TARGET ox_rhi_glfw AND TARGET glfw)
        target_link_libraries(ox_runtime PUBLIC ox_rhi_glfw)
        target_compile_definitions(ox_runtime PUBLIC OX_HAS_RHI=1 OX_HAS_RHI_GLFW=1)
    elseif(TARGET ox_rhi)
        target_link_libraries(ox_runtime PUBLIC ox_rhi)
        target_compile_definitions(ox_runtime PUBLIC OX_HAS_RHI=1)
    endif()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL ox_runtime_link_optional_modules)
