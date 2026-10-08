# Examples for docs/guide/16-world.md (CPU side of the world module; no GPU needed).
ox_guide_example(world SOURCES terrain.cpp lod_vegetation.cpp sky_weather_water.cpp streaming.cpp DEPS Oxwald::world)
ox_guide_example(world_physics SOURCES physics_bridge.cpp DEPS Oxwald::world Oxwald::physics)
