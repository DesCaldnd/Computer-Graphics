// Глава 17: Device и DeviceCaps, буферы по device address, compute, render graph на GPU, hot reload шейдеров,
// профилирование, BLAS/TLAS (docs/guide/17-rhi-vulkan.md). Нужен Vulkan-устройство; без него тесты пропускаются.
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/render_graph.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace ox;
using namespace ox::rhi;
namespace fs = std::filesystem;

namespace {

// Headless-устройство с validation layers; любой validation error валит тест.
class GuideRhiDevice : public ::testing::Test {
protected:
    void SetUp() override {
        Device::resetValidationCounters();
        DeviceDesc desc;
        desc.appName = "guide_rhi";
        desc.validation = true;  // по умолчанию: вкл. в Debug/RelWithDebInfo, выкл. в Release
        desc.surface = nullptr;  // headless: без окна и swapchain
        std::string error;
        device = Device::create(desc, &error);
        if (!device) GTEST_SKIP() << "нет Vulkan-устройства: " << error;
    }
    void TearDown() override {
        if (!device) return;
        device->waitIdle();
        device.reset(); // Device уничтожается последним — после графов и ресурсов
        EXPECT_EQ(Device::validationErrorCount(), 0u) << "validation layer сообщил об ошибках (см. лог)";
    }
    std::unique_ptr<Device> device;
};

constexpr const char* kFillComp = R"(#version 460
#include <common/bindless.glsl>
layout(local_size_x = 64) in;
OX_BUFFER(Values, { uint v[]; });
OX_PUSH_CONSTANTS({ Values values; uint count; uint scale; });
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < pc.count) pc.values.v[i] = i * pc.scale;
}
)";

constexpr const char* kFullscreenVert = R"(#version 460
layout(location = 0) out vec2 uv;
void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Читает bindless-текстуру по индексу из push constants и умножает на цвет.
constexpr const char* kTintFrag = R"(#version 460
#include <common/bindless.glsl>
OX_PUSH_CONSTANTS({ vec4 color; uint tex; });
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 base = pc.tex == OX_INVALID_INDEX ? vec4(1.0) : OX_SAMPLE_2D(pc.tex, OX_SAMPLER_NEAREST_CLAMP, uv);
    outColor = base * pc.color;
}
)";

struct TintPC {
    f32 color[4];
    u32 tex;
};

} // namespace

TEST_F(GuideRhiDevice, CapsAndFeatureDetection) {
    const DeviceCaps& caps = device->caps();
    std::printf("%s\n", caps.toString().c_str()); // полный отчёт о GPU в лог

    EXPECT_FALSE(caps.gpuName.empty());
    EXPECT_GE(caps.maxPushConstantsSize, kMaxPushConstantSize);

    // Фичи проверяем по caps, а не по платформе.
    if (!caps.rayTracingSupported()) {
        // На MoltenVK: «MoltenVK does not implement Vulkan ray tracing» — текст для подсказки в UI.
        EXPECT_FALSE(caps.whyRayTracingUnavailable().empty());
    }
    if (caps.portabilitySubset) {
        EXPECT_FALSE(caps.geometryShader) << "MoltenVK: геометрических шейдеров нет";
    }
    // Стандартные сэмплеры занимают фиксированные bindless-слоты 0..5 (как в bindless.glsl).
    EXPECT_EQ(device->samplerIndex(device->defaultSampler(DefaultSampler::NearestClamp)), 3u);
}

