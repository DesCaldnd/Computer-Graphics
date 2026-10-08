# Глава 26: качество графики (docs/guide/26-quality-settings.md).
# CPU-примеры: группы и уровни, автоопределение (оценка → уровни, апскейлер), свой cvar в группе, настройки игрока.
ox_guide_example(quality_groups SOURCES scalability_groups.cpp auto_detect.cpp custom_cvar.cpp DEPS Oxwald::render)
ox_guide_example(quality_user_settings SOURCES user_settings.cpp DEPS Oxwald::render Oxwald::runtime)

# GPU-пример: короткий бенчмарк на headless-устройстве; без Vulkan тест пропускается.
ox_guide_example(quality_benchmark SOURCES auto_detect_gpu.cpp DEPS Oxwald::render LABELS gpu)
if(TARGET ox_guide_quality_benchmark AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_quality_benchmark)
endif()
