// Compute, offscreen triangle, bindless textures, hot reload.
#include "gpu_fixture.hpp"

#include <oxwald/rhi/format.hpp>

#include <chrono>
#include <cstring>
#include <thread>

using namespace ox;
using namespace ox::rhi;
using namespace ox::rhi::test;
namespace fs = std::filesystem;

namespace {

const char* kFillCompute = R"(#version 460
#include <common/bindless.glsl>
layout(local_size_x = 64) in;
OX_BUFFER(Values, { uint v[]; });
OX_PUSH_CONSTANTS({ Values values; uint count; uint scale; });
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < pc.count) pc.values.v[i] = i * pc.scale + 1u;
}
)";

// Vertex pulling: positions + colors come from a buffer device address, no vertex input state.
const char* kTriangleVert = R"(#version 460
#include <common/bindless.glsl>
struct Vertex { vec2 pos; vec3 color; };
OX_READONLY_BUFFER(Vertices, { Vertex v[]; });
OX_PUSH_CONSTANTS({ Vertices vertices; });
layout(location = 0) out vec3 outColor;
void main() {
    Vertex vtx = pc.vertices.v[gl_VertexIndex];
    outColor = vtx.color;
    gl_Position = vec4(vtx.pos, 0.0, 1.0);
}
)";

const char* kTriangleFrag = R"(#version 460
layout(location = 0) in vec3 inColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(inColor, 1.0); }
)";

const char* kFullscreenVert = R"(#version 460
layout(location = 0) out vec2 uv;
void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kBindlessFrag = R"(#version 460
#include <common/bindless.glsl>
OX_PUSH_CONSTANTS({ uint left; uint right; uint smp; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    uint tex = uv.x < 0.5 ? pc.left : pc.right;
    outColor = OX_SAMPLE_2D(tex, pc.smp, uv);
}
)";

const char* kStorageImageCompute = R"(#version 460
#include <common/bindless.glsl>
layout(local_size_x = 8, local_size_y = 8) in;
OX_PUSH_CONSTANTS({ uint image; });
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    OX_IMAGE_STORE_2D(rgba8, pc.image, p, vec4(float(p.x) / 255.0, float(p.y) / 255.0, 1.0, 1.0));
}
)";

struct Vertex {
    f32 pos[2];
    f32 color[3];
};

} // namespace

using PipelineTest = GpuTest;

TEST_F(PipelineTest, ComputeShaderFillsBufferThroughDeviceAddress) {
    constexpr u32 kCount = 1000;
    BufferHandle buf = device->createBuffer({kCount * 4, BufferUsage::Storage, MemoryUsage::GpuOnly, "compute.out"});
    ComputePipelineDesc cd;
    cd.name = "fill";
    cd.shader = ShaderStageDesc::glsl(kFillCompute, ShaderStage::Compute, "fill.comp");
    cd.pushConstantSize = 16;
    PipelineHandle p = device->createComputePipeline(cd);
    ASSERT_NE(device->vkPipeline(p), VK_NULL_HANDLE) << device->lastPipelineError();

    struct {
        VkDeviceAddress values;
        u32 count;
        u32 scale;
    } pc{device->address(buf), kCount, 3};
    device->immediateSubmit([&](CommandList& cmd) {
        cmd.bindPipeline(p);
        cmd.pushConstants(pc);
        cmd.dispatchThreads(kCount);
        cmd.bufferBarrier(buf, Access::StorageWriteCompute, Access::TransferRead);
    });
    std::vector<u8> data = device->readBuffer(buf);
    for (u32 i = 0; i < kCount; ++i) {
        u32 v = 0;
        std::memcpy(&v, data.data() + i * 4, 4);
        ASSERT_EQ(v, i * 3 + 1) << "index " << i;
    }
    device->destroy(p);
    device->destroy(buf);
}

