# Глава 21: отражения и GI (docs/guide/21-reflections-gi.md).
# CPU-пример: компоненты, извлечение, файлы .oxcube/.oxirr, уровни качества — устройство не нужно.
ox_guide_example(reflections_cpu SOURCES reflections_cpu.cpp DEPS Oxwald::render)

# GPU-пример (обвязка — ../20-lighting-shadows/guide_render_scene.hpp): без Vulkan-устройства тесты пропускаются.
ox_guide_example(reflections_gpu SOURCES reflections_gpu.cpp DEPS Oxwald::render Oxwald::assets LABELS gpu)
if(TARGET ox_guide_reflections_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_reflections_gpu)
endif()
