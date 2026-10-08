#pragma once

// Render feature extension API. A feature plugs render graph passes into the frame at injection points, reads and
// publishes named resources through the FrameResources blackboard, and declares its cvars. Raster and ray traced
// variants of an effect are two features in the same exclusive group (exactly one is active).
//
// Frame order (per view):
//   [scene upload] → PreDepth → DepthPrepass (Depth, Normals, Velocity, EntityID) → HiZ → AfterDepth
//   → LightCulling (LightClusters) → Shadows → Lighting → ForwardOpaque (SceneColorHDR) → Sky → AfterOpaque
//   → Translucency → BeforePostProcess → PostProcess (ordered) → Upscale (single slot; default = bilinear
//   resample when render != output resolution) → AfterUpscale → Tonemap (SceneColorLDR) → Overlay → Debug → Output
//
// See docs/dev/modules/render.md for the resource contracts and a complete example feature.

#include <oxwald/core/cvar.hpp>
#include <oxwald/render/frame_resources.hpp>
#include <oxwald/render/gpu_types.hpp>
#include <oxwald/render/render_settings.hpp>
#include <oxwald/render/render_view.hpp>
#include <oxwald/rhi/device_caps.hpp>
#include <oxwald/rhi/pipeline.hpp>

#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

namespace ox::rhi {
class Device;
class CommandList;
} // namespace ox::rhi

namespace ox::render {

class Renderer;
class GpuScene;
struct RenderSnapshot;
struct RenderStats;

enum class InjectionPoint : u8 {
    PreDepth,          // before the depth prepass (GPU culling, previous-frame HiZ occlusion)
    AfterDepth,        // Depth/Normals/Velocity/HiZ ready (SSAO, SSR trace, contact shadows, decals)
    Shadows,           // shadow maps + ShadowMask (ShadowsRaster / ShadowsRT)
    Lighting,          // inputs of the lighting pass: AO, ReflectionsSpecular, IndirectDiffuse, VolumetricFog
    AfterOpaque,       // SceneColorHDR has opaque + sky (SSR composite, refraction source copy)
    Translucency,      // transparent geometry, OIT, particles, water (reuse LightClusters)
    BeforePostProcess, // TAA / temporal accumulation at render resolution
    PostProcess,       // ordered HDR effects at render resolution (bloom, DOF, motion blur, auto exposure)
    Upscale,           // single slot: render → output resolution (FSR1, DLSS); publishes SceneColorHDR
    AfterUpscale,      // output-resolution HDR effects
    Overlay,           // after tonemapping, display-encoded SceneColorLDR (UI, gizmos, editor overlays)
    Debug,             // last: debug visualisations
    Count
};
inline constexpr u32 kInjectionPointCount = u32(InjectionPoint::Count);
const char* injectionPointName(InjectionPoint p);

using InjectionMask = u32;
[[nodiscard]] constexpr InjectionMask maskOf(InjectionPoint p) { return 1u << u32(p); }
template <class... P>
[[nodiscard]] constexpr InjectionMask maskOf(InjectionPoint first, P... rest) {
    return (maskOf(first) | ... | maskOf(rest));
}

// --- scene draw lists ---------------------------------------------------------------------------------------

enum class DrawBucket : u8 { Opaque, Masked, Transparent, Refractive, Count };

// Pipeline variant bits of a batch (index into a 4-entry pipeline array).
enum DrawVariant : u32 { kVariantAlphaTest = 1u << 0, kVariantDoubleSided = 1u << 1, kVariantCount = 4 };

struct DrawBatch {
    u32 meshIndex = 0;     // GpuMeshInfo index
    u32 materialIndex = 0;
    u32 firstInstance = 0; // gl_InstanceIndex base into DrawList::instanceIds
    u32 instanceCount = 0;
    u32 firstIndex = 0;
    u32 indexCount = 0;
    i32 vertexOffset = 0;
    u32 variant = 0;       // DrawVariant bits
    f32 sortDepth = 0.0f;  // view distance (transparent buckets are sorted back to front)
};

// A run of GPU-written VkDrawIndexedIndirectCommands sharing one pipeline variant.
struct DrawIndirectRun {
    u32 firstCommand = 0;
    u32 commandCount = 0; // fixed maximum (commands of culled batches have instanceCount 0)
    u32 variant = 0;      // DrawVariant bits
    u32 countSlot = ~0u;  // index into indirectCountBuffer (drawIndirectCount path), ~0u = none
};

struct DrawList {
    std::vector<DrawBatch> batches;
    VkDeviceAddress instanceIds = 0; // u32[]: GpuInstance index per gl_InstanceIndex
    u32 instanceCount = 0;
    u64 triangleCount = 0;

