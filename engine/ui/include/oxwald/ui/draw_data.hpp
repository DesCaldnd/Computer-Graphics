#pragma once

// UI draw data shared by the ImGui layer and the RmlUi game UI, and the bridge that carries it from the game thread
// (where both libraries build their frames) to the render thread (where UiOverlayFeature draws it).
//
//   game thread                                  render thread
//   ImGuiLayer / GameUI ──► UiFrame ──publish──► UiRenderBridge ──latest()──► UiOverlayFeature (Overlay point)
//   textures: UiTextureStore (CPU copies, versioned) ──────────────────────► synced to rhi textures on demand
//   render introspection (stats, caps, render graph) ◄──────────────────────── written by the feature each frame

#include <oxwald/core/types.hpp>
#include <oxwald/render/quality.hpp>
#include <oxwald/render/render_stats.hpp>
#include <oxwald/rhi/device_caps.hpp>
#include <oxwald/rhi/profiling.hpp>

#include <glm/glm.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::ui {

// Same layout as ImDrawVert (pos, uv, packed RGBA8 colour with R in the low byte).
struct UiVertex {
    glm::vec2 pos{0.0f};
    glm::vec2 uv{0.0f};
    u32 color = 0xFFFFFFFFu;
};
static_assert(sizeof(UiVertex) == 20);

// Texture reference of a draw command:
//   0                       no texture (white)
//   1 .. 2^32-1             UiTextureStore id (font atlases, RmlUi images, ...)
//   kBindlessTextureBit | i raw bindless sampled-image index (render targets, asset textures shown in ImGui)
using UiTextureRef = u64;
inline constexpr UiTextureRef kBindlessTextureBit = 1ull << 63;
[[nodiscard]] constexpr UiTextureRef bindlessTexture(u32 sampledIndex) { return kBindlessTextureBit | sampledIndex; }
[[nodiscard]] constexpr bool isBindlessTexture(UiTextureRef ref) { return (ref & kBindlessTextureBit) != 0; }

enum UiDrawFlags : u32 {
    kUiPremultiplied = 1u << 0, // vertex colour and texture are premultiplied (RmlUi); otherwise straight alpha (ImGui)
};

struct UiDrawCmd {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    i32 vertexOffset = 0;
    UiTextureRef texture = 0;
    glm::ivec4 clip{0, 0, 0, 0}; // x0, y0, x1, y1 in frame pixels (x1/y1 exclusive)
    glm::vec4 xform{1.0f, 1.0f, 0.0f, 0.0f}; // pixel = pos * xy + zw (before `transform`)
    i32 transform = -1;          // index into UiFrame::transforms (pixel space -> pixel space, may be projective)
    u32 flags = 0;
};

// One complete UI frame in frame pixels (= output resolution of the view it is drawn into; rescaled if they differ).
struct UiFrame {
    glm::uvec2 size{0, 0};
    std::vector<UiVertex> vertices;
    std::vector<u32> indices;
    std::vector<UiDrawCmd> commands;
    std::vector<glm::mat4> transforms;
    u64 serial = 0;

    [[nodiscard]] bool empty() const { return commands.empty(); }
    void clear();
    // Appends geometry and a command that references it (indices are relative to the appended vertices).
    UiDrawCmd& add(std::span<const UiVertex> vertices, std::span<const u32> indices, const UiDrawCmd& cmd);
};

// CPU side of every UI texture. Thread-safe. Pixels are RGBA8; the render thread uploads entries whose version
// changed and destroys GPU textures whose entries disappeared.
class UiTextureStore {
public:
    struct Entry {
        u32 width = 0, height = 0;
        std::shared_ptr<const std::vector<u8>> pixels;
        u64 version = 0;
        std::string name;
    };

