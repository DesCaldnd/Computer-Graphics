#include <oxwald/rhi/access.hpp>
#include <oxwald/rhi/format.hpp>
#include <oxwald/rhi/handles.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace ox;
using namespace ox::rhi;

TEST(HandlePool, AllocateGetRelease) {
    HandlePool<std::string, BufferTag> pool;
    BufferHandle a = pool.allocate("a");
    BufferHandle b = pool.allocate("b");
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    EXPECT_NE(a, b);
    EXPECT_EQ(*pool.get(a), "a");
    EXPECT_EQ(*pool.get(b), "b");
    EXPECT_EQ(pool.size(), 2u);

    auto released = pool.release(a);
    ASSERT_TRUE(released.has_value());
    EXPECT_EQ(*released, "a");
    EXPECT_EQ(pool.get(a), nullptr);
    EXPECT_FALSE(pool.release(a).has_value()) << "double release must be a no-op";
    EXPECT_EQ(pool.size(), 1u);
}

TEST(HandlePool, StaleHandleNeverResolvesToReusedSlot) {
    HandlePool<int, TextureTag> pool;
    TextureHandle a = pool.allocate(1);
    pool.release(a);
    TextureHandle b = pool.allocate(2);
    EXPECT_EQ(a.index, b.index) << "slot is reused";
    EXPECT_NE(a.generation, b.generation);
    EXPECT_EQ(pool.get(a), nullptr);
    ASSERT_NE(pool.get(b), nullptr);
    EXPECT_EQ(*pool.get(b), 2);
}

TEST(HandlePool, NullHandleIsInvalid) {
    HandlePool<int, SamplerTag> pool;
    SamplerHandle h;
    EXPECT_FALSE(h.valid());
    EXPECT_EQ(pool.get(h), nullptr);
    pool.allocate(5);
    EXPECT_EQ(pool.get(h), nullptr);
}

TEST(HandlePool, ForEachVisitsLiveObjects) {
    HandlePool<int, PipelineTag> pool;
    auto a = pool.allocate(1);
    pool.allocate(2);
    pool.allocate(3);
    pool.release(a);
    int sum = 0;
    pool.forEach([&](PipelineHandle, int& v) { sum += v; });
    EXPECT_EQ(sum, 5);
}

TEST(IndexAllocator, ReusesFreedIndicesAndReportsExhaustion) {
    IndexAllocator alloc(3, 1);
    EXPECT_EQ(alloc.allocate(), 1u);
    EXPECT_EQ(alloc.allocate(), 2u);
    EXPECT_EQ(alloc.allocate(), ~0u);
    alloc.free(1);
    EXPECT_EQ(alloc.allocate(), 1u);
    EXPECT_EQ(alloc.used(), 2u);
}

TEST(Format, InfoAndSizes) {
    EXPECT_EQ(formatInfo(VK_FORMAT_R8G8B8A8_UNORM).blockBytes, 4u);
    EXPECT_TRUE(formatInfo(VK_FORMAT_D32_SFLOAT).depth);
    EXPECT_TRUE(formatInfo(VK_FORMAT_D24_UNORM_S8_UINT).stencil);
    EXPECT_TRUE(formatInfo(VK_FORMAT_BC7_SRGB_BLOCK).compressed);
    EXPECT_TRUE(formatInfo(VK_FORMAT_BC7_SRGB_BLOCK).srgb);
    EXPECT_EQ(formatAspect(VK_FORMAT_D32_SFLOAT_S8_UINT), VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    EXPECT_EQ(mipLevelSize(VK_FORMAT_R8G8B8A8_UNORM, 256, 128, 1, 0), 256u * 128u * 4u);
    EXPECT_EQ(mipLevelSize(VK_FORMAT_R8G8B8A8_UNORM, 256, 128, 1, 3), 32u * 16u * 4u);
    // BC1: 8 bytes per 4x4 block, partial blocks round up.
    EXPECT_EQ(mipLevelSize(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 10, 10, 1, 0), 3u * 3u * 8u);
    EXPECT_EQ(fullMipCount(1024, 512), 11u);
    EXPECT_EQ(fullMipCount(1, 1), 1u);
}

TEST(Access, MapsToSync2StagesAndLayouts) {
    EXPECT_EQ(accessInfo(Access::ColorAttachmentWrite).layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_TRUE(accessInfo(Access::ColorAttachmentWrite).write);
    EXPECT_FALSE(accessInfo(Access::SampledFragment).write);
    EXPECT_EQ(accessInfo(Access::SampledCompute).stages, VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
    EXPECT_EQ(accessInfo(Access::StorageWriteCompute).layout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(accessInfo(Access::Present).layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    for (int i = 0; i < int(Access::Count); ++i) {
        EXPECT_STRNE(accessName(Access(i)), "?");
    }
}
