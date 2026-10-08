ox_guide_example(cvars SOURCES cvars.cpp scalability.cpp DEPS Oxwald::core)
ox_guide_example(cvars_console_settings SOURCES console.cpp settings.cpp DEPS Oxwald::runtime)
ox_guide_example(cvars_lua SOURCES lua_cvars.cpp DEPS Oxwald::script)