TEST_F(PipelineTest, PushConstantOverflowIsRejected) {
    ComputePipelineDesc cd;
    cd.name = "too-small";
    cd.shader = ShaderStageDesc::glsl(kFillCompute, ShaderStage::Compute, "fill.comp");
    cd.pushConstantSize = 8; // shader uses 16 bytes
    PipelineHandle p = device->createComputePipeline(cd);
    EXPECT_TRUE(device->isAlive(p)) << "handle stays valid so a fixed shader can hot-reload into it";
    EXPECT_EQ(device->vkPipeline(p), VK_NULL_HANDLE);
    EXPECT_NE(device->lastPipelineError().find("push constant"), std::string::npos) << device->lastPipelineError();
    device->destroy(p);
}

TEST_F(PipelineTest, OffscreenTriangleDynamicRendering) {
    constexpr u32 W = 64, H = 64;
    TextureDesc td;
    td.width = W;
    td.height = H;
    td.format = VK_FORMAT_R8G8B8A8_UNORM;
    td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    td.name = "triangle.target";
    TextureHandle target = device->createTexture(td);

    const Vertex verts[3] = {{{0.f, -0.8f}, {1.f, 0.f, 0.f}}, {{0.8f, 0.8f}, {1.f, 0.f, 0.f}}, {{-0.8f, 0.8f}, {1.f, 0.f, 0.f}}};
    BufferHandle vb = device->createBuffer({sizeof(verts), BufferUsage::Storage, MemoryUsage::GpuOnly, "triangle.vb"}, verts);

    GraphicsPipelineDesc gd;
    gd.name = "triangle";
    gd.vertex = ShaderStageDesc::glsl(kTriangleVert, ShaderStage::Vertex, "triangle.vert");
    gd.fragment = ShaderStageDesc::glsl(kTriangleFrag, ShaderStage::Fragment, "triangle.frag");
    gd.colorFormats = {td.format};
    PipelineHandle p = device->createGraphicsPipeline(gd);
    ASSERT_NE(device->vkPipeline(p), VK_NULL_HANDLE) << device->lastPipelineError();

    device->beginFrame();
    CommandList& cmd = device->commandList(QueueType::Graphics, "triangle");
    {
        CommandList::ScopedLabel label(cmd, "Triangle");
        cmd.transition(target, Access::ColorAttachmentWrite, true);
        RenderingDesc rd;
        rd.colors.push_back({target, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ClearColor::rgba(0, 0, 1, 1)});
        cmd.beginRendering(rd);
        cmd.bindPipeline(p);
        const VkDeviceAddress addr = device->address(vb);
        cmd.pushConstants(addr);
        cmd.draw(3);
        cmd.endRendering();
    }
    TimelinePoint done = device->submit(cmd);
    device->endFrame();
    device->wait(done);

    std::vector<u8> img = device->readTexture(target);
    ASSERT_EQ(img.size(), W * H * 4u);
    const Rgba8 center = pixel(img, W, W / 2, H / 2);
    EXPECT_TRUE(near(center.r, 255) && near(center.g, 0) && near(center.b, 0)) << int(center.r) << "," << int(center.g) << "," << int(center.b);
    const Rgba8 corner = pixel(img, W, 1, 1);
    EXPECT_TRUE(near(corner.r, 0) && near(corner.b, 255)) << "clear color in the corner";
    const Rgba8 bottomCorner = pixel(img, W, W - 2, H - 2);
    EXPECT_TRUE(near(bottomCorner.b, 255));
    // Vulkan clip space: y = -0.8 is the top vertex (apex near the top row).
    EXPECT_TRUE(near(pixel(img, W, W / 2, 8).r, 255));
    EXPECT_TRUE(near(pixel(img, W, 8, 8).b, 255));

    device->destroy(p);
    device->destroy(vb);
    device->destroy(target);
}

