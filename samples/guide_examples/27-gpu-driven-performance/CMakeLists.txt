# Глава 27: GPU-driven рендеринг и производительность (docs/guide/27-gpu-driven-performance.md).
# CPU: выбор LOD по экранной ошибке, выбор очереди async compute.
ox_guide_example(gpu_driven_cpu SOURCES lod_and_queues.cpp DEPS Oxwald::render)

# GPU: отсечение, Hi-Z, RenderStats, стриминг мипов, параллельная запись (без Vulkan — GTEST_SKIP).
ox_guide_example(gpu_driven_gpu SOURCES gpu_driven.cpp DEPS Oxwald::render Oxwald::scene LABELS gpu)
if(TARGET ox_guide_gpu_driven_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_gpu_driven_gpu)
endif()
