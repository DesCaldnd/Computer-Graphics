#include "gpu_fixture.hpp"

#include <oxwald/rhi/swapchain.hpp>

#include <numeric>

using namespace ox;
using namespace ox::rhi;
using namespace ox::rhi::test;

using DeviceTest = GpuTest;

TEST_F(DeviceTest, CreatesDeviceAndReportsCaps) {
    const DeviceCaps& caps = device->caps();
    std::printf("%s\n", caps.toString().c_str());
    EXPECT_FALSE(caps.gpuName.empty());
    EXPECT_GE(caps.maxBindlessSampledImages, 16u);
    EXPECT_GE(caps.maxPushConstantsSize, 128u);
    EXPECT_TRUE(caps.validationEnabled) << "validation layers should be found via the bundled runtime";
    EXPECT_TRUE(caps.debugUtils);
    if (!caps.rayTracingSupported()) {
        EXPECT_FALSE(caps.whyRayTracingUnavailable().empty());
    } else {
        EXPECT_TRUE(caps.whyRayTracingUnavailable().empty());
    }
    EXPECT_NE(device->bindlessSet(), VK_NULL_HANDLE);
    EXPECT_NE(device->pipelineLayout(), VK_NULL_HANDLE);
    // Default samplers occupy fixed bindless slots (mirrored in bindless.glsl).
    for (u32 i = 0; i < u32(DefaultSampler::Count); ++i) {
        EXPECT_EQ(device->samplerIndex(device->defaultSampler(DefaultSampler(i))), i);
    }
}

TEST_F(DeviceTest, BufferUploadAndReadback) {
    std::vector<u32> data(4096);
    std::iota(data.begin(), data.end(), 100u);
    const u64 bytes = data.size() * 4;

    BufferHandle gpu = device->createBuffer({bytes, BufferUsage::Storage, MemoryUsage::GpuOnly, "test.gpu"}, data.data());
    ASSERT_TRUE(gpu.valid());
    EXPECT_EQ(device->mapped(gpu), nullptr);
    EXPECT_NE(device->address(gpu), 0u);
    std::vector<u8> back = device->readBuffer(gpu);
    ASSERT_EQ(back.size(), bytes);
    EXPECT_EQ(std::memcmp(back.data(), data.data(), bytes), 0);

    // Partial write + read at an offset.
    const u32 patch[2] = {0xDEADBEEF, 0xCAFEBABE};
    device->writeBuffer(gpu, patch, sizeof(patch), 16);
    std::vector<u8> part = device->readBuffer(gpu, 16, 8);
    EXPECT_EQ(std::memcmp(part.data(), patch, 8), 0);

    BufferHandle host = device->createBuffer({bytes, BufferUsage::Storage, MemoryUsage::Upload, "test.host"});
    ASSERT_NE(device->mapped(host), nullptr);
    device->writeBuffer(host, data.data(), bytes);
    device->immediateSubmit([&](CommandList& cmd) {
        cmd.fillBuffer(gpu, 0);
        cmd.memoryBarrier(Access::TransferWrite, Access::TransferWrite);
        cmd.copyBuffer(host, gpu, bytes);
    });
    back = device->readBuffer(gpu);
    EXPECT_EQ(std::memcmp(back.data(), data.data(), bytes), 0);

    device->destroy(gpu);
    device->destroy(host);
    EXPECT_FALSE(device->isAlive(gpu));
    EXPECT_FALSE(device->isAlive(host));
}

