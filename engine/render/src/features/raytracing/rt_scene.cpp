// CPU logic of the ray tracing scene: TLAS instance table, BLAS scheduler, LOD policy, accumulation tracker.
#include <oxwald/core/hash.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/render/features/raytracing/rt_scene.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <algorithm>
#include <cstring>

namespace ox::render::rt {

u32 instanceMask(u32 blend, bool castShadows) {
    u32 m = blend == 0 ? kMaskOpaque : blend == 1 ? kMaskAlphaTested : kMaskTranslucent;
    if (castShadows) m |= blend <= 1 ? kMaskShadowOpaque : kMaskShadowTranslucent;
    return m;
}

u32 selectBlasLod(u32 lodCount, i32 requestedLod) {
    if (lodCount == 0) return 0;
    if (requestedLod < 0) return lodCount - 1;
    return std::min(u32(requestedLod), lodCount - 1);
}

glm::mat3x4 toTlasTransform(const glm::mat4& world) {
    // VkTransformMatrixKHR is row-major 3x4: row r = (world[0][r], world[1][r], world[2][r], world[3][r]).
    glm::mat3x4 t(0.0f);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) t[r][c] = world[c][r];
    }
    return t;
}

// --- TlasInstanceTable ---

TlasInstanceTable::Result TlasInstanceTable::update(std::span<const TlasSource> sources) {
    Result res;
    std::vector<u32> order;
    order.reserve(sources.size());
    for (u32 i = 0; i < sources.size(); ++i) {
        if (sources[i].blas == 0) {
            ++res.skipped;
            continue;
        }
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return sources[a].gpuInstance < sources[b].gpuInstance; });
    if (order.size() > kMaxCustomIndex) {
        OX_LOG_WARN("render", "TLAS: {} instances exceed the 24-bit custom index, truncating", order.size());
        order.resize(kMaxCustomIndex);
    }

    std::vector<TlasEntry> entries;
    std::vector<RtInstanceGpu> gpu;
    entries.reserve(order.size());
    gpu.reserve(order.size());
    std::unordered_map<u32, u32> index;
    index.reserve(order.size());
    bool flagsChanged = false;
    u32 kept = 0;
    for (u32 i : order) {
        const TlasSource& s = sources[i];
        TlasEntry e;
        e.gpuInstance = s.gpuInstance;
        e.customIndex = u32(entries.size());
        e.mask = instanceMask(s.blend, s.castShadows);
        const RtHitGroup group = hitGroupForBlend(s.blend);
        e.sbtOffset = sbtRecordOffset(group);
        // Meshes are counter-clockwise outside (GL convention, see the raster pipelines): front = CCW, no culling.
        e.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                  VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR |
                  (group == RtHitGroup::Opaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR
                                               : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
        e.blas = s.blas;
        e.transform = toTlasTransform(s.world);

        auto it = m_index.find(s.gpuInstance);
        if (it == m_index.end()) {
            ++res.added;
        } else {
            ++kept;
            const TlasEntry& o = m_entries[it->second];
            if (o.blas != e.blas) ++res.blasSwapped;
            if (o.flags != e.flags) flagsChanged = true;
            if (o.mask != e.mask || o.sbtOffset != e.sbtOffset) ++res.retagged;
            if (std::memcmp(&o.transform, &e.transform, sizeof(e.transform)) != 0) ++res.moved;
        }
        index.emplace(e.gpuInstance, e.customIndex);

        RtInstanceGpu g;
        g.gpuInstance = s.gpuInstance;
        g.meshInfo = s.meshInfo;
        g.material = s.material;
        g.flags = (s.doubleSided ? 1u : 0u) | (s.skinned ? 2u : 0u) | (s.deformedBlas ? 4u : 0u) | ((s.blend & 0xFFu) << 8);
        g.mask = e.mask;
        gpu.push_back(g);
        entries.push_back(e);
    }
    res.removed = u32(m_entries.size()) - kept;
    res.rebuild = m_forceRebuild || res.added || res.removed || res.blasSwapped || flagsChanged ||
                  m_updatesSinceRebuild >= maxUpdatesBeforeRebuild;
    res.changed = res.rebuild || res.moved || res.retagged;
    if (res.rebuild) m_updatesSinceRebuild = 0;
    else if (res.changed) ++m_updatesSinceRebuild;
    m_forceRebuild = false;

    // Geometry fields of the GPU table are filled by the caller (mesh table lookups); keep previous values here.
    m_entries = std::move(entries);
    m_gpu = std::move(gpu);
    m_index = std::move(index);
    return res;
}