TEST_F(GuideRhiDevice, ComputeThroughBufferDeviceAddress) {
    constexpr u32 kCount = 256;
    BufferHandle values = device->createBuffer({kCount * 4, BufferUsage::Storage, MemoryUsage::GpuOnly, "guide.values"});

    ComputePipelineDesc cd;
    cd.name = "guide.fill";
    cd.shader = ShaderStageDesc::glsl(kFillComp, ShaderStage::Compute, "guide_fill.comp");
    cd.pushConstantSize = 16; // проверяется по рефлексии шейдера
    PipelineHandle fill = device->createComputePipeline(cd);
    ASSERT_NE(device->vkPipeline(fill), VK_NULL_HANDLE) << device->lastPipelineError();

    struct {
        VkDeviceAddress values; // буферы передаются адресом, а не дескриптором
        u32 count;
        u32 scale;
    } pc{device->address(values), kCount, 3};

    // immediateSubmit: записать, отправить и дождаться — для загрузки, инструментов и тестов.
    device->immediateSubmit([&](CommandList& cmd) {
        cmd.bindPipeline(fill); // заодно биндит bindless-набор
        cmd.pushConstants(pc);
        cmd.dispatchThreads(kCount); // число групп из local_size шейдера
        cmd.bufferBarrier(values, Access::StorageWriteCompute, Access::TransferRead);
    });

    std::vector<u8> back = device->readBuffer(values);
    u32 v = 0;
    std::memcpy(&v, back.data() + 100 * 4, 4);
    EXPECT_EQ(v, 300u);

    device->destroy(fill);   // отложенное удаление: GPU мог ещё использовать объект
    device->destroy(values);
    EXPECT_FALSE(device->isAlive(values));
}

TEST_F(GuideRhiDevice, RenderGraphFrames) {
    constexpr u32 W = 32, H = 32;
    GraphicsPipelineDesc gd;
    gd.name = "guide.tint";
    gd.vertex = ShaderStageDesc::glsl(kFullscreenVert, ShaderStage::Vertex, "guide_fullscreen.vert");
    gd.fragment = ShaderStageDesc::glsl(kTintFrag, ShaderStage::Fragment, "guide_tint.frag");
    gd.colorFormats = {VK_FORMAT_R8G8B8A8_UNORM}; // dynamic rendering: форматы вместо render pass
    PipelineHandle tint = device->createGraphicsPipeline(gd);
    ASSERT_NE(device->vkPipeline(tint), VK_NULL_HANDLE) << device->lastPipelineError();

    // Постоянная текстура-результат (живёт дольше графа) — импортируется каждый кадр.
    TextureDesc od;
    od.name = "guide.output";
    od.width = W;
    od.height = H;
    od.format = VK_FORMAT_R8G8B8A8_UNORM;
    od.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    TextureHandle output = device->createTexture(od);

    RenderGraph graph; // один на всё время жизни: кэширует план и транзиентную память
    auto draw = [&](PassContext& ctx, std::array<f32, 4> c, u32 tex) {
        ctx.cmd.bindPipeline(tint);
        ctx.cmd.pushConstants(TintPC{{c[0], c[1], c[2], c[3]}, tex});
        ctx.cmd.draw(3);
    };

    for (int frame = 0; frame < 4; ++frame) {
        device->beginFrame(); // ждёт слот кадра, отложенное удаление, hot reload
        graph.reset();

        TextureDesc sd = od;
        sd.name = "Scene";
        sd.usage = TextureUsage::None;
        RGTexture scene = graph.createTexture(sd);
        RGTexture out = graph.importTexture(*device, output, /*finalAccess*/ Access::TransferRead);

        graph.addPass("Scene")
            .color(scene, VK_ATTACHMENT_LOAD_OP_CLEAR, ClearColor::rgba(0, 0, 0, 1))
            .execute([&](PassContext& ctx) { draw(ctx, {1.f, 0.5f, 0.25f, 1.f}, kInvalidBindlessIndex); });
        graph.addPass("Tint")
            .read(scene, Access::SampledFragment)
            .color(out)
            .execute([&](PassContext& ctx) { draw(ctx, {0.5f, 1.f, 1.f, 1.f}, ctx.sampledIndex(scene)); });

        graph.execute(*device); // барьеры, метки, timestamps, submit
        device->endFrame();
    }
    device->waitIdle();

    std::vector<u8> pixels = device->readTexture(output);
    ASSERT_EQ(pixels.size(), usize(W) * H * 4);
    EXPECT_NEAR(int(pixels[0]), 128, 3); // 1.0 * 0.5
    EXPECT_NEAR(int(pixels[1]), 128, 3); // 0.5 * 1.0
    EXPECT_NEAR(int(pixels[2]), 64, 3);  // 0.25 * 1.0

    // Время пассов последнего завершённого кадра (для оверлея статистики).
    if (device->caps().timestampQueries) {
        bool found = false;
        for (const GpuTiming& t : device->gpuTimings()) found |= t.name == "Tint";
        EXPECT_TRUE(found);
    }
    EXPECT_FALSE(device->memoryStats().heaps.empty());

    graph.releaseResources(*device); // до уничтожения Device!
    device->destroy(tint);
    device->destroy(output);
}

