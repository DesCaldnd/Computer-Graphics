// Swapchain smoke test: GLFW window + rhi Device/Swapchain + RenderGraph, animated clear + a triangle.
//   rhi_window_smoke [--frames N] [--present vsync|mailbox|immediate] [--no-validation] [--resize]
#include <oxwald/core/log.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/glfw_surface.hpp>
#include <oxwald/rhi/render_graph.hpp>
#include <oxwald/rhi/swapchain.hpp>

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace ox;
using namespace ox::rhi;

namespace {

const char* kVert = R"(#version 460
#include <common/bindless.glsl>
OX_PUSH_CONSTANTS({ float time; });
layout(location = 0) out vec3 color;
void main() {
    const vec2 pos[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
    const vec3 col[3] = vec3[](vec3(1, 0.2, 0.2), vec3(0.2, 1, 0.2), vec3(0.2, 0.4, 1));
    float c = cos(pc.time), s = sin(pc.time);
    vec2 p = pos[gl_VertexIndex];
    gl_Position = vec4(c * p.x - s * p.y, s * p.x + c * p.y, 0.0, 1.0);
    color = col[gl_VertexIndex];
}
)";

const char* kFrag = R"(#version 460
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(color, 1.0); }
)";

} // namespace

int main(int argc, char** argv) {
    u32 frames = 120;
    PresentMode mode = PresentMode::VSync;
    bool validation = true;
    bool resize = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = u32(std::stoul(argv[++i]));
        else if (!std::strcmp(argv[i], "--present") && i + 1 < argc) {
            const std::string m = argv[++i];
            mode = m == "mailbox" ? PresentMode::Mailbox : m == "immediate" ? PresentMode::Immediate : PresentMode::VSync;
        } else if (!std::strcmp(argv[i], "--no-validation")) validation = false;
        else if (!std::strcmp(argv[i], "--resize")) resize = true;
    }

    if (!initGlfwVulkan()) return 2;
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 2;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(960, 540, "OxwaldEngine rhi smoke", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        return 2;
    }
    u32 validationErrors = 0;
    {
        GlfwSurfaceProvider surface(window);
        DeviceDesc desc;
        desc.appName = "rhi_window_smoke";
        desc.surface = &surface;
        desc.validation = validation;
        desc.framesInFlight = 2;
        std::string error;
        auto device = Device::create(desc, &error);
        if (!device) {
            std::fprintf(stderr, "device: %s\n", error.c_str());
            return 1;
        }
        std::printf("%s\n", device->caps().toString().c_str());
        SwapchainDesc sd;
        sd.presentMode = mode;
        auto swapchain = Swapchain::create(*device, sd);
        if (!swapchain) return 1;

        GraphicsPipelineDesc gd;
        gd.name = "smoke.triangle";
        gd.vertex = ShaderStageDesc::glsl(kVert, ShaderStage::Vertex, "smoke.vert");
        gd.fragment = ShaderStageDesc::glsl(kFrag, ShaderStage::Fragment, "smoke.frag");
        gd.colorFormats = {swapchain->format()};
        PipelineHandle pipeline = device->createGraphicsPipeline(gd);
        if (!device->vkPipeline(pipeline)) {
            std::fprintf(stderr, "pipeline: %s\n", device->lastPipelineError().c_str());
            return 1;
        }

        RenderGraph graph;
        const auto start = std::chrono::steady_clock::now();
        u32 presented = 0;
        for (u32 frame = 0; frame < frames && !glfwWindowShouldClose(window); ++frame) {
            glfwPollEvents();
            if (resize && frame == frames / 2) glfwSetWindowSize(window, 640, 480);
            device->beginFrame();
            if (!swapchain->acquire()) {
                device->endFrame();
                continue;
            }
            const f32 t = f32(frame) / 60.f;
            const VkExtent2D ext = swapchain->extent();
            graph.reset();
            TextureDesc bbDesc = device->desc(swapchain->currentTexture());
            RGTexture backbuffer = graph.importTexture(swapchain->currentTexture(), bbDesc, {Access::Undefined, Access::Present});
            graph.addPass("Clear+Triangle")
                .color(backbuffer, VK_ATTACHMENT_LOAD_OP_CLEAR,
                       ClearColor::rgba(0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t + 2.1f), 0.5f + 0.5f * std::sin(t + 4.2f)))
                .execute([&](PassContext& ctx) {
                    ctx.cmd.bindPipeline(pipeline);
                    ctx.cmd.pushConstants(t);
                    ctx.cmd.draw(3);
                });
            RGExecuteOptions eo;
            eo.swapchain = swapchain.get();
            graph.execute(*device, eo);
            swapchain->present();
            device->endFrame();
            ++presented;
            if (frame % 60 == 0) {
                std::printf("frame %u: %ux%u, gpu passes: %zu\n", frame, ext.width, ext.height, device->gpuTimings().size());
            }
        }
        device->waitIdle();
        const f64 secs = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
        std::printf("presented %u frames in %.2fs (%.1f fps), present mode %d\n", presented, secs, presented / secs,
                    int(swapchain->presentMode()));
        graph.releaseResources(*device);
        device->destroy(pipeline);
        swapchain.reset();
        device.reset();
        validationErrors = Device::validationErrorCount();
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    std::printf("validation errors: %u\n", validationErrors);
    return validationErrors == 0 ? 0 : 3;
}