    u32 create(u32 width, u32 height, std::vector<u8> rgba, std::string name = {});
    bool update(u32 id, u32 width, u32 height, std::vector<u8> rgba);
    void destroy(u32 id);
    [[nodiscard]] bool contains(u32 id) const;
    [[nodiscard]] std::optional<Entry> get(u32 id) const;
    [[nodiscard]] usize size() const;
    // Increments on every create/update/destroy (cheap "anything changed?" check for the render thread).
    [[nodiscard]] u64 version() const { return m_version.load(std::memory_order_acquire); }
    // Copy of the table (shared pixel buffers; no pixel copies).
    [[nodiscard]] std::unordered_map<u32, Entry> snapshot() const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<u32, Entry> m_entries;
    u32 m_nextId = 1;
    std::atomic<u64> m_version{0};
};

// Introspection published by the render thread for the debug windows (game thread).
struct RenderGraphPassInfo {
    std::string name;
    std::string queue;
    u32 batch = 0;
    u32 barriers = 0;
    bool culled = false;
    f64 gpuMs = -1.0; // from RenderStats when available
};
struct RenderGraphResourceInfo {
    std::string name;
    bool texture = true;
    bool imported = false;
    bool used = false;
    i32 aliasSlot = -1;
    u64 size = 0;
    u32 firstPass = ~0u, lastPass = 0; // execution-order indices
};
struct RenderGraphInfo {
    std::string view;
    std::vector<RenderGraphPassInfo> passes; // execution order, then culled passes
    std::vector<RenderGraphResourceInfo> resources;
    u32 batches = 0;
    u64 transientBytesUnaliased = 0, transientBytesAliased = 0;
    u64 frame = 0;
    std::string graphviz;
};

struct UpscalerAvailability {
    std::string name;   // "Off", "FSR1", "DLSS" (r.Upscaler enum names)
    bool available = true;
    std::string reason; // why it is unavailable
};
// From DeviceCaps (until render exposes its own query): FSR1 everywhere, DLSS only on NVIDIA RTX under Windows/Linux.
[[nodiscard]] std::vector<UpscalerAvailability> upscalerAvailability(const rhi::DeviceCaps& caps);

struct RenderInfo {
    bool valid = false;
    u64 frame = 0;
    render::RenderStats stats;
    rhi::GpuMemoryStats memory;
    rhi::DeviceCaps caps;
    std::vector<UpscalerAvailability> upscalers;
    glm::uvec2 outputSize{0, 0};
    glm::uvec2 renderSize{0, 0};
};

class UiRenderBridge {
public:
    // ---- game thread -> render thread ----
    void publish(std::shared_ptr<const UiFrame> frame);
    [[nodiscard]] std::shared_ptr<const UiFrame> latest() const;
    UiTextureStore& textures() { return m_textures; }
    const UiTextureStore& textures() const { return m_textures; }
    // Also draw into editor views (ViewFlags::editor). Off by default: the game UI belongs to game views.
    std::atomic<bool> drawInEditorViews{false};

    // ---- render thread -> game thread ----
    void setRenderInfo(RenderInfo info);
    [[nodiscard]] RenderInfo renderInfo() const;
    [[nodiscard]] bool hasRenderInfo() const { return m_hasInfo.load(std::memory_order_acquire); }
    // The render graph viewer sets this while open; the feature then captures the compiled plan every frame.
    std::atomic<bool> captureRenderGraph{false};
    void setRenderGraph(RenderGraphInfo info);
    [[nodiscard]] std::optional<RenderGraphInfo> renderGraph() const;

    // Quality auto-detection (settings menu "Auto"): requested on the game thread, run between frames by the
    // renderer decorator (withUi), result applied on the game thread.
    std::atomic<bool> autoDetectRequested{false};
    void setBenchmark(render::BenchmarkResult result);
    [[nodiscard]] std::optional<render::BenchmarkResult> takeBenchmark();

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const UiFrame> m_frame;
    UiTextureStore m_textures;
    RenderInfo m_info;
    std::atomic<bool> m_hasInfo{false};
    std::optional<RenderGraphInfo> m_graph;
    std::optional<render::BenchmarkResult> m_benchmark;
};

} // namespace ox::ui
