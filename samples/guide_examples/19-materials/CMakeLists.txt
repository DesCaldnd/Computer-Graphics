# Глава 19: материалы (docs/guide/19-materials.md).
# CPU: формат .oxmat (файлы в materials/), MaterialAsset в коде, проект с AssetRegistry/AssetManager.
ox_guide_example(materials_cpu SOURCES materials_cpu.cpp DEPS Oxwald::assets Oxwald::scene)

# GPU: те же материалы в рендере. Фикстура общая с главой 18 (../18-rendering-overview/guide_render_fixture.hpp).
ox_guide_example(materials_gpu SOURCES materials_gpu.cpp DEPS Oxwald::render Oxwald::assets LABELS gpu)
if(TARGET ox_guide_materials_gpu AND COMMAND ox_add_gpu_test_env)
    ox_add_gpu_test_env(ox_guide_materials_gpu)
endif()