std::optional<u32> TlasInstanceTable::entryOf(u32 gpuInstance) const {
    auto it = m_index.find(gpuInstance);
    if (it == m_index.end()) return std::nullopt;
    return it->second;
}

// --- BLAS scheduling ---

u64 blasKey(u32 meshInfo, u32 firstIndex, u32 indexCount, i32 vertexOffset, u32 vertexCount, bool deformable) {
    u64 h = fnv1a64("blas");
    for (u64 v : {u64(meshInfo), u64(firstIndex), u64(indexCount), u64(u32(vertexOffset)), u64(vertexCount),
                  u64(deformable ? 1 : 0)}) {
        h = hashCombine(h, v);
    }
    return h ? h : 1;
}

BlasScheduler::~BlasScheduler() { clear(); }

void BlasScheduler::clear() {
    for (auto& [key, item] : m_items) {
        if (item.handle) m_backend->destroy(item.handle);
        if (item.compacted) m_backend->destroy(item.compacted);
    }
    m_items.clear();
    m_stats = {};
}

void BlasScheduler::beginFrame(u64 frame) {
    m_frame = frame;
    for (auto& [key, item] : m_items) item.deformedThisFrame = false;
}

void BlasScheduler::request(const BlasRequest& r) {
    auto [it, inserted] = m_items.try_emplace(r.key);
    Item& item = it->second;
    if (inserted) {
        item.request = r;
        item.state = State::Queued;
    } else {
        item.request.priority = r.priority;
        // Same key = same geometry ranges; buffers may have been re-allocated (e.g. arena growth) → keep request.
        item.request.firstIndex = r.firstIndex;
        item.request.vertexOffset = r.vertexOffset;
    }
    item.lastRequested = m_frame;
}

void BlasScheduler::markDeformed(u64 key) {
    auto it = m_items.find(key);
    if (it != m_items.end()) it->second.deformedThisFrame = true;
}

IBlasBackend::Handle BlasScheduler::ready(u64 key) const {
    auto it = m_items.find(key);
    return it == m_items.end() ? 0 : it->second.handle;
}

std::optional<BlasScheduler::State> BlasScheduler::state(u64 key) const {
    auto it = m_items.find(key);
    if (it == m_items.end()) return std::nullopt;
    return it->second.state;
}

