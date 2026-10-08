#include <oxwald/world/streaming.hpp>

#include <oxwald/core/log.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ox::world {

// --- ThreadPoolExecutor ----------------------------------------------------------------------

ThreadPoolExecutor::ThreadPoolExecutor(u32 threads) {
    threads = std::max(1u, threads);
    for (u32 i = 0; i < threads; ++i) {
        m_threads.emplace_back([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lk(m_mutex);
                    m_cv.wait(lk, [this] { return m_stop || !m_queue.empty(); });
                    if (m_queue.empty()) {
                        return; // stopping and drained
                    }
                    job = std::move(m_queue.front());
                    m_queue.pop_front();
                }
                job();
            }
        });
    }
}

ThreadPoolExecutor::~ThreadPoolExecutor() {
    {
        std::lock_guard lk(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto& t : m_threads) {
        t.join();
    }
}

void ThreadPoolExecutor::submit(std::function<void()> job) {
    {
        std::lock_guard lk(m_mutex);
        m_queue.push_back(std::move(job));
    }
    m_cv.notify_one();
}

// --- ChunkStreamer -----------------------------------------------------------------------------

ChunkStreamer::ChunkStreamer(const ChunkStreamerSettings& settings, ChunkCallbacks callbacks, std::shared_ptr<IChunkExecutor> executor)
    : m_settings(settings), m_callbacks(std::move(callbacks)), m_executor(std::move(executor)) {
    if (!m_executor) {
        m_executor = std::make_shared<ThreadPoolExecutor>(2);
    }
    if (m_settings.unloadRadius < m_settings.loadRadius) {
        OX_LOG_WARN("world", "ChunkStreamer: unloadRadius < loadRadius, clamping (no hysteresis)");
        m_settings.unloadRadius = m_settings.loadRadius;
    }
}

ChunkStreamer::~ChunkStreamer() {
    for (auto& [c, chunk] : m_chunks) {
        if (chunk.cancel) {
            chunk.cancel->store(true);
        }
    }
    std::unique_lock lk(m_shared->mutex);
    m_shared->cv.wait(lk, [this] { return m_shared->pending == 0; });
}

f32 ChunkStreamer::distanceToChunk(ChunkCoord c, glm::vec3 p) const {
    const glm::vec2 lo = chunkOrigin(c), hi = lo + glm::vec2(m_settings.chunkSize);
    const glm::vec2 q(p.x, p.z);
    const glm::vec2 d = q - glm::clamp(q, lo, hi);
    return glm::length(d);
}

ChunkCoord ChunkStreamer::chunkAt(glm::vec3 p) const {
    return {i32(std::floor(p.x / m_settings.chunkSize)), i32(std::floor(p.z / m_settings.chunkSize))};
}

ChunkState ChunkStreamer::state(ChunkCoord c) const {
    const auto it = m_chunks.find(c);
    return it == m_chunks.end() ? ChunkState::Unloaded : it->second.state;
}

ChunkPayload* ChunkStreamer::payload(ChunkCoord c) const {
    const auto it = m_chunks.find(c);
    return it != m_chunks.end() && it->second.state == ChunkState::Loaded ? it->second.payload.get() : nullptr;
}

void ChunkStreamer::forEachChunk(const std::function<void(ChunkCoord, ChunkState)>& fn) const {
    for (const auto& [c, chunk] : m_chunks) {
        fn(c, chunk.state);
    }
}

bool ChunkStreamer::isAreaReady(glm::vec3 p, f32 radius) const {
    // ±1 chunk: a chunk whose edge lies exactly at the radius still counts.
    const ChunkCoord lo = chunkAt(p - glm::vec3(radius)), hi = chunkAt(p + glm::vec3(radius));
    for (i32 z = lo.z - 1; z <= hi.z + 1; ++z) {
        for (i32 x = lo.x - 1; x <= hi.x + 1; ++x) {
            if (distanceToChunk({x, z}, p) <= radius && state({x, z}) != ChunkState::Loaded) {
                return false;
            }
        }
    }
    return true;
}

void ChunkStreamer::requestLoad(ChunkCoord c, Chunk& chunk) {
    chunk.state = ChunkState::Loading;
    chunk.generation = ++m_generation;
    chunk.cancel = std::make_shared<std::atomic<bool>>(false);
    chunk.reloadAfterUnload = false;
    ++m_inFlightLoads;
    ++m_stats.requested;
    {
        std::lock_guard lk(m_shared->mutex);
        ++m_shared->pending;
    }
    m_executor->submit([shared = m_shared, load = m_callbacks.load, c, gen = chunk.generation, cancel = chunk.cancel] {
        std::unique_ptr<ChunkPayload> p;
        if (load && !cancel->load()) {
            try {
                p = load(c, *cancel);
            } catch (...) {
                p.reset(); // user exceptions never cross into the streamer
            }
        }
        std::lock_guard lk(shared->mutex);
        shared->completed.push_back({c, gen, std::move(p), false});
        --shared->pending;
        shared->cv.notify_all();
    });
}

void ChunkStreamer::beginUnload(ChunkCoord c, Chunk& chunk) {
    if (chunk.state == ChunkState::Loading) {
        chunk.cancel->store(true);
        ++m_stats.cancelled;
        m_chunks.erase(c);
        return;
    }
    if (chunk.state != ChunkState::Loaded) {
        return;
    }
    ++m_stats.unloaded;
    if (m_callbacks.onUnload && chunk.payload) {
        m_callbacks.onUnload(c, *chunk.payload);
    }
    if (!m_callbacks.save || !chunk.payload) {
        m_chunks.erase(c);
        return;
    }
    chunk.state = ChunkState::Unloading;
    chunk.generation = ++m_generation;
    {
        std::lock_guard lk(m_shared->mutex);
        ++m_shared->pending;
    }
    std::shared_ptr<ChunkPayload> payload(std::move(chunk.payload));
    m_executor->submit([shared = m_shared, save = m_callbacks.save, c, gen = chunk.generation, payload] {
        try {
            save(c, *payload);
        } catch (...) {
        }
        std::lock_guard lk(shared->mutex);
        shared->completed.push_back({c, gen, nullptr, true});
        --shared->pending;
        shared->cv.notify_all();
    });
}

void ChunkStreamer::drainCompletions(u32 budget) {
    std::vector<Completion> done;
    {
        std::lock_guard lk(m_shared->mutex);
        done.swap(m_shared->completed);
    }
    for (Completion& d : done) {
        auto it = m_chunks.find(d.coord);
        const bool current = it != m_chunks.end() && it->second.generation == d.generation;
        if (d.isSave) {
            if (current && it->second.state == ChunkState::Unloading) {
                m_chunks.erase(it); // reloadAfterUnload: the next update re-requests it
            }
            continue;
        }
        --m_inFlightLoads;
        if (!current || it->second.state != ChunkState::Loading) {
            continue; // cancelled
        }
        if (!d.payload) {
            OX_LOG_WARN("world", "chunk ({}, {}) failed to load; retrying later", d.coord.x, d.coord.z);
            m_failedUntil[d.coord] = m_updateIndex + m_settings.failedRetryUpdates;
            m_chunks.erase(it);
            continue;
        }
        m_ready.push_back(std::move(d));
    }
    u32 activated = 0;
    while (!m_ready.empty() && activated < budget) {
        Completion d = std::move(m_ready.front());
        m_ready.erase(m_ready.begin());
        auto it = m_chunks.find(d.coord);
        if (it == m_chunks.end() || it->second.generation != d.generation || it->second.state != ChunkState::Loading) {
            continue;
        }
        it->second.state = ChunkState::Loaded;
        it->second.payload = std::move(d.payload);
        it->second.cancel.reset();
        if (m_callbacks.onLoaded) {
            m_callbacks.onLoaded(d.coord, *it->second.payload);
        }
        ++activated;
        ++m_stats.activated;
    }
}

void ChunkStreamer::update(std::span<const StreamingViewer> viewers) {
    ++m_updateIndex;
    m_stats = {};

    for (const StreamingViewer& v : viewers) {
        auto it = m_lastViewerPos.find(v.id);
        if (it != m_lastViewerPos.end() && glm::distance(it->second, v.position) > m_settings.teleportDistance) {
            m_stats.teleported = true;
        }
        m_lastViewerPos[v.id] = v.position;
    }
    if (m_stats.teleported) {
        m_teleportBoostUpdates = 30;
    }
    const u32 mult = m_teleportBoostUpdates > 0 ? std::max(1u, m_settings.teleportBudgetMultiplier) : 1u;
    if (m_teleportBoostUpdates > 0) {
        --m_teleportBoostUpdates;
    }
    const u32 activationBudget = m_settings.maxActivationsPerUpdate * mult;

    // Apply finished jobs first so this update sees fresh states.
    drainCompletions(activationBudget);

    auto minDistance = [&](ChunkCoord c) {
        f32 d = std::numeric_limits<f32>::max();
        for (const StreamingViewer& v : viewers) {
            d = std::min(d, distanceToChunk(c, v.position) / std::max(v.radiusScale, 1e-3f));
        }
        return d;
    };

    // --- unload (hysteresis: only beyond unloadRadius) ---
    std::vector<std::pair<f32, ChunkCoord>> unloadList;
    std::vector<ChunkCoord> cancelList;
    for (const auto& [c, chunk] : m_chunks) {
        const f32 d = minDistance(c);
        if (d <= m_settings.unloadRadius) {
            continue;
        }
        if (chunk.state == ChunkState::Loading) {
            cancelList.push_back(c);
        } else if (chunk.state == ChunkState::Loaded) {
            unloadList.emplace_back(d, c);
        }
    }
    for (ChunkCoord c : cancelList) {
        beginUnload(c, m_chunks[c]);
    }
    std::sort(unloadList.begin(), unloadList.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : (a.second.x != b.second.x ? a.second.x < b.second.x : a.second.z < b.second.z);
    });
    for (usize i = 0; i < unloadList.size() && i < m_settings.maxUnloadsPerUpdate; ++i) {
        beginUnload(unloadList[i].second, m_chunks[unloadList[i].second]);
    }

    // --- load candidates (nearest / in view first) ---
    std::unordered_map<ChunkCoord, f32, ChunkCoordHash> candidates;
    for (const StreamingViewer& v : viewers) {
        const f32 r = m_settings.loadRadius * v.radiusScale;
        const ChunkCoord lo = chunkAt(v.position - glm::vec3(r)), hi = chunkAt(v.position + glm::vec3(r));
        glm::vec2 fwd(v.forward.x, v.forward.z);
        fwd = glm::dot(fwd, fwd) > 1e-8f ? glm::normalize(fwd) : glm::vec2(0.f);
        for (i32 z = lo.z - 1; z <= hi.z + 1; ++z) {
            for (i32 x = lo.x - 1; x <= hi.x + 1; ++x) {
                const ChunkCoord c{x, z};
                const f32 d = distanceToChunk(c, v.position);
                if (d > r) {
                    continue;
                }
                const glm::vec2 center = chunkOrigin(c) + glm::vec2(m_settings.chunkSize * 0.5f);
                glm::vec2 to = center - glm::vec2(v.position.x, v.position.z);
                to = glm::dot(to, to) > 1e-8f ? glm::normalize(to) : glm::vec2(0.f);
                const f32 bonus = glm::clamp(m_settings.viewDirectionWeight, 0.f, 1.f) * std::max(0.f, glm::dot(fwd, to));
                const f32 prio = d / std::max(v.radiusScale, 1e-3f) * (1.f - bonus);
                auto [it, inserted] = candidates.try_emplace(c, prio);
                if (!inserted) {
                    it->second = std::min(it->second, prio);
                }
            }
        }
    }
    std::vector<std::pair<f32, ChunkCoord>> toLoad;
    for (const auto& [c, prio] : candidates) {
        auto it = m_chunks.find(c);
        if (it != m_chunks.end()) {
            if (it->second.state == ChunkState::Unloading) {
                it->second.reloadAfterUnload = true;
            }
            continue;
        }
        auto f = m_failedUntil.find(c);
        if (f != m_failedUntil.end()) {
            if (f->second > m_updateIndex) {
                continue;
            }
            m_failedUntil.erase(f);
        }
        toLoad.emplace_back(prio, c);
    }
    std::sort(toLoad.begin(), toLoad.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : (a.second.x != b.second.x ? a.second.x < b.second.x : a.second.z < b.second.z);
    });
    const u32 maxInFlight = m_settings.maxInFlightLoads * mult;
    u32 issued = 0;
    for (const auto& [prio, c] : toLoad) {
        if (issued >= m_settings.maxLoadRequestsPerUpdate * mult || m_inFlightLoads >= maxInFlight) {
            break;
        }
        requestLoad(c, m_chunks[c]);
        ++issued;
    }

    // Synchronous executors finish inside submit(): apply those results in the same update.
    drainCompletions(activationBudget > m_stats.activated ? activationBudget - m_stats.activated : 0);

    for (const auto& [c, chunk] : m_chunks) {
        switch (chunk.state) {
        case ChunkState::Loading: ++m_stats.loading; break;
        case ChunkState::Loaded: ++m_stats.loaded; break;
        case ChunkState::Unloading: ++m_stats.unloading; break;
        default: break;
        }
    }
}

