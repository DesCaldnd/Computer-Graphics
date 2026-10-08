find_package(unofficial-lua CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)

# sol2 appears in public headers (bindApi builders take sol::table), so it is a public dependency.
ox_add_module(script
    PUBLIC_DEPS Oxwald::core lua sol2::sol2)

target_compile_definitions(ox_script PUBLIC SOL_ALL_SAFETIES_ON=1 SOL_PRINT_ERRORS=0)

# Optional bridge to the async module (ox::Future <-> Lua `await`, C++ co_await of script functions).
# `async` is configured before `script` (alphabetical), so the target already exists when enabled.
if(TARGET ox_async)
    target_sources(ox_script PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/async/async_bridge.cpp)
    target_link_libraries(ox_script PUBLIC Oxwald::async)
    target_compile_definitions(ox_script PUBLIC OX_SCRIPT_HAS_ASYNC=1)
    if(TARGET ox_script_tests)
        target_sources(ox_script_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/async/async_bridge_tests.cpp)
    endif()
endif()