TEST_F(PipelineTest, DepthTestReversedZ) {
    constexpr u32 W = 32, H = 32;
    TextureDesc cd;
    cd.width = W;
    cd.height = H;
    cd.format = VK_FORMAT_R8G8B8A8_UNORM;
    cd.usage = TextureUsage::ColorAttachment;
    TextureHandle color = device->createTexture(cd);
    TextureDesc dd = cd;
    dd.format = VK_FORMAT_D32_SFLOAT;
    dd.usage = TextureUsage::DepthStencilAttachment;
    TextureHandle depth = device->createTexture(dd);

    const char* vs = R"(#version 460
#include <common/bindless.glsl>
OX_PUSH_CONSTANTS({ float z; vec3 color; });
layout(location = 0) out vec3 c;
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, pc.z, 1.0);
    c = pc.color;
})";
    GraphicsPipelineDesc gd;
    gd.name = "depth";
    gd.vertex = ShaderStageDesc::glsl(vs, ShaderStage::Vertex, "depth.vert");
    gd.fragment = ShaderStageDesc::glsl(kTriangleFrag, ShaderStage::Fragment, "depth.frag");
    gd.colorFormats = {cd.format};
    gd.depthFormat = dd.format;
    gd.depth = {true, true, VK_COMPARE_OP_GREATER_OR_EQUAL};
    PipelineHandle p = device->createGraphicsPipeline(gd);
    ASSERT_NE(device->vkPipeline(p), VK_NULL_HANDLE) << device->lastPipelineError();

    device->immediateSubmit([&](CommandList& cmd) {
        cmd.transition(color, Access::ColorAttachmentWrite, true);
        cmd.transition(depth, Access::DepthStencilWrite, true);
        RenderingDesc rd;
        rd.colors.push_back({color, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR});
        rd.depth = DepthAttachment{depth, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {0.f, 0}};
        cmd.beginRendering(rd);
        cmd.bindPipeline(p);
        struct {
            f32 z;
            f32 color[3];
        } nearGreen{0.9f, {0, 1, 0}}, farRed{0.2f, {1, 0, 0}};
        cmd.pushConstants(nearGreen);
        cmd.draw(3);
        cmd.pushConstants(farRed); // farther in reversed-Z -> rejected
        cmd.draw(3);
        cmd.endRendering();
    });
    std::vector<u8> img = device->readTexture(color);
    const Rgba8 c = pixel(img, W, W / 2, H / 2);
    EXPECT_TRUE(near(c.g, 255) && near(c.r, 0)) << int(c.r) << "," << int(c.g);
    device->destroy(p);
    device->destroy(color);
    device->destroy(depth);
}

