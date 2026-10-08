#pragma once

// GpuResourceCache: asset UUID → GPU mesh / texture / material. Data comes from a provider (the assets module's
// manager, adapted by the runtime/editor), is loaded on worker threads (JobSystem) and uploaded on the rhi transfer
// queue. While an asset is loading, lookups return placeholders: meshes are skipped, materials use the default
// material, textures resolve to default textures (white / flat normal / checker for missing assets).
// invalidate(uuid) reloads an asset (hot reload); dependants (materials of a texture) are refreshed automatically.

#include <oxwald/assets/material.hpp>
#include <oxwald/assets/mesh.hpp>
#include <oxwald/assets/texture.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/render/gpu_scene.hpp>
#include <oxwald/rhi/handles.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

namespace ox {
class JobSystem;
}

namespace ox::render {

// Synchronous loaders, called on worker threads when a JobSystem is set (else on the render thread inside update()).
// Return nullptr when the asset does not exist (it is then marked missing).
struct AssetProvider {
    std::function<std::shared_ptr<const assets::MeshData>(const Uuid&)> loadMesh;
    std::function<std::shared_ptr<const assets::TextureData>(const Uuid&)> loadTexture;
    std::function<std::shared_ptr<const assets::MaterialAsset>(const Uuid&)> loadMaterial;
};

enum class ResourceState : u8 { Unknown, Loading, Ready, Missing };

struct GpuTexture {
    rhi::TextureHandle texture;
    u32 sampledIndex = kInvalidIndex;
    bool cube = false;
    u32 width = 0, height = 0, mips = 0;
};

// Texture mip streaming hook (TextureStreamer, area gpu-driven). When installed, applyTexture() hands the CPU data
// over and uploads only the mips from the returned level on; the streamer changes residency later with
// createTextureFromData() + replaceTexture() (no CPU or GPU waits: uploads go through the transfer queue).
class ITextureStreamingHook {
public:
    virtual ~ITextureStreamingHook() = default;
    // Returns the first mip to make resident now (>= minFirstMip, the r.Textures.MaxSize limit).
    virtual u32 onTextureLoaded(const Uuid& id, std::shared_ptr<const assets::TextureData> data, u32 minFirstMip) = 0;
    virtual void onTextureRemoved(const Uuid& id) = 0;
};

class GpuResourceCache {
public:
    GpuResourceCache(rhi::Device& device, GpuScene& scene);
    ~GpuResourceCache();
    GpuResourceCache(const GpuResourceCache&) = delete;
    GpuResourceCache& operator=(const GpuResourceCache&) = delete;

    void setProvider(AssetProvider provider, JobSystem* jobs = nullptr);

    // Direct registration (procedural meshes, editor previews, tests). Replaces existing entries (hot reload).
    // Thread-safe: on the render thread (the thread calling update()) or before the first update() the data is
    // applied immediately; from any other thread (game code with a threaded renderer) it is copied and applied at
    // the start of the next update().
    void addMesh(const Uuid& id, const assets::MeshData& mesh);
    void addTexture(const Uuid& id, const assets::TextureData& texture);
    void addMaterial(const Uuid& id, const assets::MaterialAsset& material);
    // Registers an existing GPU texture under a UUID (render targets used as material textures, tools).
    void addExternalTexture(const Uuid& id, rhi::TextureHandle texture);
    void remove(const Uuid& id);
    // Hot reload: reloads from the provider (render thread).
    void invalidate(const Uuid& id);
    // Thread-safe variant (asset watcher / main thread); applied in the next update().
    void queueInvalidate(const Uuid& id);

    // Lookups (render thread). Unknown ids trigger a load through the provider.
    const GpuMesh* mesh(const Uuid& id);
    u32 materialIndex(const Uuid& id); // default material while loading / missing
    const GpuTexture* texture(const Uuid& id);
    [[nodiscard]] ResourceState state(const Uuid& id) const;

    // Per frame on the render thread: finishes loads, uploads, refreshes material tables.
    void update();
    // Blocks until every pending load finished and was uploaded (tests, loading screens).
    void flush();

    // Sampler state applied to material textures (r.Textures.* cvars); changing it refreshes all materials.
    void setTextureQuality(i32 anisotropy, f32 mipBias, i32 maxTextureSize);

    // --- streaming support ---
    void setStreamingHook(ITextureStreamingHook* hook) { m_streaming = hook; }
    // GPU texture holding mips [firstMip, last] of `data`, uploaded asynchronously (transfer queue). Invalid handle
    // for unsupported data.
    [[nodiscard]] GpuTexture createTextureFromData(const assets::TextureData& data, u32 firstMip);
    // Swaps the GPU texture of a loaded texture (residency change): materials are refreshed, the previous texture is
    // destroyed once the frames using it retired. False when `id` is not a loaded texture.
    bool replaceTexture(const Uuid& id, const GpuTexture& texture);
    // Resident GPU texture without triggering a load (nullptr when not ready).
    [[nodiscard]] const GpuTexture* residentTexture(const Uuid& id) const;
    // Sum of the GPU memory of loaded (non-external) textures, in bytes.
    [[nodiscard]] u64 textureBytes() const;

    [[nodiscard]] const DefaultTextures& defaults() const { return m_defaults; }
    [[nodiscard]] u32 pendingLoads() const;

private:
    struct MeshEntry {
        ResourceState state = ResourceState::Unknown;
        GpuMesh gpu;
    };
    struct TextureEntry {
        ResourceState state = ResourceState::Unknown;
        GpuTexture gpu;
        bool external = false;
    };
    struct MaterialEntry {
        ResourceState state = ResourceState::Unknown;
        u32 index = 0;
        assets::MaterialAsset source;
        bool hasSource = false;
    };
    struct Completed {
        Uuid id;
        std::shared_ptr<const assets::MeshData> mesh;
        std::shared_ptr<const assets::TextureData> texture;
        std::shared_ptr<const assets::MaterialAsset> material;
        u8 kind = 0; // 0 mesh, 1 texture, 2 material
    };

    [[nodiscard]] bool onOwnerThread() const;
    void requestLoad(const Uuid& id, u8 kind);
    void applyMesh(const Uuid& id, const assets::MeshData& mesh);
    void applyTexture(const Uuid& id, const assets::TextureData& texture,
                      std::shared_ptr<const assets::TextureData> shared = nullptr);
    void applyMaterial(const Uuid& id, const assets::MaterialAsset& material);
    void refreshMaterial(MaterialEntry& entry);
    u32 resolveTexture(const Uuid& id, u32 fallback);
    void createDefaults();

    rhi::Device* m_device;
    GpuScene* m_scene;
    AssetProvider m_provider;
    JobSystem* m_jobs = nullptr;
    DefaultTextures m_defaults;
    std::unordered_map<Uuid, MeshEntry> m_meshes;
    std::unordered_map<Uuid, TextureEntry> m_textures;
    std::unordered_map<Uuid, MaterialEntry> m_materialsById;
    mutable std::mutex m_mutex;
    std::vector<Completed> m_completed;
    std::vector<Completed> m_queuedAdds; // add*() from other threads, applied in update()
    std::vector<Uuid> m_invalidations;
    std::atomic<std::thread::id> m_ownerThread{}; // render thread (last update() caller)
    std::atomic<u32> m_inFlight{0};
    i32 m_anisotropy = 8;
    f32 m_mipBias = 0.0f;
    i32 m_maxTextureSize = 8192;
    u32 m_samplerRepeat = 4;
    u32 m_samplerClamp = 1;
    ITextureStreamingHook* m_streaming = nullptr;
};

} // namespace ox::render
