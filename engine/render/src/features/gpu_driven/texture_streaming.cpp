#include "texture_streaming.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/gpu_scene.hpp>
#include <oxwald/rhi/device.hpp>

#include <algorithm>
#include <cmath>

namespace ox::render {

namespace {

using S = Scalability;

CVar<bool> cvStreaming("r.Streaming", true, "Texture mip streaming (distance / texel density heuristic)");
CVar<int> cvPoolSize("r.Streaming.PoolSizeMB", 1024, "Texture streaming budget in MiB (resident mips of streamed textures)",
                     S::Textures, {256, 512, 1024, 2048});
CVar<int> cvTailSize("r.Streaming.TailSize", 64, "Mips up to this size are always resident", 1, 4096);
CVar<float> cvStreamingBias("r.Streaming.MipBias", 0.0f, "Bias added to the wanted mip (positive = blurrier, less memory)",
                            -4.0f, 8.0f);
CVar<int> cvMaxUpload("r.Streaming.MaxUploadMBPerFrame", 32, "Upload limit per frame for residency increases (MiB)", 1, 4096);
CVar<int> cvDropDelay("r.Streaming.DropDelayFrames", 30, "Frames a texture must want fewer mips before they are evicted",
                      0, 10000);

} // namespace

TextureStreamer::TextureStreamer(rhi::Device& device, GpuResourceCache& cache, GpuScene& scene)
    : m_device(&device), m_cache(&cache), m_scene(&scene) {}

TextureStreamer::~TextureStreamer() {
    for (auto& [id, e] : m_textures) {
        if (e.pending.texture) m_device->destroy(e.pending.texture);
    }
}

u64 TextureStreamer::bytesFrom(const Entry& e, u32 first) {
    u64 bytes = 0;
    for (usize m = first; m < e.data->mips.size(); ++m) bytes += e.data->mips[m].data.size();
    return bytes;
}

u32 TextureStreamer::onTextureLoaded(const Uuid& id, std::shared_ptr<const assets::TextureData> data, u32 minFirstMip) {
    if (!cvStreaming || !data || data->mips.size() <= 1) {
        m_textures.erase(id);
        return minFirstMip;
    }
    Entry& e = m_textures[id];
    if (e.pending.texture) m_device->destroy(e.pending.texture);
    e = {};
    e.data = std::move(data);
    e.minFirst = minFirstMip;
    const u32 tail = u32(std::max(cvTailSize.get(), 1));
    u32 t = minFirstMip;
    while (t + 1 < e.data->mips.size() && std::max(e.data->mips[t].width, e.data->mips[t].height) > tail) ++t;
    e.tailFirst = t;
    e.residentFirst = e.wantedFirst = t;
    return t;
}

void TextureStreamer::onTextureRemoved(const Uuid& id) {
    auto it = m_textures.find(id);
    if (it == m_textures.end()) return;
    if (it->second.pending.texture) m_device->destroy(it->second.pending.texture);
    m_textures.erase(it);
}

u32 TextureStreamer::residentFirstMip(const Uuid& id) const {
    auto it = m_textures.find(id);
    return it != m_textures.end() ? it->second.residentFirst : ~0u;
}

void TextureStreamer::update(const LodSelection& camera, TextureStreamingStats& stats) {
    OX_PROFILE_ZONE();
    ++m_frame;
    stats.enabled = cvStreaming;
    stats.budgetBytes = u64(std::max(cvPoolSize.get(), 1)) << 20;
    if (!cvStreaming || m_textures.empty()) {
        stats.streamedTextures = u32(m_textures.size());
        return;
    }
    // 1. Swap in the textures uploaded last frame (their transfer has been submitted and waited for by the GPU).
    for (auto& [id, e] : m_textures) {
        if (!e.pending.texture || e.pendingFrame >= m_frame) continue;
        if (m_cache->replaceTexture(id, e.pending)) {
            if (e.pendingFirst < e.residentFirst) stats.mipsLoaded += e.residentFirst - e.pendingFirst;
            else stats.mipsEvicted += e.pendingFirst - e.residentFirst;
            e.residentFirst = e.pendingFirst;
        } else {
            m_device->destroy(e.pending.texture);
        }
        e.pending = {};
    }

    // 2. Projected size per material (max over its instances), then per texture.
    const GpuMaterial* materials = m_scene->materials();
    GpuSceneHeader header;
    m_scene->fillHeader(header);
    m_materialPixels.assign(header.materialCount, 0.0f);
    for (const GpuInstance& inst : m_scene->instances()) {
        if (!(inst.flags & kInstanceVisible) || inst.materialIndex >= header.materialCount) continue;
        const glm::vec3 c(inst.boundingSphere);
        const f32 r = inst.boundingSphere.w;
        const f32 dist = std::max(glm::length(c - camera.cameraPosition) - r, 0.05f);
        const f32 px = camera.orthographic ? 2.0f * r * camera.projScale : 2.0f * r * camera.projScale / dist;
        m_materialPixels[inst.materialIndex] = std::max(m_materialPixels[inst.materialIndex], px);
    }
    m_byIndex.clear();
    for (auto& [id, e] : m_textures) {
        e.wantedFirst = e.tailFirst;
        e.importance = 0.0f;
        if (const GpuTexture* t = m_cache->residentTexture(id)) m_byIndex[t->sampledIndex] = &e;
    }
    const f32 bias = cvStreamingBias;
    for (u32 m = 0; m < m_materialPixels.size(); ++m) {
        const f32 px = m_materialPixels[m];
        if (px <= 0.0f) continue;
        const GpuMaterial& mat = materials[m];
        const f32 tiling = std::max({std::abs(mat.uvTiling.x), std::abs(mat.uvTiling.y), 1e-3f});
        for (u32 index : {mat.albedoTexture, mat.normalTexture, mat.ormTexture, mat.emissiveTexture}) {
            if (index == kInvalidIndex) continue;
            auto it = m_byIndex.find(index);
            if (it == m_byIndex.end()) continue;
            Entry& e = *it->second;
            const f32 size = f32(std::max(e.data->width, e.data->height));
            const f32 level = std::log2(std::max(size / (px * tiling), 1.0f)) + bias;
            const u32 wanted = std::clamp(u32(std::max(level, 0.0f)), e.minFirst, e.tailFirst);
            e.wantedFirst = std::min(e.wantedFirst, wanted);
            e.importance = std::max(e.importance, px);
        }
    }

    // 3. Budget: coarsen the least important textures first.
    u64 wantedBytes = 0;
    m_sorted.clear();
    for (auto& [id, e] : m_textures) {
        wantedBytes += bytesFrom(e, e.wantedFirst);
        m_sorted.push_back(&e);
    }
    stats.wantedBytes = wantedBytes;
    std::sort(m_sorted.begin(), m_sorted.end(), [](const Entry* a, const Entry* b) { return a->importance < b->importance; });
    u64 total = wantedBytes;
    for (bool progress = true; total > stats.budgetBytes && progress;) {
        progress = false;
        for (Entry* e : m_sorted) {
            if (total <= stats.budgetBytes) break;
            if (e->wantedFirst >= e->tailFirst) continue;
            total -= e->data->mips[e->wantedFirst].data.size();
            ++e->wantedFirst;
            progress = true;
        }
    }

    // 4. Residency changes: increases now (upload limit), decreases after the drop delay or when over budget.
    u64 resident = 0;
    for (auto& [id, e] : m_textures) resident += bytesFrom(e, e.residentFirst);
    const bool overBudget = resident > stats.budgetBytes;
    u64 uploadBudget = u64(std::max(cvMaxUpload.get(), 1)) << 20;
    u32 pending = 0;
    // Most important first, so the upload limit serves what is closest to the camera.
    for (auto it = m_sorted.rbegin(); it != m_sorted.rend(); ++it) {
        Entry& e = **it;
        if (e.pending.texture) {
            ++pending;
            continue;
        }
        u32 target = e.residentFirst;
        if (e.wantedFirst < e.residentFirst) {
            const u64 bytes = bytesFrom(e, e.wantedFirst);
            if (bytes > uploadBudget) continue;
            uploadBudget -= bytes;
            target = e.wantedFirst;
            e.dropFrames = 0;
        } else if (e.wantedFirst > e.residentFirst) {
            if (!overBudget && ++e.dropFrames < u32(cvDropDelay.get())) continue;
            target = e.wantedFirst;
            e.dropFrames = 0;
        } else {
            e.dropFrames = 0;
            continue;
        }
        GpuTexture t = m_cache->createTextureFromData(*e.data, target);
        if (!t.texture) continue;
        e.pending = t;
        e.pendingFirst = target;
        e.pendingFrame = m_frame;
        ++pending;
    }
    stats.streamedTextures = u32(m_textures.size());
    stats.pendingRequests = pending;
    stats.residentBytes = resident;
}

} // namespace ox::render
