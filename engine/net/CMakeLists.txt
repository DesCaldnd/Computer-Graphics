find_package(unofficial-enet CONFIG REQUIRED)

ox_add_module(net
    PUBLIC_DEPS  Oxwald::core
    PRIVATE_DEPS unofficial::enet::enet)
