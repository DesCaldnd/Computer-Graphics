#include "gpu_fixture.hpp"

#include <oxwald/rhi/render_graph.hpp>

using namespace ox;
using namespace ox::rhi;
using namespace ox::rhi::test;

namespace {

const char* kFullscreenVert = R"(#version 460
layout(location = 0) out vec2 uv;
void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Samples a bindless texture and multiplies it (or outputs a constant color when tex == invalid).
const char* kScaleFrag = R"(#version 460
#include <common/bindless.glsl>
OX_PUSH_CONSTANTS({ vec4 color; uint tex; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 base = pc.tex == OX_INVALID_INDEX ? vec4(1.0) : OX_SAMPLE_2D(pc.tex, OX_SAMPLER_NEAREST_CLAMP, uv);
    outColor = base * pc.color;
}
)";

// Inverts a sampled texture into a storage image.
const char* kInvertComp = R"(#version 460
#include <common/bindless.glsl>
layout(local_size_x = 8, local_size_y = 8) in;
OX_PUSH_CONSTANTS({ uint src; uint dst; });
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    vec4 c = OX_FETCH_2D(pc.src, p, 0);
    OX_IMAGE_STORE_2D(rgba8, pc.dst, p, vec4(1.0 - c.rgb, 1.0));
}
)";

const char* kWriteColorComp = R"(#version 460
#include <common/bindless.glsl>
layout(local_size_x = 1) in;
OX_BUFFER(Color, { vec4 c; });
OX_PUSH_CONSTANTS({ Color out_; vec4 value; });
void main() { pc.out_.c = pc.value; }
)";

const char* kBufferColorFrag = R"(#version 460
#include <common/bindless.glsl>
OX_READONLY_BUFFER(Color, { vec4 c; });
OX_PUSH_CONSTANTS({ Color color; });
layout(location = 0) out vec4 outColor;
void main() { outColor = pc.color.c; }
)";

struct ScalePC {
    f32 color[4];
    u32 tex;
};

TextureDesc rgba8(const char* name, u32 w, u32 h) {
    TextureDesc d;
    d.name = name;
    d.width = w;
    d.height = h;
    d.format = VK_FORMAT_R8G8B8A8_UNORM;
    d.usage = TextureUsage::None;
    return d;
}

} // namespace

class RenderGraphGpuTest : public GpuTest {
protected:
    void SetUp() override {
        GpuTest::SetUp();
        if (!device) return;
        GraphicsPipelineDesc gd;
        gd.name = "rg.scale";
        gd.vertex = ShaderStageDesc::glsl(kFullscreenVert, ShaderStage::Vertex, "rg_fullscreen.vert");
        gd.fragment = ShaderStageDesc::glsl(kScaleFrag, ShaderStage::Fragment, "rg_scale.frag");
        gd.colorFormats = {VK_FORMAT_R8G8B8A8_UNORM};
        scale = device->createGraphicsPipeline(gd);
        ASSERT_NE(device->vkPipeline(scale), VK_NULL_HANDLE) << device->lastPipelineError();
    }
    void releaseResources() override {
        graph.reset();
        graph.releaseResources(*device);
        device->waitIdle();
    }

    void drawScaled(PassContext& ctx, std::array<f32, 4> color, u32 tex) {
        ctx.cmd.bindPipeline(scale);
        ScalePC pc{{color[0], color[1], color[2], color[3]}, tex};
        ctx.cmd.pushConstants(pc);
        ctx.cmd.draw(3);
    }

    PipelineHandle scale;
    RenderGraph graph;
};