    // GPU-driven lists (FeatureContext::cullDrawList with r.GpuDriven): batches is empty and the draws are
    // VkDrawIndexedIndirectCommands written by GPU culling (instance/triangle counts are unknown on the CPU).
    // drawBatches() handles both kinds; do not record these draws by hand.
    rhi::BufferHandle indirectBuffer;
    u64 indirectOffset = 0; // byte offset of command 0
    std::vector<DrawIndirectRun> indirectRuns;
    u32 indirectMultiplier = 1; // instanceMultiplier the list was culled for (baked into the commands)
    rhi::BufferHandle indirectCountBuffer; // per-run u32 draw counts (compacted commands), when valid
    u64 indirectCountOffset = 0;

    [[nodiscard]] bool empty() const { return batches.empty() && indirectRuns.empty(); }
    [[nodiscard]] bool gpuDriven() const { return !indirectRuns.empty(); }
};

struct ViewDrawLists {
    DrawList buckets[u32(DrawBucket::Count)];
    [[nodiscard]] const DrawList& operator[](DrawBucket b) const { return buckets[u32(b)]; }
};

// Culling volume for custom draw lists (shadow views, reflection captures, planar reflections).
struct DrawFilter {
    std::optional<Frustum> frustum;
    std::optional<Sphere> sphere;
    u32 requiredInstanceFlags = 0; // e.g. kInstanceCastShadows
    u32 bucketMask = 0xF;          // bit per DrawBucket
    glm::vec3 sortOrigin{0.0f};
    // Mesh LOD selection (FeatureContext::lodSelection() = this view's camera); unset = LOD 0.
    std::optional<LodSelection> lod;
};

// Per-frame GPU upload memory (host visible). Valid until the frame retires.
struct GpuAllocation {
    void* cpu = nullptr;
    VkDeviceAddress address = 0;
    u64 size = 0;
    rhi::BufferHandle buffer;
    u64 offset = 0;
};

struct DefaultTextures {
    rhi::TextureHandle white, black, flatNormal, checker, blackCube;
    u32 whiteIndex = kInvalidIndex, blackIndex = kInvalidIndex, flatNormalIndex = kInvalidIndex;
    u32 checkerIndex = kInvalidIndex, blackCubeIndex = kInvalidIndex;
};

// --- contexts -----------------------------------------------------------------------------------------------

struct FeatureInitContext {
    rhi::Device& device;
    Renderer& renderer;
};

// Called for every enabled feature before the frame graph is declared (in registration order).
struct ViewSetup {
    const RenderSettings& settings;
    const rhi::DeviceCaps& caps;
    const RenderView& view;
    Extent2D outputExtent;
    f32 screenPercentage = 100.0f; // upscalers set this from their quality mode
    u32 jitterPhases = 0;          // > 0 enables Halton(2,3) sub-pixel jitter with this period (TAA, upscalers)
    f32 mipBias = 0.0f;            // added to material texture sampling (upscalers: log2(render/output))
};

struct FrameState; // renderer internal

// Everything a feature may touch while declaring its passes. Lifetime: one setup() call.
class FeatureContext {
public:
    FeatureContext(FrameState& frame, IRenderFeature* feature, InjectionPoint point)
        : m_frame(&frame), m_feature(feature), m_point(point) {}

    [[nodiscard]] InjectionPoint point() const { return m_point; }
    [[nodiscard]] IRenderFeature* feature() const { return m_feature; }
    rhi::RenderGraph& graph();
    FrameResources& resources();
    RenderView& view();
    [[nodiscard]] const RenderSettings& settings() const;
    [[nodiscard]] const RenderSnapshot& snapshot() const;
    rhi::Device& device();
    [[nodiscard]] const rhi::DeviceCaps& caps() const;
    GpuScene& scene();
    Renderer& renderer();
    [[nodiscard]] Extent2D renderExtent() const;
    [[nodiscard]] Extent2D outputExtent() const;

