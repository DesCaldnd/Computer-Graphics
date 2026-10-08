find_package(recastnavigation CONFIG REQUIRED)

ox_add_module(ai
    PUBLIC_DEPS  Oxwald::core
    PRIVATE_DEPS RecastNavigation::Recast
                 RecastNavigation::Detour
                 RecastNavigation::DetourCrowd
                 RecastNavigation::DetourTileCache)