TEST_F(RenderGraphGpuTest, TwoPassFrameGraphicsThenCompute) {
    constexpr u32 W = 32, H = 32;
    ComputePipelineDesc cd;
    cd.name = "rg.invert";
    cd.shader = ShaderStageDesc::glsl(kInvertComp, ShaderStage::Compute, "rg_invert.comp");
    PipelineHandle invert = device->createComputePipeline(cd);
    ASSERT_NE(device->vkPipeline(invert), VK_NULL_HANDLE) << device->lastPipelineError();

    TextureDesc outDesc = rgba8("rg.output", W, H);
    outDesc.usage = TextureUsage::Storage | TextureUsage::TransferSrc;
    TextureHandle output = device->createTexture(outDesc);

    for (int frame = 0; frame < 3; ++frame) {
        device->beginFrame();
        graph.reset();
        RGTexture scene = graph.createTexture(rgba8("Scene", W, H));
        RGTexture out = graph.importTexture(*device, output, Access::TransferRead);
        graph.addPass("Scene", PassType::Graphics)
            .color(scene, VK_ATTACHMENT_LOAD_OP_CLEAR, ClearColor::rgba(0, 0, 0, 1))
            .execute([&](PassContext& ctx) { drawScaled(ctx, {0.25f, 0.5f, 1.f, 1.f}, kInvalidBindlessIndex); });
        graph.addPass("Invert", PassType::Compute)
            .read(scene, Access::SampledCompute)
            .overwrite(out, Access::StorageWriteCompute)
            .execute([&](PassContext& ctx) {
                ctx.cmd.bindPipeline(invert);
                const u32 pc[2] = {ctx.sampledIndex(scene), ctx.storageIndex(out)};
                ctx.cmd.pushConstants(pc, sizeof(pc));
                ctx.cmd.dispatchThreads(W, H);
            });
        graph.execute(*device);
        device->endFrame();
    }
    // Frame 0 imports `output` in Undefined, later frames in TransferRead: two plans, then cached.
    EXPECT_EQ(graph.compileCount(), 2u) << "same topology every frame -> cached plan";
    EXPECT_EQ(graph.plan().passes.size(), 2u);
    EXPECT_EQ(device->trackedAccess(output), Access::TransferRead);
    device->waitIdle();
    std::vector<u8> img = device->readTexture(output);
    const Rgba8 p = pixel(img, W, 7, 9);
    EXPECT_TRUE(near(p.r, 191) && near(p.g, 127) && near(p.b, 0)) << int(p.r) << "," << int(p.g) << "," << int(p.b);
    if (device->caps().timestampQueries) {
        bool found = false;
        for (const auto& t : device->gpuTimings()) found |= t.name == "Invert";
        EXPECT_TRUE(found) << "per-pass GPU timestamps";
    }
    device->destroy(invert);
    device->destroy(output);
}

TEST_F(RenderGraphGpuTest, AliasedTransientsProduceCorrectResults) {
    constexpr u32 W = 16, H = 16;
    TextureDesc outDesc = rgba8("rg.alias.out", W, H);
    outDesc.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    TextureHandle output = device->createTexture(outDesc);

    device->beginFrame();
    graph.reset();
    RGTexture a = graph.createTexture(rgba8("A", W, H));
    RGTexture b = graph.createTexture(rgba8("B", W, H));
    RGTexture c = graph.createTexture(rgba8("C", W, H));
    RGTexture out = graph.importTexture(*device, output, Access::TransferRead);
    graph.addPass("P0").color(a).execute([&](PassContext& ctx) { drawScaled(ctx, {1.f, 1.f, 1.f, 1.f}, kInvalidBindlessIndex); });
    graph.addPass("P1").read(a, Access::SampledFragment).color(b).execute([&](PassContext& ctx) {
        drawScaled(ctx, {1.f, 0.5f, 1.f, 1.f}, ctx.sampledIndex(a));
    });
    graph.addPass("P2").read(b, Access::SampledFragment).color(c).execute([&](PassContext& ctx) {
        drawScaled(ctx, {0.5f, 1.f, 1.f, 1.f}, ctx.sampledIndex(b));
    });
    graph.addPass("P3").read(c, Access::SampledFragment).color(out).execute([&](PassContext& ctx) {
        drawScaled(ctx, {1.f, 1.f, 0.f, 1.f}, ctx.sampledIndex(c));
    });
    graph.addPass("Unused").color(graph.createTexture(rgba8("Unused", W, H)));
    CommandList& cmd = device->commandList();
    graph.execute(cmd);
    TimelinePoint done = device->submit(cmd);
    device->endFrame();
    device->wait(done);

    const RenderGraphPlan& plan = graph.plan();
    EXPECT_EQ(plan.culledPasses.size(), 1u);
    EXPECT_EQ(plan.aliasSlots.size(), 2u) << plan.dump(graph);
    EXPECT_EQ(plan.resources[a.id].aliasSlot, plan.resources[c.id].aliasSlot);
    EXPECT_NE(graph.physicalTexture(a), graph.physicalTexture(c)) << "distinct images sharing memory";
    std::vector<u8> img = device->readTexture(output);
    const Rgba8 p = pixel(img, W, 8, 8);
    EXPECT_TRUE(near(p.r, 128) && near(p.g, 128) && near(p.b, 0)) << int(p.r) << "," << int(p.g) << "," << int(p.b);
    device->destroy(output);
    std::printf("%s", graph.exportGraphviz().c_str());
}