    // Per-view shader constants (common/view.glsl). Mutable during setup; uploaded before the graph executes.
    GpuViewConstants& viewConstants();
    // Device addresses for push constants (`view`, `scene` — the first 16 bytes of every render push block).
    [[nodiscard]] VkDeviceAddress viewAddress() const;
    [[nodiscard]] VkDeviceAddress sceneAddress() const;

    GpuAllocation allocate(u64 size, u64 alignment = 16);
    template <class T>
    VkDeviceAddress upload(std::span<const T> data) {
        GpuAllocation a = allocate(data.size_bytes() ? data.size_bytes() : sizeof(T), alignof(T) < 16 ? 16 : alignof(T));
        if (!data.empty()) std::memcpy(a.cpu, data.data(), data.size_bytes());
        return a.address;
    }

    // Camera-culled draw lists of this view.
    [[nodiscard]] const ViewDrawLists& drawLists() const;
    // Custom culled draw list (instance ids uploaded to frame memory).
    DrawList buildDrawList(const DrawFilter& filter);
    // Like buildDrawList(), but culled on the GPU when r.GpuDriven is active (Opaque/Masked buckets): frustum /
    // sphere / flags / LOD tests run in a compute pass recorded before the next pass declared after this call, and
    // the result is an indirect list (draw it with drawBatches(), passing the same instanceMultiplier). Falls back
    // to buildDrawList() otherwise.
    DrawList cullDrawList(const DrawFilter& filter, u32 instanceMultiplier = 1);
    // LOD selection for this view's camera (r.ViewDistance.LODBias, r.GpuDriven.LODErrorPixels).
    [[nodiscard]] LodSelection lodSelection() const;
    // Binds pipelines[batch.variant] per batch, pushes `pushConstants` (must start with view, scene, instanceIds
    // addresses; see OX_RENDER_DRAW_PUSH in common/scene.glsl) and issues one indexed instanced draw per batch.
    void drawBatches(rhi::CommandList& cmd, const DrawList& list, std::span<const rhi::PipelineHandle> pipelines,
                     const void* pushConstants, u32 pushSize, u32 instanceMultiplier = 1);

    // Ping-pong history pair (persistent, reallocated on resize). One call per name per frame.
    HistoryTexture history(std::string_view name, const rhi::TextureDesc& desc);
    // Persistent per (feature, view) state. T must derive from IFeatureViewState and be default constructible.
    template <class T>
    T& viewState() {
        IFeatureViewState*& slot = viewStateSlot(std::type_index(typeid(T)));
        if (!slot) slot = new T();
        return static_cast<T&>(*slot);
    }

    [[nodiscard]] const DefaultTextures& defaults() const;
    RenderStats& stats();
    // Counts a draw in RenderStats (drawBatches does it automatically).
    void countDraw(u64 triangles, u32 instances = 1);

    // Renderer internals (built-in features only).
    FrameState& internalFrame() { return *m_frame; }

private:
    IFeatureViewState*& viewStateSlot(std::type_index type);
    FrameState* m_frame;
    IRenderFeature* m_feature;
    InjectionPoint m_point;
};

// --- the interface -----------------------------------------------------------------------------------------

class IRenderFeature {
public:
    virtual ~IRenderFeature() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual InjectionMask injectionPoints() const = 0;
    // Ascending order inside an injection point (PostProcess chains: bloom 100, DOF 200, ...).
    [[nodiscard]] virtual i32 order() const { return 0; }
    // Features sharing a non-empty group are mutually exclusive: the enabled one with the highest priority runs
    // (e.g. "Shadows": ShadowsRaster priority 0, ShadowsRT priority 100 enabled only with r.RayTracing + caps).
    [[nodiscard]] virtual std::string_view exclusiveGroup() const { return {}; }
    [[nodiscard]] virtual i32 priority() const { return 0; }
    // Resource names this feature publishes (documentation, editor UI, conflict diagnostics).
    [[nodiscard]] virtual std::vector<std::string_view> provides() const { return {}; }
    // Names of the cvars the feature reads (settings UI grouping).
    [[nodiscard]] virtual std::vector<std::string> cvarNames() const { return {}; }

