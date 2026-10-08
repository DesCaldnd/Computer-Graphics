find_package(unofficial-lua CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)

# sol2 appears in public headers (bindApi builders take sol::table), so it is a public dependency.
ox_add_module(script
    PUBLIC_DEPS Oxwald::core lua sol2::sol2)

target_compile_definitions(ox_script PUBLIC SOL_ALL_SAFETIES_ON=1 SOL_PRINT_ERRORS=0)
