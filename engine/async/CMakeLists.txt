ox_add_module(async
    PUBLIC_DEPS Oxwald::core)

# Optional request/response RPC helper on top of the net module (header-only, separate target so `async` itself
# never depends on net). Modules are configured alphabetically, so test for the directory instead of the target.
if(EXISTS ${CMAKE_SOURCE_DIR}/engine/net/CMakeLists.txt AND (NOT OX_MODULES OR "net" IN_LIST OX_MODULES))
    add_library(ox_async_net INTERFACE)
    add_library(Oxwald::async_net ALIAS ox_async_net)
    target_include_directories(ox_async_net INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/net/include)
    target_link_libraries(ox_async_net INTERFACE ox_async ox_net)
    if(OX_BUILD_TESTS)
        add_executable(ox_async_net_tests ${CMAKE_CURRENT_SOURCE_DIR}/net/tests/rpc_call_tests.cpp)
        target_link_libraries(ox_async_net_tests PRIVATE ox_async_net GTest::gtest_main)
        target_include_directories(ox_async_net_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/net/tests)
        ox_set_warnings(ox_async_net_tests)
        gtest_discover_tests(ox_async_net_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS "async;net")
    endif()
endif()
