// Secondary device + cross-device channels. This machine (and CI) usually has one GPU: the secondary VkDevice is
// created on the primary's physical device (allowSameAdapter), which exercises every transfer path, the bindless
// mirror, the worker-thread synchronisation and the failure handling end to end.
#include "gpu_fixture.hpp"

#include <oxwald/rhi/multi_device.hpp>

#include <cstring>

using namespace ox;
using namespace ox::rhi;
using namespace ox::rhi::test;

namespace {

const char* kInvertShader = R"(#version 460
#include <common/bindless.glsl>
layout(local_size_x = 8, local_size_y = 8) in;
OX_PUSH_CONSTANTS({ uint src; uint dst; uint w; uint h; float scale; });
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.w) || p.y >= int(pc.h)) return;
    vec4 c = OX_FETCH_2D(pc.src, p, 0);
    OX_IMAGE_STORE_2D(rgba8, pc.dst, p, vec4((1.0 - c.rgb) * pc.scale, c.a));
}
)";

struct InvertPush {
    u32 src, dst, w, h;
    f32 scale;
};

class MultiDeviceTest : public GpuTest {
protected:
    void customize(DeviceDesc& d) override { appendMultiDeviceExtensions(d); }
    void releaseResources() override {
        channel.reset();
        secondary.reset();
        for (TextureHandle t : textures) device->destroy(t);
        if (pipeline) device->destroy(pipeline);
    }

    bool makeSecondary(CrossDeviceTransfer transfer) {
        SecondaryDeviceDesc sd;
        sd.allowSameAdapter = true;
        sd.transfer = transfer;
        sd.calibrate = false;
        std::string error;
        secondary = SecondaryDevice::create(*device, sd, &error);
        if (!secondary) ADD_FAILURE() << "secondary device: " << error;
        return secondary != nullptr;
    }

    PipelineHandle invertPipeline() {
        if (!pipeline) {
            ComputePipelineDesc cd;
            cd.name = "test.invert";
            cd.shader = ShaderStageDesc::glsl(kInvertShader, ShaderStage::Compute, "invert.comp");
            pipeline = device->createComputePipeline(cd);
        }
        return pipeline;
    }

    TextureHandle texture(u32 w, u32 h, TextureUsage usage, const char* name) {
        TextureDesc td;
        td.width = w;
        td.height = h;
        td.format = VK_FORMAT_R8G8B8A8_UNORM;
        td.usage = usage;
        td.name = name;
        textures.push_back(device->createTexture(td));
        return textures.back();
    }

