# ECS glue: components + systems binding the CPU modules (physics, animation, spline, audio, ai, net, script)
# to the scene World. All module dependencies are public because the component headers expose their types
# (enums, handles, settings structs).
set(_ox_gameplay_deps
    Oxwald::scene Oxwald::physics Oxwald::animation Oxwald::spline Oxwald::audio Oxwald::ai Oxwald::net
    Oxwald::script)
if(TARGET Oxwald::async)
    list(APPEND _ox_gameplay_deps Oxwald::async)
endif()

ox_add_module(gameplay
    PUBLIC_DEPS ${_ox_gameplay_deps})

if(TARGET Oxwald::async)
    target_compile_definitions(ox_gameplay PUBLIC OX_GAMEPLAY_HAS_ASYNC=1)
endif()
