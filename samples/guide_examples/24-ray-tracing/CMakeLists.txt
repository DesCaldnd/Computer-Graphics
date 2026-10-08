# Глава 24: трассировка лучей (docs/guide/24-ray-tracing.md).
# CPU: доступность и причины, гейтинг, RtSettings/Scalability, LOD BLAS, маски, накопление — RT-железо не нужно.
ox_guide_example(rt_settings SOURCES rt_settings.cpp DEPS Oxwald::render)

# GPU: галочка r.RayTracing на настоящем устройстве. На Mac (MoltenVK без RT) проверяет инертность галочки,
# на RTX-машине — что сцена TLAS строится. Без Vulkan — GTEST_SKIP.
ox_guide_example(rt_gpu SOURCES rt_gpu.cpp DEPS Oxwald::render LABELS gpu)
if(TARGET ox_guide_rt_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_rt_gpu)
endif()