    static std::vector<u8> pattern(u32 w, u32 h, u32 seed) {
        std::vector<u8> d(usize(w) * h * 4);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                u8* p = &d[(usize(y) * w + x) * 4];
                p[0] = u8((x * 4 + seed) & 0xFF);
                p[1] = u8((y * 4 + seed * 3) & 0xFF);
                p[2] = u8((x ^ y) + seed);
                p[3] = 255;
            }
        }
        return d;
    }

    // inputs -> [secondary: invert(scale) into tmp, invert(1) into out] -> out = scale * input-ish; one round trip.
    void roundTrip(CrossDeviceTransfer transfer, bool chain) {
        if (!makeSecondary(transfer)) return;
        EXPECT_TRUE(secondary->sameAdapter());
        constexpr u32 W = 61, H = 37; // odd sizes: row pitch / dispatch rounding
        CrossDeviceChannelDesc cd;
        cd.name = "test";
        TextureDesc td;
        td.width = W;
        td.height = H;
        td.format = VK_FORMAT_R8G8B8A8_UNORM;
        cd.textures = {td, td, td}; // 0 input, 1 intermediate, 2 output
        cd.inputs = {0};
        cd.outputs = {chain ? 2u : 1u};
        cd.slots = 2;
        std::string error;
        channel = secondary->createChannel(cd, &error);
        ASSERT_TRUE(channel) << error;
        EXPECT_EQ(channel->uploadBytes(), u64(W) * H * 4);

        const TextureHandle src = texture(W, H, TextureUsage::Sampled | TextureUsage::TransferDst | TextureUsage::TransferSrc, "src");
        const TextureHandle dst = texture(W, H, TextureUsage::Sampled | TextureUsage::TransferDst | TextureUsage::TransferSrc, "dst");
        for (u32 frame = 0; frame < 4; ++frame) { // several jobs per slot: slot reuse, barriers across jobs
            const u32 slot = frame % 2;
            const std::vector<u8> data = pattern(W, H, frame * 17);
            device->uploadTexture(src, data);
            device->immediateSubmit([&](CommandList& cmd) {
                cmd.transition(src, Access::TransferRead);
                const TextureHandle in[] = {src};
                channel->recordExport(cmd, slot, in);
            });
            PortableDispatch a;
            a.name = "invert.a";
            a.pipeline = invertPipeline();
            const InvertPush pa{0, 0, W, H, chain ? 0.5f : 1.0f};
            std::memcpy(a.push.data(), &pa, sizeof(pa));
            a.pushSize = sizeof(pa);
            a.bindings = {{offsetof(InvertPush, src), 0, false}, {offsetof(InvertPush, dst), 1, true}};
            a.threads = {W, H, 1};
            std::vector<PortableDispatch> dispatches{a};
            if (chain) {
                PortableDispatch b = a;
                b.name = "invert.b";
                const InvertPush pb{0, 0, W, H, 1.0f};
                std::memcpy(b.push.data(), &pb, sizeof(pb));
                b.bindings = {{offsetof(InvertPush, src), 1, false}, {offsetof(InvertPush, dst), 2, true}};
                dispatches.push_back(b);
            }
            ASSERT_TRUE(channel->submit(slot, frame + 1, device->lastSubmitted(QueueType::Graphics), dispatches));
            ASSERT_TRUE(channel->wait(slot, frame + 1)) << "secondary job failed";
            EXPECT_TRUE(channel->ready(slot, frame + 1));
            device->immediateSubmit([&](CommandList& cmd) {
                cmd.transition(dst, Access::TransferWrite, true);
                const TextureHandle out[] = {dst};
                channel->recordImport(cmd, slot, out);
                cmd.transition(dst, Access::SampledCompute);
            });
            const std::vector<u8> result = device->readTexture(dst);
            ASSERT_EQ(result.size(), data.size());
            u32 bad = 0;
            for (usize i = 0; i < data.size(); ++i) {
                if (i % 4 == 3) continue;
                // single: 255 - v. chain: 255 - (255 - v) * 0.5 (two UNORM roundings).
                const f32 v = f32(data[i]) / 255.0f;
                const f32 expected = chain ? 1.0f - std::round((1.0f - v) * 0.5f * 255.0f) / 255.0f : 1.0f - v;
                if (std::abs(f32(result[i]) - expected * 255.0f) > 1.01f) ++bad;
            }
            EXPECT_EQ(bad, 0u) << "frame " << frame;
            const CrossDeviceJobTiming t = channel->timing(slot);
            EXPECT_GE(t.computeMs, 0.0);
            if (transfer == CrossDeviceTransfer::Staging) EXPECT_GT(t.cpuCopyMs, 0.0);
        }
    }

    std::unique_ptr<SecondaryDevice> secondary;
    std::unique_ptr<CrossDeviceChannel> channel;
    std::vector<TextureHandle> textures;
    PipelineHandle pipeline;
};

} // namespace

TEST_F(MultiDeviceTest, EnumeratesAdapters) {
    const std::vector<AdapterInfo> adapters = enumerateAdapters(*device);
    ASSERT_FALSE(adapters.empty());
    u32 primaries = 0;
    for (const AdapterInfo& a : adapters) primaries += a.primary ? 1 : 0;
    EXPECT_EQ(primaries, 1u);
    const std::string text = describeAdapters(adapters);
    EXPECT_NE(text.find("primary"), std::string::npos);
    OX_LOG_INFO("test", "adapters:\n{}", text);
}

TEST_F(MultiDeviceTest, NoSecondaryWithoutASecondGpuUnlessAllowed) {
    const std::vector<AdapterInfo> adapters = enumerateAdapters(*device);
    bool other = false;
    for (const AdapterInfo& a : adapters) other = other || (!a.primary && a.suitable && a.type != VK_PHYSICAL_DEVICE_TYPE_CPU);
    if (other) GTEST_SKIP() << "this machine has a second GPU";
    std::string error;
    EXPECT_EQ(SecondaryDevice::create(*device, {}, &error), nullptr);
    EXPECT_NE(error.find("no second GPU"), std::string::npos) << error;
    SecondaryDeviceDesc byIndex;
    for (const AdapterInfo& a : adapters) {
        if (a.primary) byIndex.adapterIndex = i32(a.index);
    }
    EXPECT_EQ(SecondaryDevice::create(*device, byIndex, &error), nullptr);
    EXPECT_NE(error.find("AllowSameAdapter"), std::string::npos) << error;
    EXPECT_EQ(SecondaryDevice::liveCount(), 0u);
}

