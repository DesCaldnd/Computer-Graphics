# Глава 25: апскейлеры, сглаживание и постобработка (docs/guide/25-upscalers-postprocess.md).
# CPU: режимы качества, доступность, Auto-рекомендация, cvar'ы, объёмы постобработки — устройство не нужно.
ox_guide_example(upscalers SOURCES upscalers.cpp DEPS Oxwald::render)
ox_guide_example(postprocess_volumes SOURCES postprocess_volumes.cpp DEPS Oxwald::render)

# GPU: TAAU в половинном разрешении, откат DLSS → TAAU, bloom из объёма. Без Vulkan — GTEST_SKIP.
ox_guide_example(postprocess_gpu SOURCES postprocess_gpu.cpp DEPS Oxwald::render Oxwald::assets LABELS gpu)
if(TARGET ox_guide_postprocess_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_postprocess_gpu)
endif()
