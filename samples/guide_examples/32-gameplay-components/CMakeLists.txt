# Глава 32: компоненты ECS (docs/guide/32-gameplay-components.md). Lua-скрипты лежат в scripts/.
ox_guide_example(gameplay_systems SOURCES systems_modes.cpp  DEPS Oxwald::gameplay)
ox_guide_example(gameplay_lua     SOURCES lua_entity_api.cpp DEPS Oxwald::gameplay)
# Компоненты мира есть только в сборке gameplay с модулем world.
ox_guide_example(gameplay_world   SOURCES world_components.cpp DEPS Oxwald::gameplay Oxwald::world)
# Компоненты рендера: только CPU-часть (регистрация и сериализация), GPU не нужен.
ox_guide_example(gameplay_render_components SOURCES render_components.cpp DEPS Oxwald::render Oxwald::scene)