TEST_F(MultiDeviceTest, SameAdapterCapsAndCalibration) {
    SecondaryDeviceDesc sd;
    sd.allowSameAdapter = true;
    std::string error;
    secondary = SecondaryDevice::create(*device, sd, &error);
    ASSERT_TRUE(secondary) << error;
    EXPECT_EQ(SecondaryDevice::liveCount(), 1u);
    EXPECT_TRUE(secondary->caps().sameDevice);
    EXPECT_TRUE(secondary->caps().luidMatch || !secondary->adapter().luidValid);
    if (device->hasDeviceExtension("VK_KHR_external_memory_win32") || device->hasDeviceExtension("VK_KHR_external_memory_fd")) {
        EXPECT_EQ(secondary->transferPath(), CrossDeviceTransfer::ExternalMemory) << secondary->transferReason();
    }
    if (device->caps().timestampQueries) {
        EXPECT_GT(secondary->calibratedSpeedRatio(), 0.2f);
        EXPECT_LT(secondary->calibratedSpeedRatio(), 5.0f); // same GPU
        OX_LOG_INFO("test", "calibration: primary {:.3f} ms, secondary {:.3f} ms", secondary->calibrationPrimaryMs(),
                    secondary->calibrationSecondaryMs());
    }
    secondary.reset();
    EXPECT_EQ(SecondaryDevice::liveCount(), 0u);
}

TEST_F(MultiDeviceTest, RoundTripExternalMemory) {
    if (!device->hasDeviceExtension("VK_KHR_external_memory_win32") && !device->hasDeviceExtension("VK_KHR_external_memory_fd")) {
        GTEST_SKIP() << "no external memory extension";
    }
    roundTrip(CrossDeviceTransfer::ExternalMemory, false);
    if (channel) EXPECT_EQ(channel->path(), CrossDeviceTransfer::ExternalMemory);
}

TEST_F(MultiDeviceTest, RoundTripHostPointer) {
    if (!device->hasDeviceExtension("VK_EXT_external_memory_host")) GTEST_SKIP() << "no VK_EXT_external_memory_host";
    roundTrip(CrossDeviceTransfer::HostPointer, false);
    if (channel) EXPECT_EQ(channel->path(), CrossDeviceTransfer::HostPointer);
}

TEST_F(MultiDeviceTest, RoundTripStaging) {
    roundTrip(CrossDeviceTransfer::Staging, false);
    if (channel) EXPECT_EQ(channel->path(), CrossDeviceTransfer::Staging);
}

TEST_F(MultiDeviceTest, ChainWithIntermediateTexture) { roundTrip(CrossDeviceTransfer::Auto, true); }

TEST_F(MultiDeviceTest, DeviceLossFailsJobsAndIsReported) {
    if (!makeSecondary(CrossDeviceTransfer::Staging)) return;
    TextureDesc td;
    td.width = td.height = 16;
    CrossDeviceChannelDesc cd;
    cd.name = "lost";
    cd.textures = {td, td};
    cd.inputs = {0};
    cd.outputs = {1};
    channel = secondary->createChannel(cd);
    ASSERT_TRUE(channel);
    secondary->simulateDeviceLost();
    EXPECT_FALSE(secondary->lost()); // fails on the next job
    PortableDispatch a;
    a.pipeline = invertPipeline();
    a.threads = {16, 16, 1};
    EXPECT_TRUE(channel->submit(0, 1, {}, {a}));
    EXPECT_FALSE(channel->wait(0, 1));
    EXPECT_TRUE(secondary->lost());
    EXPECT_NE(secondary->lostReason().find("simulated"), std::string::npos);
    EXPECT_FALSE(channel->submit(1, 2, {}, {a})); // refused once lost
    EXPECT_FALSE(channel->wait(1, 2, 100));
    EXPECT_EQ(secondary->createChannel(cd), nullptr);
}

TEST_F(MultiDeviceTest, UnsupportedTexturesAreRejected) {
    if (!makeSecondary(CrossDeviceTransfer::Staging)) return;
    TextureDesc depth;
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.width = depth.height = 8;
    CrossDeviceChannelDesc cd;
    cd.textures = {depth};
    cd.inputs = {0};
    std::string error;
    EXPECT_EQ(secondary->createChannel(cd, &error), nullptr);
    EXPECT_NE(error.find("portable"), std::string::npos) << error;
    TextureDesc srgb;
    srgb.format = VK_FORMAT_R8G8B8A8_SRGB; // no storage support on most GPUs: fine as an input, not as an output
    srgb.width = srgb.height = 8;
    cd.textures = {srgb, srgb};
    cd.inputs = {0};
    cd.outputs = {1};
    channel = secondary->createChannel(cd, &error);
    if (!channel) EXPECT_NE(error.find("lacks features"), std::string::npos) << error;
}
