#pragma once

// Header-only bridge to the editor's IViewportRenderer (editor/src/viewport/viewport_renderer.hpp). Include it from
// editor code only (it needs the editor's include path and Qt). Register with
// EditorServices::setViewportRendererFactory(...) using makeEditorViewportRenderer(device).

#include <oxwald/render/editor_viewport.hpp>
#include <oxwald/rhi/swapchain.hpp>

#include "viewport/viewport_renderer.hpp"

#include <memory>

namespace ox::render {

class EditorViewportAdapter final : public editor::IViewportRenderer {
public:
    explicit EditorViewportAdapter(rhi::Device& device) : m_core(device) {}

    QString name() const override { return QStringLiteral("Vulkan (render)"); }
    bool usesPainter() const override { return false; }
    bool supportsViewMode(editor::ViewMode) const override { return true; }

    void render(const editor::ViewportFrame& frame, const editor::ViewportTarget& target) override {
        if (!target.swapchain || !target.commandList) return;
        m_core.render(convert(frame), target.swapchain->currentTexture(), *target.commandList);
    }

    bool pick(const editor::ViewportFrame& frame, glm::ivec2 pixel, Uuid& out) override {
        const auto hit = m_core.pick(convert(frame), {frame.sizePx.x, frame.sizePx.y}, pixel);
        out = hit.value_or(Uuid{});
        return true; // ID-buffer picking answered (nil = nothing hit)
    }

    std::vector<editor::GpuPassTiming> passTimings() const override {
        std::vector<editor::GpuPassTiming> out;
        for (const PassTiming& p : m_core.stats().passes) out.push_back({p.name, f32(p.gpuMs)});
        return out;
    }

    EditorViewportRenderer& core() { return m_core; }

private:
    static DebugView toDebugView(editor::ViewMode m) {
        using V = editor::ViewMode;
        switch (m) {
        case V::Unlit:
        case V::BufferBaseColor: return DebugView::Albedo;
        case V::Wireframe: return DebugView::Wireframe;
        case V::Normals: return DebugView::Normals;
        case V::Overdraw: return DebugView::Overdraw;
        case V::BufferRoughness: return DebugView::Roughness;
        case V::BufferMetallic: return DebugView::Metallic;
        case V::BufferDepth: return DebugView::Depth;
        case V::BufferMotion: return DebugView::Velocity;
        default: return DebugView::None;
        }
    }

    EditorViewportFrame convert(const editor::ViewportFrame& f) const {
        EditorViewportFrame e;
        e.world = f.world;
        e.camera.world = glm::inverse(f.view);
        e.camera.projection = f.camera.orthographic ? CameraParams::Projection::Orthographic : CameraParams::Projection::Perspective;
        e.camera.verticalFov = glm::radians(f.camera.verticalFovDeg);
        e.camera.orthographicHeight = f.camera.orthoHeight;
        e.camera.nearPlane = f.camera.nearPlane;
        e.camera.farPlane = f.camera.farPlane;
        e.camera.ev100 = 13.0f; // editor default exposure (r.Exposure.* cvars can override)
        e.debugView = toDebugView(f.viewMode);
        e.grid = false; // the editor draws its grid into frame.lines
        e.debugDraw = f.showFlags.debugDraw;
        e.shadows = f.showFlags.shadows;
        e.selection = f.selection;
        e.hidden = f.hidden;
        e.lines = f.lines;
        e.time = f.time;
        e.dt = f.dt;
        return e;
    }

    EditorViewportRenderer m_core;
};

inline std::unique_ptr<editor::IViewportRenderer> makeEditorViewportRenderer(rhi::Device& device) {
    return std::make_unique<EditorViewportAdapter>(device);
}

} // namespace ox::render
