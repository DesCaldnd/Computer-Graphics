# Глава 20: освещение и тени (docs/guide/20-lighting-shadows.md).
# CPU-пример: компоненты света, экспозиция, кластеры, каскады, атлас теней, cvar'ы — устройство не нужно.
ox_guide_example(lighting_cpu SOURCES lighting_cpu.cpp DEPS Oxwald::render)

# GPU-пример: без Vulkan-устройства тесты пропускаются (GTEST_SKIP).
# guide_render_scene.hpp — общая обвязка headless-рендера для глав 20–22.
ox_guide_example(lighting_gpu SOURCES lighting_gpu.cpp DEPS Oxwald::render Oxwald::assets LABELS gpu)
if(TARGET ox_guide_lighting_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_lighting_gpu)
endif()
