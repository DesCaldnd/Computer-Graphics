find_package(EnTT CONFIG REQUIRED)

ox_add_module(scene
    PUBLIC_DEPS Oxwald::core EnTT::EnTT)