TEST_F(DeviceTest, AsyncUploadOnTransferQueue) {
    std::vector<u32> data(1 << 16);
    std::iota(data.begin(), data.end(), 7u);
    const u64 bytes = data.size() * 4;
    BufferHandle dst = device->createBuffer({bytes, BufferUsage::Storage, MemoryUsage::GpuOnly, "test.async"});
    BufferHandle copy = device->createBuffer({bytes, BufferUsage::Storage, MemoryUsage::Readback, "test.copy"});

    device->beginFrame();
    device->uploadBufferAsync(dst, {reinterpret_cast<const u8*>(data.data()), bytes}, 0, Access::TransferRead);
    // The graphics submit waits for the transfer timeline and acquires queue ownership automatically.
    CommandList& cmd = device->commandList(QueueType::Graphics, "consume");
    cmd.copyBuffer(dst, copy, bytes);
    cmd.memoryBarrier(Access::TransferWrite, Access::HostRead);
    TimelinePoint done = device->submit(cmd);
    device->endFrame();
    device->wait(done);
    std::vector<u8> back = device->readBuffer(copy);
    EXPECT_EQ(std::memcmp(back.data(), data.data(), bytes), 0);

    // Many small uploads exercise the staging ring (wrap-around + reclamation).
    for (int frame = 0; frame < 6; ++frame) {
        device->beginFrame();
        for (int i = 0; i < 16; ++i) {
            std::vector<u32> chunk(32 * 1024, u32(frame * 100 + i));
            device->uploadBufferAsync(dst, {reinterpret_cast<const u8*>(chunk.data()), chunk.size() * 4}, u64(i) * 4096);
        }
        device->endFrame();
    }
    device->waitIdle();
    std::vector<u8> last = device->readBuffer(dst, 15 * 4096, 4);
    u32 v = 0;
    std::memcpy(&v, last.data(), 4);
    EXPECT_EQ(v, 515u);
    device->destroy(dst);
    device->destroy(copy);
}

TEST_F(DeviceTest, TextureUploadReadbackAndMips) {
    TextureDesc td;
    td.width = 64;
    td.height = 32;
    td.mipLevels = 0; // full chain
    td.format = VK_FORMAT_R8G8B8A8_UNORM;
    td.usage = TextureUsage::Sampled | TextureUsage::TransferDst | TextureUsage::TransferSrc;
    td.name = "test.texture";
    TextureHandle tex = device->createTexture(td);
    ASSERT_TRUE(tex.valid());
    EXPECT_EQ(device->desc(tex).mipLevels, 7u);
    EXPECT_NE(device->sampledIndex(tex), kInvalidBindlessIndex);

    std::vector<u8> pixels(64 * 32 * 4);
    for (u32 y = 0; y < 32; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            u8* p = &pixels[(y * 64 + x) * 4];
            p[0] = u8(x * 4);
            p[1] = u8(y * 8);
            p[2] = 200;
            p[3] = 255;
        }
    }
    device->uploadTexture(tex, pixels, {0, 1});
    EXPECT_EQ(device->trackedAccess(tex), Access::SampledGraphics);
    std::vector<u8> back = device->readTexture(tex, 0, 0);
    ASSERT_EQ(back.size(), pixels.size());
    EXPECT_EQ(back, pixels);
    EXPECT_EQ(device->trackedAccess(tex), Access::SampledGraphics) << "readback restores the previous state";

    device->immediateSubmit([&](CommandList& cmd) { cmd.generateMipmaps(tex, Access::SampledGraphics); });
    std::vector<u8> lastMip = device->readTexture(tex, 6, 0);
    ASSERT_EQ(lastMip.size(), 4u);
    EXPECT_TRUE(near(lastMip[2], 200, 2));
    EXPECT_TRUE(near(lastMip[0], 126, 8)) << int(lastMip[0]);

    // Views per mip/layer are cached.
    VkImageView v1 = device->view(tex, {{2, 1, 0, 1}});
    VkImageView v2 = device->view(tex, {{2, 1, 0, 1}});
    EXPECT_EQ(v1, v2);
    EXPECT_NE(v1, device->view(tex));
    device->destroy(tex);
}

TEST_F(DeviceTest, CubeArrayDepthAndCompressedTextures) {
    TextureDesc cube;
    cube.type = TextureType::Cube;
    cube.width = cube.height = 16;
    cube.arrayLayers = 12; // cube array of 2
    cube.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    cube.name = "test.cube";
    TextureHandle c = device->createTexture(cube);
    ASSERT_TRUE(c.valid());
    std::vector<u8> faces(16 * 16 * 8 * 12, 0x3C);
    device->uploadTexture(c, faces);

    TextureDesc depth;
    depth.width = depth.height = 64;
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.usage = TextureUsage::DepthStencilAttachment | TextureUsage::Sampled;
    depth.name = "test.depth";
    TextureHandle d = device->createTexture(depth);
    ASSERT_TRUE(d.valid());
    device->immediateSubmit([&](CommandList& cmd) {
        cmd.transition(d, Access::TransferWrite, true);
        cmd.clearDepthStencil(d, {0.25f, 0});
        cmd.transition(d, Access::SampledFragment);
    });
    std::vector<u8> back = device->readTexture(d);
    f32 v = 0;
    std::memcpy(&v, back.data(), 4);
    EXPECT_FLOAT_EQ(v, 0.25f);

    if (device->caps().textureCompressionBC) {
        TextureDesc bc;
        bc.width = bc.height = 16;
        bc.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        bc.name = "test.bc1";
        TextureHandle b = device->createTexture(bc);
        std::vector<u8> blocks(4 * 4 * 8, 0xAB);
        device->uploadTexture(b, blocks);
        EXPECT_EQ(device->readTexture(b), blocks);
        device->destroy(b);
    }
    device->destroy(c);
    device->destroy(d);
}

