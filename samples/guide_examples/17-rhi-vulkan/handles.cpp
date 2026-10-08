// Глава 17: generational handles, bindless-индексы, Access -> барьеры, таблица форматов
// (docs/guide/17-rhi-vulkan.md). Только CPU, Vulkan-устройство не нужно.
#include <oxwald/rhi/access.hpp>
#include <oxwald/rhi/format.hpp>
#include <oxwald/rhi/handles.hpp>

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

using namespace ox;
using namespace ox::rhi;

TEST(GuideRhiHandles, StaleHandleNeverResolves) {
    // Тот же пул, что Device использует внутри для буферов/текстур/пайплайнов.
    HandlePool<std::string, TextureTag> pool;

    TextureHandle albedo = pool.allocate("albedo");
    ASSERT_TRUE(albedo); // generation != 0
    EXPECT_EQ(*pool.get(albedo), "albedo");

    pool.release(albedo);                            // как Device::destroy(texture)
    TextureHandle normal = pool.allocate("normal");  // слот переиспользован...
    EXPECT_EQ(normal.index, albedo.index);
    EXPECT_NE(normal.generation, albedo.generation); // ...но поколение другое

    EXPECT_EQ(pool.get(albedo), nullptr) << "старый handle не видит новый объект";
    EXPECT_FALSE(pool.release(albedo).has_value()) << "повторное освобождение безопасно";

    TextureHandle null; // handle по умолчанию — «нулевой»
    EXPECT_FALSE(null.valid());

    // Handles можно класть в хэш-контейнеры.
    std::unordered_set<TextureHandle> set{normal};
    EXPECT_TRUE(set.contains(normal));
}

TEST(GuideRhiHandles, BindlessIndexAllocator) {
    // Так устроены bindless-слоты: индексы 0..5 сэмплеров заняты DefaultSampler.
    IndexAllocator samplers(/*capacity*/ 8, /*reservedLow*/ 6);
    EXPECT_EQ(samplers.allocate(), 6u);
    EXPECT_EQ(samplers.allocate(), 7u);
    EXPECT_EQ(samplers.allocate(), kInvalidBindlessIndex) << "место кончилось";
    samplers.free(6);
    EXPECT_EQ(samplers.allocate(), 6u) << "освобождённый индекс переиспользуется";
}

TEST(GuideRhiHandles, AccessMapsToSync2Barrier) {
    // Access описывает «как используется ресурс»; RHI сам выводит stage/access/layout.
    AccessInfo sampled = accessInfo(Access::SampledFragment);
    EXPECT_EQ(sampled.stages, VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT));
    EXPECT_EQ(sampled.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_FALSE(sampled.write);

    AccessInfo storage = accessInfo(Access::StorageWriteCompute);
    EXPECT_EQ(storage.layout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_TRUE(isWrite(Access::StorageWriteCompute));

    EXPECT_EQ(accessInfo(Access::Present).layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

TEST(GuideRhiHandles, FormatTable) {
    EXPECT_TRUE(formatInfo(VK_FORMAT_D32_SFLOAT).depth);
    EXPECT_TRUE(formatInfo(VK_FORMAT_BC7_SRGB_BLOCK).compressed);
    EXPECT_EQ(formatInfo(VK_FORMAT_BC7_SRGB_BLOCK).blockWidth, 4u);
    // Размер одного mip-уровня в байтах (для загрузки данных).
    EXPECT_EQ(mipLevelSize(VK_FORMAT_R8G8B8A8_UNORM, 256, 256, 1, /*mip*/ 1), 128u * 128u * 4u);
    EXPECT_EQ(fullMipCount(1024, 512), 11u);
}
