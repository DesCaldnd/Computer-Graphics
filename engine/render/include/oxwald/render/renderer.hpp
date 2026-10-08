#pragma once

// Renderer: owns the GPU scene, the resource cache, the feature registry and the views; builds and executes one
// render graph per view per frame (clustered forward+, see render_feature.hpp for the frame order).
//
//   auto renderer = ox::render::Renderer::create(device);
//   ViewId vp = renderer->createView({.name = "Main"});
//   loop:
//     device.beginFrame();  swapchain->acquire();
//     renderer->beginFrame(snapshot);                                   // settings, uploads, instances
//     renderer->renderView({.view = vp, .camera = cam, .target = {.swapchain = swapchain.get()}});
//     renderer->endFrame();                                             // stats
//     swapchain->present();  device.endFrame();
//
// The device frame (beginFrame/endFrame, acquire/present) belongs to the caller: the runtime's render thread, the
// editor viewport, or a test.

#include <oxwald/render/frame_resources.hpp>
#include <oxwald/render/gpu_resource_cache.hpp>
#include <oxwald/render/gpu_scene.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/render_settings.hpp>
#include <oxwald/render/render_stats.hpp>
#include <oxwald/render/render_view.hpp>
#include <oxwald/render/snapshot.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace ox {
class JobSystem;
}
namespace ox::rhi {
class Device;
class Swapchain;
} // namespace ox::rhi

namespace ox::render {

struct RendererDesc {
    JobSystem* jobs = nullptr;         // asset loading on worker threads (optional)
    bool builtinFeatures = true;       // Shadows, IBL, Sky, Tonemap helpers, debug draw, editor overlays, ...
    bool instantiateFactories = true;  // create every feature from registerFeatureFactory()
};

struct RenderTarget {
    rhi::TextureHandle texture;               // offscreen colour target (UNORM or SRGB), or
    rhi::Swapchain* swapchain = nullptr;      // the swapchain's current image (acquired by the caller)
    rhi::Access finalAccess = rhi::Access::SampledFragment; // offscreen: state after the frame
};

struct ViewRenderRequest {
    ViewId view = 0;
    CameraParams camera;
    RenderTarget target;
    std::optional<RenderSettings> settingsOverride; // e.g. an editor viewport with its own debug view
    // Record the whole view into this command list (graphics queue, single queue) instead of submitting; the
    // caller submits it (editor viewport: its frame command list with the swapchain semaphores).
    rhi::CommandList* recordInto = nullptr;
};

struct PickResult {
    bool ready = false;
    u32 x = 0, y = 0, width = 0, height = 0; // render-resolution rect that was read
    std::vector<u32> ids;                    // row-major entity ids (encodeEntityId values; 0 = none)
    // Most frequent non-zero id in the rect (0 if none).
    [[nodiscard]] u32 dominant() const;
    // Unique non-zero ids.
    [[nodiscard]] std::vector<u32> unique() const;
};

using PickRequestId = u32;

class Renderer {
public:
    static std::unique_ptr<Renderer> create(rhi::Device& device, const RendererDesc& desc = {});
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    rhi::Device& device();
    FeatureRegistry& features();
    GpuScene& scene();
    GpuResourceCache& resources();

    ViewId createView(const ViewDesc& desc = {});
    void destroyView(ViewId view);
    RenderView* view(ViewId view);

    // --- frame ---
    void beginFrame(const RenderSnapshot& snapshot);
    void renderView(const ViewRenderRequest& request);
    void endFrame();
    // beginFrame + renderView for each + endFrame.
    void render(const RenderSnapshot& snapshot, std::span<const ViewRenderRequest> views);

    [[nodiscard]] const RenderSettings& settings() const; // snapshot of this frame
    [[nodiscard]] const RenderStats& stats() const;        // last completed frame (GPU timings lag by the frames in flight)

    // --- picking (editor) ---
    // Output-pixel rect of a view; read back when that view renders next. Requires ViewFlags::editor.
    PickRequestId requestPick(ViewId view, u32 x, u32 y, u32 width = 1, u32 height = 1);
    // Returns the result once the GPU finished (ready == true), else ready == false. Results are kept until taken.
    PickResult takePickResult(PickRequestId id);

    // Rebuilds IBL / caches on the next frame (environment edited in place, tooling).
    void invalidateEnvironment();
    // Releases transient graph memory of all views (e.g. after a big resize).
    void trimMemory();

    struct Impl;
    Impl& impl() { return *m_impl; }

private:
    Renderer();
    std::unique_ptr<Impl> m_impl;
};

// Registers built-in render features with a Renderer (called by create() when RendererDesc::builtinFeatures).
void registerBuiltinFeatures(Renderer& renderer);

} // namespace ox::render
