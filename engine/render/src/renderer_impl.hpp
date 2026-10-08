#pragma once

// Private renderer state shared by renderer.cpp, the feature context and the built-in features.

#include <oxwald/render/gpu_resource_cache.hpp>
#include <oxwald/render/gpu_scene.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/render_stats.hpp>
#include <oxwald/render/render_view.hpp>
#include <oxwald/render/renderer.hpp>
#include <oxwald/render/snapshot.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/render_graph.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <typeindex>
#include <unordered_map>

namespace ox::render {

class GpuDriven;
class TextureStreamer;

// Linear allocator over host-visible buffers, one chunk list per frame in flight.
class GpuFrameAllocator {
public:
    void init(rhi::Device& device, u32 framesInFlight, u64 chunkSize = 8ull << 20);
    void release(rhi::Device& device);
    void beginFrame(u32 frameSlot);
    GpuAllocation allocate(u64 size, u64 alignment = 16);
    [[nodiscard]] u64 usedThisFrame() const;

private:
    struct Chunk {
        rhi::BufferHandle buffer;
        u8* cpu = nullptr;
        VkDeviceAddress address = 0;
        u64 size = 0;
        u64 used = 0;
    };
    rhi::Device* m_device = nullptr;
    std::vector<std::vector<Chunk>> m_frames;
    u32 m_slot = 0;
    u64 m_chunkSize = 0;
};

struct HistoryEntry {
    rhi::TextureHandle textures[2];
    rhi::TextureDesc desc;
    u32 current = 0;          // index written this frame
    u64 lastFrame = ~0ull;    // view frame index of the last write
    bool valid = false;
};

struct PickRequest {
    PickRequestId id = 0;
    u32 x = 0, y = 0, w = 1, h = 1; // output pixels
    bool recorded = false;
    rhi::BufferHandle readback;
    rhi::TimelinePoint done;
    u64 recordedFrame = ~0ull; // recordInto mode: frame the pick was recorded in
    PickResult result;
};

struct RenderView::Impl {
    std::map<std::pair<const IRenderFeature*, std::type_index>, IFeatureViewState*> featureStates;
    std::unordered_map<std::string, HistoryEntry> histories;
    rhi::BufferHandle clusterBuffer;
    u64 clusterBufferSize = 0;
    std::vector<PickRequest> picks;
    u64 lastSettingsHash = 0;
    u64 lastGraphCompiles = 0;
    std::vector<u32> drawIds[u32(DrawBucket::Count)];
    // FrameState containers swapped in/out every frame so their capacity is reused (no steady-state allocations).
    ViewDrawLists cachedLists[3]; // CPU lists, GPU early, GPU late
    std::vector<GpuLight> cachedLights;
    std::vector<GpuShadow> cachedShadows;
};

// Everything one view's frame needs; FeatureContext forwards to it.
struct FrameState {
    Renderer::Impl* r = nullptr;
    RenderView* view = nullptr;
    const RenderSnapshot* snapshot = nullptr;
    RenderSettings settings;
    FrameResources resources;
    GpuViewConstants constants;
    GpuAllocation constantsAlloc;
    GpuSceneHeader header;
    GpuAllocation headerAlloc;
    std::vector<GpuLight> lights;    // directional first
    std::vector<GpuShadow> shadows;
    u32 directionalCount = 0;
    ViewDrawLists drawLists;        // CPU-culled; built lazily on first drawLists() access when gpuDriven
    bool cpuDrawListsBuilt = true;
    f32 drawDistance = 0.0f;
    // GPU-driven path (features/gpu_driven): Opaque/Masked indirect lists of the two occlusion phases.
    bool gpuDriven = false;
    bool gpuLateActive = false;
    ViewDrawLists gpuEarly, gpuLate;
    std::vector<IRenderFeature*> features; // resolved, registration order
    RenderStats* stats = nullptr;
    bool firstViewOfFrame = false;
    bool allowParallelRecording = false; // not when recording into a caller's command list
    VkFormat outputFormat = VK_FORMAT_UNDEFINED;
};

struct BuiltinPipelines {
    rhi::PipelineHandle prepass[kVariantCount][2]; // [variant][entity id]
    rhi::PipelineHandle forward[kVariantCount];
    rhi::PipelineHandle lightCull;
    rhi::PipelineHandle hiz;
    rhi::PipelineHandle tonemap;
    rhi::PipelineHandle resample;
    rhi::PipelineHandle pick;
    std::unordered_map<VkFormat, rhi::PipelineHandle> finalBlit;
};

struct Renderer::Impl {
    rhi::Device* device = nullptr;
    RendererDesc desc;
    std::unique_ptr<GpuScene> scene;
    std::unique_ptr<GpuResourceCache> cache;
    std::unique_ptr<GpuDriven> gpuDriven;
    std::unique_ptr<TextureStreamer> streamer;
    LodSelection streamingCamera; // main view of the frame (texture streaming heuristic)
    FeatureRegistry features;
    std::map<ViewId, std::unique_ptr<RenderView>> views;
    ViewId nextViewId = 1;
    BuiltinPipelines pipelines;
    GpuFrameAllocator frameAlloc;
    rhi::BufferHandle zeroSH; // 9 × vec4 of zeros (no environment)
    RenderSettings settings;
    const RenderSnapshot* snapshot = nullptr;
    RenderStats stats;          // last completed frame
    RenderStats building;       // frame being built
    u64 frameCounter = 0;
    u64 deviceFrameAtBegin = ~0ull;
    bool inFrame = false;
    bool sceneUploaded = false;
    bool environmentDirty = true;
    PickRequestId nextPick = 1;
    std::vector<PickRequest> finishedPicks;
    std::chrono::steady_clock::time_point frameStart;

    Renderer* self = nullptr;

    void createPipelines();
    rhi::PipelineHandle finalPipeline(VkFormat format);
    void buildView(FrameState& fs, const ViewRenderRequest& request, rhi::RGTexture output, Extent2D outputExtent);
    void runFeatures(FrameState& fs, InjectionPoint point);
    void commit(FrameState& fs);
    void collectPicks();
    void buildCpuDrawLists(FrameState& fs);
    // Multithreaded recording of CPU-path instanced batches (secondary command lists, r.ParallelRecording).
    [[nodiscard]] bool wantsParallelRecording(const FrameState& fs, std::initializer_list<const DrawList*> lists) const;
    void recordParallel(FrameState& fs, rhi::PassContext& ctx, std::initializer_list<const DrawList*> lists,
                        const rhi::PipelineHandle* pipelines, const void* push, u32 pushSize);
};

// Built-in features (features/*.cpp).
std::unique_ptr<IRenderFeature> makeShadowsRasterFeature();
std::unique_ptr<IRenderFeature> makeEnvironmentFeature();
std::unique_ptr<IRenderFeature> makeSkyFeature();
std::unique_ptr<IRenderFeature> makeDebugLinesFeature();
std::unique_ptr<IRenderFeature> makeEditorOverlaysFeature();
std::unique_ptr<IRenderFeature> makeDebugViewsFeature();

// Shared helpers.
rhi::RGTexture importPersistent(rhi::RenderGraph& graph, rhi::Device& device, rhi::TextureHandle texture);
Renderer::Impl& rendererImpl(FeatureContext& ctx);
FrameState& frameState(FeatureContext& ctx);

} // namespace ox::render
