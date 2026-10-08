#pragma once

// Qt-free renderer for editor viewports. The editor wraps it into its IViewportRenderer with the header-only
// adapter in editor_viewport_adapter.hpp (include it from editor code, where viewport/viewport_renderer.hpp resolves):
//
//   #include <oxwald/render/editor_viewport_adapter.hpp>
//   services.setViewportRendererFactory([](rhi::Device& d) { return ox::render::makeEditorViewportRenderer(d); });
//
// The editor owns the device frame and the swapchain: render() records the whole view into the editor's command
// list and leaves the backbuffer in ColorAttachmentWrite (tracked) so the editor can transition(Present).

#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/render/renderer.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace ox {
class World;
}
namespace ox::rhi {
class CommandList;
}

namespace ox::render {

struct EditorViewportFrame {
    const World* world = nullptr; // edit or play world (read only; extracted every frame on the UI thread)
    CameraParams camera;
    DebugView debugView = DebugView::None;
    bool wireframe = false;
    bool grid = true;
    bool debugDraw = true;
    bool shadows = true;
    bool selectionOutline = true;
    std::span<const Uuid> selection;
    std::span<const Uuid> hidden;
    const DebugDraw* lines = nullptr; // already flushed by the editor
    f64 time = 0.0;
    f32 dt = 0.0f;
};

class EditorViewportRenderer {
public:
    explicit EditorViewportRenderer(rhi::Device& device, const RendererDesc& desc = {});
    ~EditorViewportRenderer();

    Renderer& renderer() { return *m_renderer; }

    // Records into `cmd` (the caller's frame on the graphics queue) targeting `target` (e.g. the acquired swapchain
    // image). The caller owns Device::beginFrame/endFrame and the submit.
    void render(const EditorViewportFrame& frame, rhi::TextureHandle target, rhi::CommandList& cmd);
    // Offscreen frame with its own device frame + submit (thumbnails, tests). Returns RGBA8 pixels. Honours the
    // frame's grid/debugDraw/selectionOutline flags like render().
    std::vector<u8> renderToImage(const EditorViewportFrame& frame, u32 width, u32 height);
    // Synchronous ID-buffer pick at an output pixel of a viewport of `viewportSize`. Renders one offscreen frame
    // (call outside the editor's device frame). nullopt = nothing under the cursor.
    std::optional<Uuid> pick(const EditorViewportFrame& frame, Extent2D viewportSize, glm::ivec2 pixel);
    [[nodiscard]] const RenderStats& stats() const { return m_renderer->stats(); }
    // Baked probe / irradiance volume files (reflections::installBakedData) for the scenes this viewport shows,
    // e.g. reading <project>/Baked/<file>. resetBakedData() re-reads them (after a bake or project switch).
    void setBakedDataReader(std::function<std::optional<std::vector<u8>>(const std::string& fileName)> reader);
    void resetBakedData() { m_bakedAttempted.clear(); }

private:
    void buildSnapshot(const EditorViewportFrame& frame);
    void applyViewFlags(const EditorViewportFrame& frame);
    RenderSettings settingsFor(const EditorViewportFrame& frame) const;
    rhi::TextureHandle offscreen(u32 width, u32 height);

    rhi::Device* m_device;
    std::unique_ptr<Renderer> m_renderer;
    ViewId m_view = 0;
    RenderSnapshot m_snapshot;
    rhi::TextureHandle m_offscreen;
    std::function<std::optional<std::vector<u8>>(const std::string&)> m_bakedReader;
    std::unordered_set<Uuid> m_bakedAttempted;
};

} // namespace ox::render