void ChunkStreamer::flush(std::span<const StreamingViewer> viewers) {
    for (int guard = 0; guard < 100000; ++guard) {
        {
            std::unique_lock lk(m_shared->mutex);
            m_shared->cv.wait(lk, [this] { return m_shared->pending == 0; });
        }
        drainCompletions(std::numeric_limits<u32>::max());
        if (viewers.empty()) {
            return;
        }
        update(viewers);
        bool idle = m_stats.requested == 0 && m_ready.empty() && m_stats.loading == 0 && m_stats.unloading == 0;
        {
            std::lock_guard lk(m_shared->mutex);
            idle = idle && m_shared->pending == 0 && m_shared->completed.empty();
        }
        if (idle) {
            return;
        }
    }
}

void ChunkStreamer::unloadAll() {
    std::vector<ChunkCoord> coords;
    for (const auto& [c, chunk] : m_chunks) {
        coords.push_back(c);
    }
    for (ChunkCoord c : coords) {
        auto it = m_chunks.find(c);
        if (it != m_chunks.end()) {
            beginUnload(c, it->second);
        }
    }
}

void ChunkStreamer::debugDraw(const DebugLineFn& line, f32 y) const {
    if (!line) {
        return;
    }
    for (const auto& [c, chunk] : m_chunks) {
        glm::vec4 col(0.5f);
        switch (chunk.state) {
        case ChunkState::Loading: col = {1.f, 0.9f, 0.1f, 1.f}; break;
        case ChunkState::Loaded: col = {0.1f, 0.9f, 0.2f, 1.f}; break;
        case ChunkState::Unloading: col = {1.f, 0.2f, 0.1f, 1.f}; break;
        default: break;
        }
        const glm::vec2 o = chunkOrigin(c) + glm::vec2(0.5f); // inset so neighbours' outlines don't overlap
        const f32 s = m_settings.chunkSize - 1.f;
        const glm::vec3 a{o.x, y, o.y}, b{o.x + s, y, o.y}, cc{o.x + s, y, o.y + s}, d{o.x, y, o.y + s};
        line(a, b, col);
        line(b, cc, col);
        line(cc, d, col);
        line(d, a, col);
    }
}

} // namespace ox::world