TEST_F(PipelineTest, BindlessTextureSamplingAndStorageImages) {
    constexpr u32 W = 32, H = 16;
    auto solid = [&](u8 r, u8 g, u8 b, const char* name) {
        TextureDesc td;
        td.width = td.height = 4;
        td.format = VK_FORMAT_R8G8B8A8_UNORM;
        td.name = name;
        TextureHandle t = device->createTexture(td);
        std::vector<u8> px(4 * 4 * 4);
        for (usize i = 0; i < px.size(); i += 4) {
            px[i] = r;
            px[i + 1] = g;
            px[i + 2] = b;
            px[i + 3] = 255;
        }
        device->uploadTexture(t, px, {0, ~0u, 0, ~0u, Access::SampledFragment});
        return t;
    };
    TextureHandle red = solid(255, 0, 0, "bindless.red");
    TextureHandle green = solid(0, 255, 0, "bindless.green");
    EXPECT_NE(device->sampledIndex(red), device->sampledIndex(green));

    TextureDesc td;
    td.width = W;
    td.height = H;
    td.format = VK_FORMAT_R8G8B8A8_UNORM;
    td.usage = TextureUsage::ColorAttachment | TextureUsage::Storage;
    TextureHandle target = device->createTexture(td);

    GraphicsPipelineDesc gd;
    gd.name = "bindless";
    gd.vertex = ShaderStageDesc::glsl(kFullscreenVert, ShaderStage::Vertex, "fullscreen.vert");
    gd.fragment = ShaderStageDesc::glsl(kBindlessFrag, ShaderStage::Fragment, "bindless.frag");
    gd.colorFormats = {td.format};
    PipelineHandle p = device->createGraphicsPipeline(gd);
    ASSERT_NE(device->vkPipeline(p), VK_NULL_HANDLE) << device->lastPipelineError();

    device->immediateSubmit([&](CommandList& cmd) {
        cmd.transition(target, Access::ColorAttachmentWrite, true);
        RenderingDesc rd;
        rd.colors.push_back({target, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR});
        cmd.beginRendering(rd);
        cmd.bindPipeline(p);
        const u32 pc[3] = {device->sampledIndex(red), device->sampledIndex(green), u32(DefaultSampler::NearestClamp)};
        cmd.pushConstants(pc, sizeof(pc));
        cmd.draw(3);
        cmd.endRendering();
    });
    std::vector<u8> img = device->readTexture(target);
    EXPECT_TRUE(near(pixel(img, W, 4, 8).r, 255) && near(pixel(img, W, 4, 8).g, 0));
    EXPECT_TRUE(near(pixel(img, W, 28, 8).g, 255) && near(pixel(img, W, 28, 8).r, 0));

    // Storage image written through its bindless storage index.
    ComputePipelineDesc cd;
    cd.name = "storage";
    cd.shader = ShaderStageDesc::glsl(kStorageImageCompute, ShaderStage::Compute, "storage.comp");
    PipelineHandle cp = device->createComputePipeline(cd);
    ASSERT_NE(device->vkPipeline(cp), VK_NULL_HANDLE) << device->lastPipelineError();
    const u32 storage = device->storageIndex(target, 0);
    EXPECT_EQ(storage, device->storageIndex(target, 0)) << "storage index is cached";
    device->immediateSubmit([&](CommandList& cmd) {
        cmd.transition(target, Access::StorageWriteCompute, true);
        cmd.bindPipeline(cp);
        cmd.pushConstants(storage);
        cmd.dispatchThreads(W, H);
        cmd.transition(target, Access::TransferRead);
    });
    img = device->readTexture(target);
    const Rgba8 px = pixel(img, W, 10, 5);
    EXPECT_EQ(px.r, 10);
    EXPECT_EQ(px.g, 5);
    EXPECT_EQ(px.b, 255);

    device->destroy(p);
    device->destroy(cp);
    device->destroy(red);
    device->destroy(green);
    device->destroy(target);
}

TEST_F(PipelineTest, HotReloadSwapsPipelineKeepingTheHandle) {
    const fs::path dir = makeTempDir("hotreload");
    writeTextFile(dir / "value.glsl", "#define VALUE 1u\n");
    const fs::path shader = dir / "write.comp";
    writeTextFile(shader, R"(#version 460
#include <common/bindless.glsl>
#include "value.glsl"
layout(local_size_x = 1) in;
OX_BUFFER(Out, { uint v; });
OX_PUSH_CONSTANTS({ Out o; });
void main() { pc.o.v = VALUE; }
)");
    BufferHandle buf = device->createBuffer({4, BufferUsage::Storage, MemoryUsage::Readback, "hotreload.out"});
    ComputePipelineDesc cd;
    cd.name = "hotreload";
    cd.shader = ShaderStageDesc::file(shader);
    PipelineHandle p = device->createComputePipeline(cd);
    ASSERT_NE(device->vkPipeline(p), VK_NULL_HANDLE) << device->lastPipelineError();
    EXPECT_EQ(device->pipelineVersion(p), 1u);

    auto run = [&]() {
        device->beginFrame();
        CommandList& cmd = device->commandList(QueueType::Graphics);
        cmd.bindPipeline(p);
        const VkDeviceAddress a = device->address(buf);
        cmd.pushConstants(a);
        cmd.dispatch(1);
        cmd.memoryBarrier(Access::StorageWriteCompute, Access::HostRead);
        TimelinePoint t = device->submit(cmd);
        device->endFrame();
        device->wait(t);
        u32 v = 0;
        std::memcpy(&v, device->readBuffer(buf).data(), 4);
        return v;
    };
    auto touch = [&](const fs::path& p, const std::string& text) {
        writeTextFile(p, text);
        // Make sure the mtime moves even on coarse-grained file systems.
        fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(2));
    };

    EXPECT_EQ(run(), 1u);
    // Changing an *included* file triggers the rebuild.
    touch(dir / "value.glsl", "#define VALUE 2u\n");
    EXPECT_EQ(device->reloadChangedShaders(true), 1u);
    EXPECT_EQ(device->pipelineVersion(p), 2u);
    EXPECT_EQ(run(), 2u);

    // A broken edit keeps the previous pipeline running.
    touch(dir / "value.glsl", "#define VALUE this is not valid glsl\n");
    EXPECT_EQ(device->reloadChangedShaders(true), 0u);
    EXPECT_EQ(device->pipelineVersion(p), 2u);
    EXPECT_NE(device->vkPipeline(p), VK_NULL_HANDLE);
    EXPECT_EQ(run(), 2u);

    // Fixing it recovers; beginFrame() polls automatically.
    touch(dir / "value.glsl", "#define VALUE 3u\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(run(), 3u) << "beginFrame() swapped the pipeline before recording";
    EXPECT_EQ(device->pipelineVersion(p), 3u);

    device->destroy(p);
    device->destroy(buf);
    fs::remove_all(dir);
}

