// Глава 27: выбор LOD по экранной ошибке (общая функция CPU и шейдера отсечения) и выбор очереди async compute.
#include <oxwald/render/features/gpu_driven/gpu_driven.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/rhi/device_caps.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace ox;
using namespace ox::render;

TEST(GpuDrivenCpu, ScreenSpaceErrorLod) {
    // Цепочка LOD: ошибка упрощения (в единицах меша) растёт с уровнем.
    GpuMeshLod lods[3];
    lods[0].indexCount = 3000;
    lods[1].indexCount = 1500;
    lods[1].error = 0.01f;
    lods[2].indexCount = 600;
    lods[2].error = 0.04f;

    LodSelection sel;
    const f32 heightPx = 1080.0f, fovY = glm::radians(60.0f);
    sel.projScale = 0.5f * heightPx / std::tan(fovY * 0.5f); // пикселей на метр на расстоянии 1 м
    sel.thresholdPixels = 1.0f;                              // r.GpuDriven.LODErrorPixels · 2^LODBias

    // LOD l выбирается, пока error · scale · projScale / distance ≤ threshold.
    const f32 d1 = lods[1].error * sel.projScale / sel.thresholdPixels; // ≈ 9.4 м
    EXPECT_EQ(selectLod(lods, 3, 1.0f, d1 * 0.9f, sel), 0u);
    EXPECT_EQ(selectLod(lods, 3, 1.0f, d1 * 1.1f, sel), 1u);
    EXPECT_EQ(selectLod(lods, 3, 1.0f, 100.0f, sel), 2u);
    EXPECT_EQ(selectLod(lods, 3, 2.0f, d1 * 1.1f, sel), 0u); // вдвое крупнее экземпляр — позже переключается

    sel.thresholdPixels = 2.0f; // Low: r.GpuDriven.LODErrorPixels 2 → переход вдвое ближе
    EXPECT_EQ(selectLod(lods, 3, 1.0f, d1 * 0.6f, sel), 1u);
}

TEST(GpuDrivenCpu, AsyncComputeQueueChoice) {
    static FeatureRegistry features;
    registerGpuDrivenFeatures(features); // регистрирует r.AsyncCompute (Renderer::create делает это сам)
    rhi::DeviceCaps desktop;
    desktop.asyncComputeQueue = true;
    rhi::DeviceCaps moltenVk = desktop;
    moltenVk.portabilitySubset = true;

    auto& reg = CVarRegistry::instance();
    reg.set("r.AsyncCompute", "Auto", CVarSource::Code);
    EXPECT_EQ(asyncComputeHint(desktop), rhi::QueueType::Compute);
    EXPECT_EQ(asyncComputeHint(moltenVk), rhi::QueueType::Graphics); // Metal всё равно сериализует очереди
    reg.set("r.AsyncCompute", "On", CVarSource::Code);
    EXPECT_EQ(asyncComputeHint(moltenVk), rhi::QueueType::Compute);
    EXPECT_EQ(asyncComputeHint(rhi::DeviceCaps{}), rhi::QueueType::Graphics); // нет отдельной очереди
    reg.set("r.AsyncCompute", "Auto", CVarSource::Code);
}
