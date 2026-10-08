# Глава 22: объёмные эффекты (docs/guide/22-volumetrics.md).
# CPU-пример: компоненты, сетка фроксов, уровни качества — устройство не нужно.
ox_guide_example(volumetrics_cpu SOURCES volumetrics_cpu.cpp DEPS Oxwald::render)

# GPU-пример (обвязка — ../20-lighting-shadows/guide_render_scene.hpp): без Vulkan-устройства тесты пропускаются.
ox_guide_example(volumetrics_gpu SOURCES volumetrics_gpu.cpp DEPS Oxwald::render Oxwald::assets LABELS gpu)
if(TARGET ox_guide_volumetrics_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_volumetrics_gpu)
endif()
