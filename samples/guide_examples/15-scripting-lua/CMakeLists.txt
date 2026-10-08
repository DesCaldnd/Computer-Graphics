# Глава 15: скрипты на Lua (docs/guide/15-scripting-lua.md). Скрипты лежат в scripts/.
ox_guide_example(lua_vm         SOURCES vm_basics.cpp      DEPS Oxwald::script)
ox_guide_example(lua_instance   SOURCES instance.cpp       DEPS Oxwald::script)
ox_guide_example(lua_hot_reload SOURCES hot_reload.cpp     DEPS Oxwald::script)
# Привязки сервисов runtime (таблица `input`) — нужен runtime, собранный со script.
ox_guide_example(lua_input      SOURCES input_bindings.cpp DEPS Oxwald::runtime Oxwald::script)