TEST_F(DeviceTest, FramesInFlightStress) {
    BufferHandle counter = device->createBuffer({4 * 100, BufferUsage::Storage, MemoryUsage::GpuOnly, "test.counter"});
    std::vector<TextureHandle> transient;
    for (u32 frame = 0; frame < 100; ++frame) {
        device->beginFrame();
        EXPECT_EQ(device->frameNumber(), frame);
        CommandList& cmd = device->commandList(QueueType::Graphics);
        cmd.beginTimestamp("frame");
        cmd.updateBuffer(counter, &frame, 4, frame * 4);
        cmd.endTimestamp();
        // Per-frame resources destroyed while in flight exercise deferred destruction.
        TextureDesc td;
        td.width = td.height = 8;
        td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
        TextureHandle t = device->createTexture(td);
        cmd.transition(t, Access::TransferWrite, true);
        cmd.clearTexture(t, ClearColor::rgba(1, 0, 0, 1));
        cmd.transition(t, Access::SampledFragment);
        device->submit(cmd);
        if (device->caps().asyncComputeQueue && frame % 3 == 0) {
            CommandList& c = device->commandList(QueueType::Compute);
            device->submit(c, {{device->lastSubmitted(QueueType::Graphics)}});
        }
        device->destroy(t);
        device->endFrame();
    }
    device->waitIdle();
    std::vector<u8> back = device->readBuffer(counter);
    for (u32 i = 0; i < 100; ++i) {
        u32 v = 0;
        std::memcpy(&v, back.data() + i * 4, 4);
        ASSERT_EQ(v, i);
    }
    EXPECT_TRUE(device->isComplete(device->lastSubmitted(QueueType::Graphics)));
    if (device->caps().timestampQueries) {
        ASSERT_FALSE(device->gpuTimings().empty());
        EXPECT_EQ(device->gpuTimings()[0].name, "frame");
    }
    GpuMemoryStats stats = device->memoryStats();
    EXPECT_FALSE(stats.heaps.empty());
    EXPECT_GT(stats.totalUsageBytes, 0u);
    device->destroy(counter);
}

TEST_F(DeviceTest, StaleHandlesAreRejected) {
    BufferHandle b = device->createBuffer({256, BufferUsage::Storage, MemoryUsage::Upload, "stale"});
    device->destroy(b);
    BufferHandle b2 = device->createBuffer({256, BufferUsage::Storage, MemoryUsage::Upload, "fresh"});
    EXPECT_FALSE(device->isAlive(b));
    EXPECT_TRUE(device->isAlive(b2));
    EXPECT_EQ(device->vkBuffer(b), VK_NULL_HANDLE);
    device->destroy(b2);
}

TEST_F(DeviceTest, RenderDocAndPipelineCache) {
    RenderDocCapture& rd = device->renderDoc();
#if defined(__APPLE__)
    EXPECT_FALSE(rd.available());
    EXPECT_NE(rd.unavailableReason().find("Xcode"), std::string::npos);
#endif
    rd.triggerCapture(); // no-op when unavailable
    EXPECT_FALSE(device->savePipelineCache()) << "no path configured";
}

