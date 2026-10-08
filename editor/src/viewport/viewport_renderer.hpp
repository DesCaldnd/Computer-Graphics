#pragma once

#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/core/uuid.hpp>

#include <QImage>
#include <QString>

#include <memory>
#include <span>
#include <string>
#include <vector>

class QPainter;

namespace ox {
class World;
namespace rhi {
class Device;
class Swapchain;
class CommandList;
} // namespace rhi
} // namespace ox

// Contract between the editor viewport and whoever draws it. The editor ships a software implementation
// (PainterViewportRenderer: grid, debug-draw lines, gizmos, placeholder shaded boxes) and, when the rhi module is
// present, a Vulkan swapchain path. The render module implements IViewportRenderer for real frames, picking and
// thumbnails and registers it with EditorServices::setViewportRendererFactory().
namespace ox::editor {

enum class ViewMode : u8 {
    Lit,
    Unlit,
    Wireframe,
    LightingOnly,
    Normals,
    Overdraw,
    BufferBaseColor,
    BufferRoughness,
    BufferMetallic,
    BufferDepth,
    BufferMotion,
    Count
};
[[nodiscard]] QString viewModeName(ViewMode mode);
[[nodiscard]] bool isBufferView(ViewMode mode);

struct ShowFlags {
    bool grid = true;
    bool gizmos = true;
    bool icons = true;
    bool debugDraw = true;
    bool bounds = false;
    bool lights = true;
    bool cameras = true;
    bool fog = true;
    bool shadows = true;
    bool postProcess = true;
    bool stats = false;
};

struct ViewportCamera {
    glm::vec3 position{6.0f, 4.5f, 8.0f};
    f32 yaw = glm::radians(37.0f);    // around +Y; 0 looks down -Z
    f32 pitch = glm::radians(-22.0f); // around local X
    f32 verticalFovDeg = 60.0f;
    f32 nearPlane = 0.05f;
    f32 farPlane = 2000.0f;
    bool orthographic = false;
    f32 orthoHeight = 10.0f;

    [[nodiscard]] glm::quat rotation() const;
    [[nodiscard]] glm::vec3 forward() const;
    [[nodiscard]] glm::vec3 right() const;
    [[nodiscard]] glm::vec3 up() const;
    [[nodiscard]] glm::mat4 view() const;
    // Reversed-Z (near = 1, far = 0) like the engine renderer.
    [[nodiscard]] glm::mat4 projection(f32 aspect) const;
};

// Everything a renderer needs for one editor view.
struct ViewportFrame {
    World* world = nullptr; // edit world, or the play world while playing
    ViewportCamera camera;
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 viewProjection{1.0f};
    glm::uvec2 sizePx{1, 1};
    f32 devicePixelRatio = 1.0f;
    ViewMode viewMode = ViewMode::Lit;
    ShowFlags showFlags;
    std::span<const Uuid> selection;
    std::span<const Uuid> hidden; // editor-only hidden entities (outliner eye)
    // Editor overlay geometry (grid, gizmos, selection bounds, light/camera shapes, gameplay debug draw), already
    // flushed for this frame: draw depthTestedLines() with depth test and overlayLines() on top.
    const DebugDraw* lines = nullptr;
    glm::vec4 selectionColor{1.0f, 0.62f, 0.1f, 1.0f};
    glm::vec4 clearColor{0.08f, 0.09f, 0.11f, 1.0f};
    bool playMode = false;
    f64 time = 0.0;
    f32 dt = 0.0f;
};

// Where to draw. Software path: painter. Vulkan path: device + swapchain + a command list recording this frame
// (the backbuffer is swapchain->currentTexture()).
struct ViewportTarget {
    QPainter* painter = nullptr;
    rhi::Device* device = nullptr;
    rhi::Swapchain* swapchain = nullptr;
    rhi::CommandList* commandList = nullptr;
};

struct GpuPassTiming {
    std::string name;
    f32 milliseconds = 0.0f;
};

class IViewportRenderer {
public:
    virtual ~IViewportRenderer() = default;
    [[nodiscard]] virtual QString name() const = 0;
    // True if this renderer draws into a QPainter (software) rather than a Vulkan swapchain.
    [[nodiscard]] virtual bool usesPainter() const = 0;
    [[nodiscard]] virtual bool supportsViewMode(ViewMode mode) const { return !isBufferView(mode); }
    virtual void render(const ViewportFrame& frame, const ViewportTarget& target) = 0;
    // ID-buffer picking. Return false when unsupported; the editor then picks on the CPU (bounds/ray tests).
    virtual bool pick(const ViewportFrame& frame, glm::ivec2 pixel, Uuid& out) {
        (void)frame;
        (void)pixel;
        (void)out;
        return false;
    }
    // Per-pass GPU times of the last finished frame (Stats panel).
    [[nodiscard]] virtual std::vector<GpuPassTiming> passTimings() const { return {}; }
};

// Content browser thumbnails (materials, meshes, prefabs). Called on the UI thread; results are cached.
struct ThumbnailRequest {
    Uuid asset;
    QString path;
    QString type;
    int sizePx = 128;
};
class IThumbnailRenderer {
public:
    virtual ~IThumbnailRenderer() = default;
    // Null image: the browser keeps its placeholder tile.
    [[nodiscard]] virtual QImage render(const ThumbnailRequest& request) = 0;
};

} // namespace ox::editor