TEST_F(PipelineTest, MsaaRenderWithResolve) {
    if (!(device->caps().framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT)) GTEST_SKIP() << "no 4x MSAA";
    constexpr u32 W = 32, H = 32;
    TextureDesc msaa;
    msaa.width = W;
    msaa.height = H;
    msaa.samples = 4;
    msaa.format = VK_FORMAT_R8G8B8A8_UNORM;
    msaa.usage = TextureUsage::ColorAttachment;
    msaa.name = "msaa.color";
    TextureHandle ms = device->createTexture(msaa);
    TextureDesc single = msaa;
    single.samples = 1;
    single.name = "msaa.resolved";
    TextureHandle resolved = device->createTexture(single);

    const Vertex verts[3] = {{{-1.f, -1.f}, {0.f, 1.f, 0.f}}, {{3.f, -1.f}, {0.f, 1.f, 0.f}}, {{-1.f, 3.f}, {0.f, 1.f, 0.f}}};
    BufferHandle vb = device->createBuffer({sizeof(verts), BufferUsage::Storage, MemoryUsage::GpuOnly, "msaa.vb"}, verts);
    GraphicsPipelineDesc gd;
    gd.name = "msaa";
    gd.vertex = ShaderStageDesc::glsl(kTriangleVert, ShaderStage::Vertex, "triangle.vert");
    gd.fragment = ShaderStageDesc::glsl(kTriangleFrag, ShaderStage::Fragment, "triangle.frag");
    gd.colorFormats = {msaa.format};
    gd.samples = 4;
    PipelineHandle p = device->createGraphicsPipeline(gd);
    ASSERT_NE(device->vkPipeline(p), VK_NULL_HANDLE) << device->lastPipelineError();
    device->immediateSubmit([&](CommandList& cmd) {
        cmd.transition(ms, Access::ColorAttachmentWrite, true);
        cmd.transition(resolved, Access::ColorAttachmentWrite, true);
        RenderingDesc rd;
        ColorAttachment c{ms, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, ClearColor::rgba(0, 0, 0, 1)};
        c.resolve = resolved;
        rd.colors.push_back(c);
        cmd.beginRendering(rd);
        cmd.bindPipeline(p);
        const VkDeviceAddress a = device->address(vb);
        cmd.pushConstants(a);
        cmd.draw(3);
        cmd.endRendering();
    });
    std::vector<u8> img = device->readTexture(resolved);
    EXPECT_TRUE(near(pixel(img, W, 16, 16).g, 255));
    device->destroy(p);
    device->destroy(vb);
    device->destroy(ms);
    device->destroy(resolved);
}
