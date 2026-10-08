# Глава 23: прозрачность, вода, частицы (docs/guide/23-transparency-water-particles.md).
# CPU: материалы .oxmat, компоненты, extract, волны Герстнера, cvar'ы — устройство не нужно.
ox_guide_example(translucency SOURCES translucency.cpp DEPS Oxwald::render Oxwald::assets)

# GPU: стекло (OIT/сортировка/преломление), вода, подводный режим, частицы. Без Vulkan — GTEST_SKIP.
ox_guide_example(translucency_gpu SOURCES translucency_gpu.cpp DEPS Oxwald::render Oxwald::assets LABELS gpu)
if(TARGET ox_guide_translucency_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_translucency_gpu)
endif()