TEST_F(RenderGraphGpuTest, AsyncComputeWithQueueOwnershipTransfer) {
    if (!device->caps().asyncComputeQueue) GTEST_SKIP() << "no separate compute queue";
    constexpr u32 W = 8, H = 8;
    ComputePipelineDesc cd;
    cd.name = "rg.writeColor";
    cd.shader = ShaderStageDesc::glsl(kWriteColorComp, ShaderStage::Compute, "rg_write.comp");
    PipelineHandle write = device->createComputePipeline(cd);
    GraphicsPipelineDesc gd;
    gd.name = "rg.bufferColor";
    gd.vertex = ShaderStageDesc::glsl(kFullscreenVert, ShaderStage::Vertex, "rg_fullscreen.vert");
    gd.fragment = ShaderStageDesc::glsl(kBufferColorFrag, ShaderStage::Fragment, "rg_buffer.frag");
    gd.colorFormats = {VK_FORMAT_R8G8B8A8_UNORM};
    PipelineHandle draw = device->createGraphicsPipeline(gd);
    ASSERT_NE(device->vkPipeline(write), VK_NULL_HANDLE) << device->lastPipelineError();
    ASSERT_NE(device->vkPipeline(draw), VK_NULL_HANDLE) << device->lastPipelineError();

    TextureDesc outDesc = rgba8("rg.async.out", W, H);
    outDesc.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    TextureHandle output = device->createTexture(outDesc);

    for (int frame = 0; frame < 4; ++frame) {
        device->beginFrame();
        graph.reset();
        RGBuffer color = graph.createBuffer({16, BufferUsage::Storage, MemoryUsage::GpuOnly, "AsyncColor"});
        RGTexture out = graph.importTexture(*device, output, Access::TransferRead);
        graph.addPass("Simulate", PassType::Compute)
            .queue(QueueType::Compute)
            .overwrite(color, Access::StorageWriteCompute)
            .execute([&](PassContext& ctx) {
                ctx.cmd.bindPipeline(write);
                struct {
                    VkDeviceAddress addr;
                    f32 value[4];
                } pc{ctx.address(color), {0.f, 1.f, f32(frame) / 4.f, 1.f}};
                ctx.cmd.pushConstants(pc);
                ctx.cmd.dispatch(1);
            });
        graph.addPass("Draw").read(color, Access::StorageReadGraphics).color(out).execute([&](PassContext& ctx) {
            ctx.cmd.bindPipeline(draw);
            const VkDeviceAddress a = ctx.address(color);
            ctx.cmd.pushConstants(a);
            ctx.cmd.draw(3);
        });
        graph.execute(*device);
        device->endFrame();
    }
    const RenderGraphPlan& plan = graph.plan();
    ASSERT_EQ(plan.batches.size(), 2u);
    EXPECT_EQ(plan.batches[0].queue, QueueType::Compute);
    EXPECT_EQ(plan.batches[1].waitBatches, std::vector<u32>{0});
    device->waitIdle();
    std::vector<u8> img = device->readTexture(output);
    const Rgba8 p = pixel(img, W, 4, 4);
    EXPECT_TRUE(near(p.r, 0) && near(p.g, 255) && near(p.b, 191)) << int(p.r) << "," << int(p.g) << "," << int(p.b);
    device->destroy(write);
    device->destroy(draw);
    device->destroy(output);
}

TEST(AccelerationStructures, BuildBlasAndTlasWhenSupported) {
    Device::resetValidationCounters();
    DeviceDesc desc;
    desc.validation = true;
    auto device = Device::create(desc);
    if (!device) GTEST_SKIP() << "no Vulkan device";
    if (!device->caps().rayTracingSupported()) {
        EXPECT_FALSE(device->createBlas({}).valid()) << "API refuses cleanly without caps";
        GTEST_SKIP() << device->caps().whyRayTracingUnavailable();
    }
    const f32 verts[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const u32 idx[] = {0, 1, 2};
    BufferHandle vb = device->createBuffer({sizeof(verts), BufferUsage::AccelStructInput, MemoryUsage::GpuOnly, "as.vb"}, verts);
    BufferHandle ib = device->createBuffer({sizeof(idx), BufferUsage::AccelStructInput, MemoryUsage::GpuOnly, "as.ib"}, idx);
    BlasDesc bd;
    bd.name = "blas";
    bd.geometries.push_back({vb, 0, 3, 12, VK_FORMAT_R32G32B32_SFLOAT, ib, 0, 3});
    AccelStructHandle blas = device->createBlas(bd);
    ASSERT_TRUE(blas.valid());
    EXPECT_NE(device->accelStructAddress(blas), 0u);
    AccelStructHandle tlas = device->createTlas({"tlas", 16, true});
    TlasInstance inst;
    inst.blas = blas;
    device->immediateSubmit([&](CommandList& cmd) { cmd.buildTlas(tlas, {&inst, 1}); });
    device->immediateSubmit([&](CommandList& cmd) { cmd.buildTlas(tlas, {&inst, 1}, true); });
    device->waitIdle();
    device->destroy(tlas);
    device->destroy(blas);
    device->destroy(vb);
    device->destroy(ib);
    device.reset();
    EXPECT_EQ(Device::validationErrorCount(), 0u);
}