void BlasScheduler::update() {
    Stats& s = m_stats;
    s.builtThisFrame = s.compactedThisFrame = s.refitsThisFrame = s.rebuildsThisFrame = s.evictedThisFrame = 0;
    m_handlesChanged = false;

    // `compacted` doubles as "pending replacement" for builds/rebuilds: the item keeps tracing against `handle`
    // until the replacement completes.
    auto poll = [&](Item& it) {
        if (it.state == State::Building && it.compacted && m_backend->isComplete(it.compacted)) {
            if (it.handle) m_backend->destroy(it.handle);
            it.handle = it.compacted;
            it.compacted = 0;
            it.refits = 0;
            it.state = (!it.request.deformable && !m_backend->compactsInternally()) ? State::CompactionQueued : State::Built;
            ++s.builtThisFrame;
            m_handlesChanged = true;
        } else if (it.state == State::Compacting && it.compacted && m_backend->isComplete(it.compacted)) {
            const u64 before = m_backend->memorySize(it.handle);
            m_backend->destroy(it.handle);
            it.handle = it.compacted;
            it.compacted = 0;
            const u64 after = m_backend->memorySize(it.handle);
            if (before > after) s.savedByCompaction += before - after;
            it.state = State::Compacted;
            ++s.compactedThisFrame;
            m_handlesChanged = true;
        }
    };
    for (auto& [key, it] : m_items) poll(it);

    // Deterministic processing order: priority (desc), then key.
    std::vector<std::pair<u64, Item*>> ordered;
    ordered.reserve(m_items.size());
    for (auto& [key, it] : m_items) ordered.emplace_back(key, &it);
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.second->request.priority != b.second->request.priority) return a.second->request.priority > b.second->request.priority;
        return a.first < b.first;
    });

    // Compactions whose size query is available.
    u32 compactions = 0;
    for (auto& [key, it] : ordered) {
        if (it->state != State::CompactionQueued || compactions >= m_config.maxCompactionsPerFrame) continue;
        const std::optional<u64> size = m_backend->compactedSize(it->handle);
        if (!size) continue; // query not ready yet
        if (*size == 0 || *size >= m_backend->memorySize(it->handle)) {
            it->state = State::Compacted; // nothing to gain
            continue;
        }
        it->compacted = m_backend->compact(it->handle, *size);
        it->compactedBytes = *size;
        it->state = State::Compacting;
        ++compactions;
    }

    // Eviction of BLASes nobody requested for a while.
    for (auto i = m_items.begin(); i != m_items.end();) {
        Item& it = i->second;
        if (m_frame > it.lastRequested + m_config.evictAfterFrames) {
            if (it.handle) m_backend->destroy(it.handle);
            if (it.compacted) m_backend->destroy(it.compacted);
            if (it.handle) m_handlesChanged = true;
            ++s.evictedThisFrame;
            i = m_items.erase(i);
        } else {
            ++i;
        }
    }
    ordered.clear();
    for (auto& [key, it] : m_items) ordered.emplace_back(key, &it);
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.second->request.priority != b.second->request.priority) return a.second->request.priority > b.second->request.priority;
        return a.first < b.first;
    });

    // Refits / periodic rebuilds of deformed geometry.
    u32 builds = 0;
    u64 triangles = 0;
    for (auto& [key, it] : ordered) {
        if (!it->request.deformable || !it->deformedThisFrame || !it->handle || it->state == State::Building) continue;
        if (it->refits >= m_config.maxRefitsBeforeRebuild) {
            it->compacted = m_backend->build(it->request, false);
            it->state = State::Building;
            ++s.rebuildsThisFrame;
            ++builds;
            triangles += it->request.indexCount / 3;
        } else {
            m_backend->refit(it->handle, it->request);
            ++it->refits;
            ++s.refitsThisFrame;
        }
    }

    // New builds within the per-frame budget (the first one always goes through so huge meshes are not starved).
    for (auto& [key, it] : ordered) {
        if (it->state != State::Queued) continue;
        const u64 tris = it->request.indexCount / 3;
        if (builds >= m_config.maxBuildsPerFrame) break;
        if (builds > 0 && triangles + tris > m_config.maxTrianglesPerFrame) break;
        it->compacted = m_backend->build(it->request, !it->request.deformable);
        it->state = State::Building;
        ++builds;
        triangles += tris;
    }
    // Synchronous backends finish immediately: make the results usable this frame.
    for (auto& [key, it] : m_items) poll(it);

    s.resident = s.queued = s.building = s.compacting = 0;
    s.memoryBytes = 0;
    for (auto& [key, it] : m_items) {
        if (it.handle) {
            ++s.resident;
            s.memoryBytes += m_backend->memorySize(it.handle);
        }
        if (it.state == State::Queued) ++s.queued;
        if (it.state == State::Building) ++s.building;
        if (it.state == State::Compacting) ++s.compacting;
    }
}

// --- accumulation ---

u64 hashMatrix(const glm::mat4& m) {
    u64 h = 0xcbf29ce484222325ull;
    const auto* p = reinterpret_cast<const u8*>(&m);
    for (usize i = 0; i < sizeof(m); ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

bool AccumulationTracker::update(u64 cameraHash, u64 sceneHash, u64 settingsHash) {
    const bool reset = !m_valid || cameraHash != m_camera || sceneHash != m_scene || settingsHash != m_settings;
    m_camera = cameraHash;
    m_scene = sceneHash;
    m_settings = settingsHash;
    m_valid = true;
    if (reset) m_samples = 0;
    return reset;
}

} // namespace ox::render::rt