TEST_F(DeviceTest, AsyncTextureUploadWithOwnershipTransfer) {
    TextureDesc td;
    td.width = 16;
    td.height = 16;
    td.mipLevels = 2;
    td.format = VK_FORMAT_R8G8B8A8_UNORM;
    td.name = "async.texture";
    TextureHandle tex = device->createTexture(td);
    std::vector<u8> data(16 * 16 * 4 + 8 * 8 * 4);
    for (usize i = 0; i < data.size(); ++i) data[i] = u8(i < 16 * 16 * 4 ? 10 : 20);
    device->beginFrame();
    device->uploadTextureAsync(tex, data, {0, ~0u, 0, ~0u, Access::SampledFragment});
    // A graphics submission consumes the upload (waits on the transfer timeline, acquires ownership).
    CommandList& cmd = device->commandList();
    TimelinePoint t = device->submit(cmd);
    device->endFrame();
    device->wait(t);
    EXPECT_EQ(device->trackedAccess(tex), Access::SampledFragment);
    std::vector<u8> mip0 = device->readTexture(tex, 0);
    std::vector<u8> mip1 = device->readTexture(tex, 1);
    EXPECT_EQ(mip0[0], 10);
    EXPECT_EQ(mip0.back(), 10);
    EXPECT_EQ(mip1[0], 20);
    device->destroy(tex);
}

TEST(DeviceCache, PipelineCachePersistsToDisk) {
    const auto dir = makeTempDir("pipecache");
    const auto path = dir / "pipelines.bin";
    for (int run = 0; run < 2; ++run) {
        DeviceDesc desc;
        desc.validation = true;
        desc.pipelineCachePath = path;
        auto device = Device::create(desc);
        if (!device) GTEST_SKIP() << "no Vulkan device";
        ComputePipelineDesc cd;
        cd.name = "cached";
        cd.shader = ShaderStageDesc::glsl("#version 460\nlayout(local_size_x=1) in;\nvoid main() {}\n", ShaderStage::Compute,
                                          "cached.comp");
        PipelineHandle p = device->createComputePipeline(cd);
        EXPECT_NE(device->vkPipeline(p), VK_NULL_HANDLE);
        device->destroy(p);
        EXPECT_TRUE(device->savePipelineCache());
    }
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 32u);
    usize files = 0;
    for (const auto& e : std::filesystem::directory_iterator(path.parent_path())) {
        EXPECT_EQ(e.path().extension(), path.extension()) << "no temp files left behind: " << e.path();
        ++files;
    }
    EXPECT_EQ(files, 1u);
    std::filesystem::remove_all(dir);
}

namespace {
// Window-less surface (VK_EXT_headless_surface, supported by MoltenVK and Mesa) to exercise the surface/present path.
class HeadlessSurfaceProvider final : public ISurfaceProvider {
public:
    std::vector<const char*> requiredInstanceExtensions() const override {
        return {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    }
    VkSurfaceKHR createSurface(VkInstance instance) override {
        auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
        if (!create) return VK_NULL_HANDLE;
        VkHeadlessSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        return create(instance, &ci, nullptr, &surface) == VK_SUCCESS ? surface : VK_NULL_HANDLE;
    }
    VkExtent2D framebufferSize() const override { return {64, 64}; }
};

bool headlessSurfaceAvailable() {
    u32 count = 0;
    if (!vkEnumerateInstanceExtensionProperties) return false;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const auto& e : exts) {
        if (std::string_view(e.extensionName) == VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) return true;
    }
    return false;
}
} // namespace

// Regression: vk-bootstrap caches instance functions from the first instance of the process; a surface device
// created after a headless one crashed in get_present_queue_index (null vkGetPhysicalDeviceSurfaceSupportKHR).
TEST(DeviceLifecycle, SurfaceDeviceAfterHeadlessDevice) {
    {
        DeviceDesc headless;
        headless.validation = false;
        auto device = Device::create(headless);
        if (!device) GTEST_SKIP() << "no Vulkan device";
    }
    if (!headlessSurfaceAvailable()) GTEST_SKIP() << "VK_EXT_headless_surface not available";
    HeadlessSurfaceProvider provider;
    DeviceDesc windowed;
    windowed.validation = false;
    windowed.surface = &provider;
    std::string error;
    auto device = Device::create(windowed, &error);
    ASSERT_NE(device, nullptr) << error;
    EXPECT_NE(device->surface(), VK_NULL_HANDLE);
    auto swapchain = Swapchain::create(*device);
    EXPECT_NE(swapchain, nullptr);
    swapchain.reset();
    device->waitIdle();
}
