#pragma once

// Texture mip streaming (r.Streaming*): textures loaded through GpuResourceCache start with their small mip tail
// resident; every frame the streamer estimates the mip each texture needs from the screen-space texel density of
// the instances using it (bounding-sphere projection × UV tiling — a distance heuristic, no GPU feedback pass),
// fits the wanted residency into the budget (Scalability::Textures) by coarsening the least important textures, and
// changes residency by uploading a new texture with the wanted mip range on the transfer queue and swapping it in a
// frame later (no CPU waits, no GPU bubbles). Drops are delayed (hysteresis) unless the pool is over budget.

#include <oxwald/render/gpu_resource_cache.hpp>
#include <oxwald/render/render_stats.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace ox::render {

class TextureStreamer final : public ITextureStreamingHook {
public:
    TextureStreamer(rhi::Device& device, GpuResourceCache& cache, GpuScene& scene);
    ~TextureStreamer() override;

    u32 onTextureLoaded(const Uuid& id, std::shared_ptr<const assets::TextureData> data, u32 minFirstMip) override;
    void onTextureRemoved(const Uuid& id) override;

    // After the frame's views: camera = LOD selection of the main view (projScale in pixels per unit at 1 m).
    void update(const LodSelection& camera, TextureStreamingStats& stats);
    // First resident mip of a streamed texture (~0u when not streamed; tests / debug UI).
    [[nodiscard]] u32 residentFirstMip(const Uuid& id) const;

private:
    struct Entry {
        std::shared_ptr<const assets::TextureData> data;
        u32 minFirst = 0;      // r.Textures.MaxSize limit
        u32 tailFirst = 0;     // always resident (small mips)
        u32 residentFirst = 0;
        u32 wantedFirst = 0;
        f32 importance = 0.0f; // max projected size in pixels
        u32 dropFrames = 0;
        // Pending swap (uploaded last frame).
        GpuTexture pending;
        u32 pendingFirst = 0;
        u64 pendingFrame = 0;
    };
    [[nodiscard]] static u64 bytesFrom(const Entry& e, u32 first);

    rhi::Device* m_device;
    GpuResourceCache* m_cache;
    GpuScene* m_scene;
    std::unordered_map<Uuid, Entry> m_textures;
    std::unordered_map<u32, Entry*> m_byIndex; // bindless sampled index → entry (rebuilt per frame)
    std::vector<f32> m_materialPixels;         // scratch: max projected size per material
    std::vector<Entry*> m_sorted;              // scratch
    u64 m_frame = 0;
};

} // namespace ox::render
