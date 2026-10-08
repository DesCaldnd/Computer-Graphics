#pragma once

#include "viewport/viewport_renderer.hpp"

#include <QObject>

#include <memory>
#include <vector>

namespace ox::editor {

class EditorContext;

#if OX_EDITOR_HAS_RENDER
class GpuViewportRenderer;

// Render module integration (engine/render): the GPU IViewportRenderer created on the viewport's Vulkan device
// (render::EditorViewportAdapter), its AssetManager provider + hot reload (re-wired whenever the engine restarts),
// mesh/material/model thumbnails rendered offscreen and render::autoDetectQuality as the quality benchmark.
class RenderIntegration : public QObject {
    Q_OBJECT
public:
    explicit RenderIntegration(EditorContext& ctx);
    ~RenderIntegration() override;

    std::unique_ptr<IViewportRenderer> createRenderer(rhi::Device& device);
    void rendererDestroyed(GpuViewportRenderer* r);
    // The most recently created live renderer (thumbnails), or null.
    [[nodiscard]] GpuViewportRenderer* live() const { return m_renderers.empty() ? nullptr : m_renderers.back(); }
    [[nodiscard]] EditorContext& context() const { return m_ctx; }

private:
    void wireAssets(GpuViewportRenderer& r);
    void unwireAssets();

    EditorContext& m_ctx;
    std::vector<GpuViewportRenderer*> m_renderers;
};
#endif

} // namespace ox::editor
