find_package(unofficial-enet CONFIG REQUIRED)

ox_add_module(net
    PUBLIC_DEPS  Oxwald::core
    PRIVATE_DEPS unofficial::enet::enet)

# The vcpkg enet target carries no link interface: on Windows the static enet.lib needs Winsock (sockets) and
# winmm (timeGetTime / timeBeginPeriod).
if(WIN32)
    target_link_libraries(ox_net PRIVATE ws2_32 winmm)
endif()