TEST_F(GuideRhiDevice, ShaderHotReload) {
    const fs::path dir = fs::temp_directory_path() / "oxwald_guide_rhi_hotreload";
    fs::create_directories(dir);
    auto write = [](const fs::path& p, const char* text) {
        std::ofstream(p, std::ios::trunc) << text;
        // Сдвигаем mtime явно: на некоторых ФС его разрешение — секунды.
        fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(2));
    };
    write(dir / "value.glsl", "#define VALUE 1u\n");
    write(dir / "value.comp", R"(#version 460
#include <common/bindless.glsl>
#include "value.glsl"
layout(local_size_x = 1) in;
OX_BUFFER(Out, { uint v; });
OX_PUSH_CONSTANTS({ Out o; });
void main() { pc.o.v = VALUE; }
)");

    BufferHandle out = device->createBuffer({4, BufferUsage::Storage, MemoryUsage::Readback, "guide.hotreload"});
    ComputePipelineDesc cd;
    cd.name = "guide.hotreload";
    cd.shader = ShaderStageDesc::file(dir / "value.comp"); // из файла => пайплайн перезагружаемый
    PipelineHandle p = device->createComputePipeline(cd);
    ASSERT_NE(device->vkPipeline(p), VK_NULL_HANDLE) << device->lastPipelineError();

    auto run = [&] {
        device->immediateSubmit([&](CommandList& cmd) {
            cmd.bindPipeline(p);
            cmd.pushConstants(device->address(out));
            cmd.dispatch(1);
            cmd.memoryBarrier(Access::StorageWriteCompute, Access::HostRead);
        });
        u32 v = 0;
        std::memcpy(&v, device->readBuffer(out).data(), 4);
        return v;
    };
    EXPECT_EQ(run(), 1u);

    write(dir / "value.glsl", "#define VALUE 2u\n"); // правим include, а не сам шейдер
    EXPECT_EQ(device->reloadChangedShaders(/*force*/ true), 1u); // обычно это делает beginFrame()
    EXPECT_EQ(device->pipelineVersion(p), 2u) << "handle тот же, VkPipeline новый";
    EXPECT_EQ(run(), 2u);

    write(dir / "value.glsl", "#define VALUE oops\n"); // ошибка: продолжает работать старый пайплайн
    EXPECT_EQ(device->reloadChangedShaders(true), 0u);
    EXPECT_EQ(run(), 2u);

    device->destroy(p);
    device->destroy(out);
    fs::remove_all(dir);
}

TEST_F(GuideRhiDevice, RayTracingOnlyWithCaps) {
    if (!device->caps().rayTracingSupported()) {
        // API не падает, а возвращает невалидный handle и пишет причину в лог.
        EXPECT_FALSE(device->createBlas({}).valid());
        GTEST_SKIP() << device->caps().whyRayTracingUnavailable();
    }
    const f32 verts[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    BufferHandle vb = device->createBuffer({sizeof(verts), BufferUsage::AccelStructInput, MemoryUsage::GpuOnly, "guide.as.vb"}, verts);

    BlasDesc bd;
    bd.name = "guide.blas";
    BlasTriangles tri;
    tri.vertexBuffer = vb;
    tri.vertexCount = 3; // без индексного буфера
    bd.geometries.push_back(tri);
    AccelStructHandle blas = device->createBlas(bd); // строится сразу (+ compaction)
    ASSERT_TRUE(blas.valid());

    TlasDesc td;
    td.name = "guide.tlas";
    td.maxInstances = 16;
    AccelStructHandle tlas = device->createTlas(td);
    TlasInstance inst;
    inst.blas = blas;
    device->immediateSubmit([&](CommandList& cmd) { cmd.buildTlas(tlas, {&inst, 1}); });
    EXPECT_NE(device->accelStructAddress(tlas), 0u); // в GLSL: accelerationStructureEXT(addr)

    device->waitIdle();
    device->destroy(tlas);
    device->destroy(blas);
    device->destroy(vb);
}
