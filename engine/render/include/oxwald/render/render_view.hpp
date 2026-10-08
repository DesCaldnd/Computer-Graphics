#pragma once

// RenderView: persistent per-viewport state (camera, render vs output resolution, sub-pixel jitter, previous-frame
// matrices, history textures, per-feature state, its render graph). Created through Renderer::createView().

#include <oxwald/core/math.hpp>
#include <oxwald/render/render_types.hpp>
#include <oxwald/rhi/handles.hpp>
#include <oxwald/rhi/render_graph.hpp>

#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>

namespace ox {
struct CameraComponent;
}

namespace ox::render {

class IRenderFeature;

// Halton low-discrepancy sequence (index starts at 1 for a non-zero first sample).
[[nodiscard]] f32 halton(u32 index, u32 base);
// Sub-pixel jitter in pixels, in (-0.5, 0.5]², Halton(2,3) with `phases` samples (8 or 16 typical).
[[nodiscard]] glm::vec2 haltonJitter(u64 frame, u32 phases);

struct CameraParams {
    enum class Projection : u8 { Perspective, Orthographic };
    glm::mat4 world{1.0f}; // camera to world (camera looks down -Z, Y up)
    Projection projection = Projection::Perspective;
    f32 verticalFov = glm::radians(60.0f); // radians
    f32 orthographicHeight = 10.0f;        // full height in metres
    f32 nearPlane = 0.1f;
    f32 farPlane = 1000.0f; // <= 0: infinite (perspective)
    f32 ev100 = 15.0f;      // physical camera exposure (CameraComponent::ev100())

    static CameraParams fromComponent(const CameraComponent& camera, const glm::mat4& world);
    static CameraParams lookAt(glm::vec3 eye, glm::vec3 target, f32 fovDegrees = 60.0f, f32 nearPlane = 0.1f,
                               f32 farPlane = 1000.0f, glm::vec3 up = {0, 1, 0});
    [[nodiscard]] glm::vec3 position() const { return glm::vec3(world[3]); }
    // Reversed-Z, Vulkan Y flip (NDC y down).
    [[nodiscard]] glm::mat4 projectionMatrix(f32 aspect) const;
    [[nodiscard]] glm::mat4 viewMatrix() const { return glm::inverse(world); }
};

// What a viewport shows (editor show flags).
struct ViewFlags {
    bool editor = false;     // EntityID buffer, selection outline, grid
    bool grid = false;
    bool debugDraw = true;
    bool selectionOutline = true;
    bool overlays = true;    // Overlay injection point (UI)
};

struct ViewDesc {
    std::string name = "View";
    ViewFlags flags;
};

using ViewId = u32;

// Per-feature, per-view persistent state: derive and fetch with FeatureContext::viewState<T>().
struct IFeatureViewState {
    virtual ~IFeatureViewState() = default;
    virtual void release(rhi::Device& device) {}
};

// Persistent ping-pong pair managed by the view (TAA, SSR, volumetrics...). `previous` is last frame's `current`.
struct HistoryTexture {
    rhi::RGTexture current;   // write this frame (imported, contents undefined)
    rhi::RGTexture previous;  // read last frame's result
    bool previousValid = false; // false on the first frame / after resize / camera cut
};

class RenderView {
public:
    RenderView(ViewId id, ViewDesc desc);
    ~RenderView();
    RenderView(const RenderView&) = delete;
    RenderView& operator=(const RenderView&) = delete;

    [[nodiscard]] ViewId id() const { return m_id; }
    [[nodiscard]] const ViewDesc& desc() const { return m_desc; }
    ViewDesc& desc() { return m_desc; }

    // Valid during a frame.
    [[nodiscard]] const CameraParams& camera() const { return m_camera; }
    [[nodiscard]] Extent2D renderExtent() const { return m_renderExtent; }
    [[nodiscard]] Extent2D outputExtent() const { return m_outputExtent; }
    [[nodiscard]] u64 frameIndex() const { return m_frameIndex; } // frames rendered by this view
    [[nodiscard]] glm::vec2 jitterPixels() const { return m_jitter; }
    [[nodiscard]] glm::vec2 jitterNdc() const;
    [[nodiscard]] f32 mipBias() const { return m_mipBias; } // material texture bias of this frame (upscalers)
    [[nodiscard]] bool cameraCut() const { return m_cameraCut; }

    // Matrices (Vulkan Y flip, reversed-Z). "Unjittered" is what motion vectors use.
    [[nodiscard]] const glm::mat4& viewMatrix() const { return m_view; }
    [[nodiscard]] const glm::mat4& projMatrix() const { return m_proj; }
    [[nodiscard]] const glm::mat4& unjitteredProj() const { return m_unjitteredProj; }
    [[nodiscard]] const glm::mat4& viewProj() const { return m_viewProj; }
    [[nodiscard]] const glm::mat4& unjitteredViewProj() const { return m_unjitteredViewProj; }
    [[nodiscard]] const glm::mat4& prevViewProj() const { return m_prevViewProj; }
    [[nodiscard]] const glm::mat4& prevUnjitteredViewProj() const { return m_prevUnjitteredViewProj; }
    [[nodiscard]] Frustum frustum() const { return Frustum::fromViewProj(m_unjitteredViewProj, true); }

    rhi::RenderGraph& graph() { return m_graph; }
    void requestCameraCut() { m_cameraCutRequested = true; }

    struct Impl;
    Impl& impl() { return *m_impl; }

private:
    friend class Renderer;
    friend class FeatureContext;

    ViewId m_id;
    ViewDesc m_desc;
    CameraParams m_camera;
    Extent2D m_renderExtent;
    Extent2D m_outputExtent;
    u64 m_frameIndex = 0;
    glm::vec2 m_jitter{0.0f};
    glm::vec2 m_prevJitter{0.0f};
    f32 m_mipBias = 0.0f;
    bool m_cameraCut = true;
    bool m_cameraCutRequested = false;
    glm::mat4 m_view{1.0f}, m_proj{1.0f}, m_unjitteredProj{1.0f}, m_viewProj{1.0f}, m_unjitteredViewProj{1.0f};
    glm::mat4 m_prevViewProj{1.0f}, m_prevUnjitteredViewProj{1.0f};
    rhi::RenderGraph m_graph;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ox::render
