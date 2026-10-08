# Глава 28: рендеринг мира — ландшафт, растительность, небо, compute-скиннинг (docs/guide/28-world-rendering.md).
# CPU: cvar'ы по группам, компоненты рендера, WorldSnapshot, троттлинг IBL неба.
ox_guide_example(world_render_cpu SOURCES world_render_cpu.cpp DEPS Oxwald::render Oxwald::world)
# CPU: компоненты мира на сцене → WorldRenderData → extract рендера (мост gameplay → render).
ox_guide_example(world_render_components SOURCES world_components.cpp DEPS Oxwald::render Oxwald::gameplay Oxwald::world)

# GPU: ландшафт + растительность + небо, скиннинг (без Vulkan — GTEST_SKIP).
ox_guide_example(world_render_gpu SOURCES world_gpu.cpp DEPS Oxwald::render Oxwald::world Oxwald::scene LABELS gpu)
if(TARGET ox_guide_world_render_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_world_render_gpu)
endif()
