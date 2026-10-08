# Глава 17: RHI и Vulkan (docs/guide/17-rhi-vulkan.md).
# CPU-примеры: handles, render graph planner, компилятор шейдеров — устройство не нужно.
ox_guide_example(rhi_handles SOURCES handles.cpp DEPS Oxwald::rhi)
ox_guide_example(rhi_render_graph SOURCES render_graph.cpp DEPS Oxwald::rhi)
ox_guide_example(rhi_shader_compiler SOURCES shader_compiler.cpp DEPS Oxwald::rhi)

# GPU-пример: без Vulkan-устройства тесты пропускаются (GTEST_SKIP).
ox_guide_example(rhi_device SOURCES device.cpp DEPS Oxwald::rhi LABELS gpu)
# ox_add_gpu_test_env копирует loader + MoltenVK + validation layer в <exe dir>/vulkan (POST_BUILD), поэтому
# работает и с gtest_discover_tests: окружение не нужно, rhi находит runtime рядом с исполняемым файлом.
if(TARGET ox_guide_rhi_device AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_rhi_device)
endif()
