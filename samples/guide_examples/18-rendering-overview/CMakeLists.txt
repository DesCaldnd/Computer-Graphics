# Глава 18: устройство рендерера (docs/guide/18-rendering-overview.md).
# CPU: реестр фич, ресурсы, настройки, extract — устройство не нужно.
ox_guide_example(render_overview_cpu SOURCES frame_cpu.cpp DEPS Oxwald::render)

# GPU: своя фича с шейдером из shaders/guide/, debug views, hot reload, EditorViewportRenderer.
# Без Vulkan-устройства тесты пропускаются (GTEST_SKIP). guide_render_fixture.hpp использует и глава 19.
ox_guide_example(render_overview_gpu SOURCES custom_feature.cpp guide_render_fixture.hpp DEPS Oxwald::render LABELS gpu)
if(TARGET ox_guide_render_overview_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_render_overview_gpu)
endif()

# Рендерер как IRenderer движка (createRenderer(), как в OxwaldPlayer).
ox_guide_example(render_overview_runtime SOURCES runtime_renderer.cpp DEPS Oxwald::render Oxwald::runtime LABELS gpu)
if(TARGET ox_guide_render_overview_runtime AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_render_overview_runtime)
endif()
