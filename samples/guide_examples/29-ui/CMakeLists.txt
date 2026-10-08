# Глава 29: UI — отладочный оверлей ImGui и игровой UI на RmlUi (docs/guide/29-ui.md).
# Документы .rml/.rcss лежат в ui/, Lua-скрипт — в scripts/. CPU-примеры: окно и Vulkan-устройство не нужны.
ox_guide_example(ui_imgui SOURCES imgui_overlay.cpp DEPS Oxwald::ui)
ox_guide_example(ui_game  SOURCES game_ui.cpp       DEPS Oxwald::ui)
# Lua-таблицы `ui` и `debug` есть, только если ui собран вместе со script (OX_UI_HAS_SCRIPT).
ox_guide_example(ui_lua   SOURCES lua_ui.cpp        DEPS Oxwald::ui Oxwald::script)

# GPU-пример: UiModule + withUi(render::createRenderer()) на headless-движке. Без Vulkan тест пропускается.
ox_guide_example(ui_engine SOURCES engine_integration.cpp DEPS Oxwald::ui LABELS gpu)
if(TARGET ox_guide_ui_engine AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_ui_engine)
endif()