    // Evaluated every frame per view: cvar or capability changes rebuild the graph on the next frame.
    [[nodiscard]] virtual bool isEnabled(const RenderSettings& settings, const rhi::DeviceCaps& caps) const {
        return true;
    }
    // Lazily on first enable (create pipelines, persistent textures). Return false to disable permanently.
    virtual bool initialize(FeatureInitContext& ctx) { return true; }
    virtual void shutdown(rhi::Device& device) {}
    // Before the graph is declared: request jitter, change render resolution, mip bias.
    virtual void prepareView(ViewSetup& setup) {}
    // Declare passes for ctx.point() (called once per injection point in injectionPoints()).
    virtual void setup(FeatureContext& ctx) = 0;
};

// Process-wide factories ("features register themselves"): modules call registerFeatureFactory() from their explicit
// registerXxx() init; every Renderer instantiates all factories at creation.
using FeatureFactory = std::function<std::unique_ptr<IRenderFeature>()>;
void registerFeatureFactory(std::string name, FeatureFactory factory);
[[nodiscard]] std::vector<std::pair<std::string, FeatureFactory>> featureFactories();

class FeatureRegistry {
public:
    FeatureRegistry();
    ~FeatureRegistry();
    FeatureRegistry(const FeatureRegistry&) = delete;
    FeatureRegistry& operator=(const FeatureRegistry&) = delete;

    IRenderFeature& add(std::unique_ptr<IRenderFeature> feature);
    template <class T, class... Args>
    T& emplace(Args&&... args) {
        return static_cast<T&>(add(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    bool remove(std::string_view name, rhi::Device* device = nullptr);
    [[nodiscard]] IRenderFeature* find(std::string_view name) const;
    [[nodiscard]] std::vector<IRenderFeature*> all() const;

    // Every feature gets a toggle cvar "r.Feature.<Name>" (default true).
    [[nodiscard]] static CVar<bool>& toggle(std::string_view featureName);

    // Enabled features after the toggle cvar, isEnabled() and exclusive-group resolution, in registration order.
    [[nodiscard]] std::vector<IRenderFeature*> resolve(const RenderSettings& settings, const rhi::DeviceCaps& caps) const;
    // Of a resolved set: the features at `point`, sorted by order().
    [[nodiscard]] static std::vector<IRenderFeature*> at(std::span<IRenderFeature* const> resolved, InjectionPoint point);

    // Initialization bookkeeping (Renderer).
    bool ensureInitialized(IRenderFeature& feature, FeatureInitContext& ctx);
    void shutdownAll(rhi::Device& device);
    [[nodiscard]] bool failed(const IRenderFeature& feature) const;

private:
    struct Entry {
        std::unique_ptr<IRenderFeature> feature;
        bool initialized = false;
        bool failed = false;
        u32 index = 0;
    };
    std::vector<Entry> m_entries;
    u32 m_nextIndex = 0;
};

// --- helpers for feature authors ----------------------------------------------------------------------------

// Fullscreen triangle vertex shader (render/common/fullscreen.vert): outputs `layout(location = 0) out vec2 uv`.
rhi::ShaderStageDesc fullscreenVertexShader();
// Graphics pipeline for a fullscreen fragment shader (no depth, no culling).
rhi::PipelineHandle createFullscreenPipeline(rhi::Device& device, std::string name, std::string fragmentPath,
                                             std::vector<VkFormat> colorFormats,
                                             std::vector<rhi::BlendState> blend = {},
                                             std::vector<rhi::ShaderDefine> defines = {});
rhi::PipelineHandle createComputePipeline(rhi::Device& device, std::string name, std::string path,
                                          std::vector<rhi::ShaderDefine> defines = {});
// Records a fullscreen triangle with the given push constants (pipeline must be bound by the caller or passed).
void drawFullscreen(rhi::CommandList& cmd, rhi::PipelineHandle pipeline, const void* pushConstants, u32 pushSize);

} // namespace ox::render
