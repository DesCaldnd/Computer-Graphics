#pragma once

#include <oxwald/world/common.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ox::world {

struct ChunkCoord {
    i32 x = 0, z = 0;
    bool operator==(const ChunkCoord&) const = default;
};
struct ChunkCoordHash {
    usize operator()(const ChunkCoord& c) const { return usize(u64(u32(c.x)) << 32 | u32(c.z)) * 0x9E3779B97F4A7C15ull; }
};

// --- executors --------------------------------------------------------------------------------
// Pluggable async executor for load/save jobs. The integration layer can adapt the core job system;
// ThreadPoolExecutor is the stand-alone default.
class IChunkExecutor {
public:
    virtual ~IChunkExecutor() = default;
    virtual void submit(std::function<void()> job) = 0;
};

class ThreadPoolExecutor final : public IChunkExecutor {
public:
    explicit ThreadPoolExecutor(u32 threads = 2);
    ~ThreadPoolExecutor() override; // finishes queued jobs, then joins
    void submit(std::function<void()> job) override;

private:
    std::vector<std::thread> m_threads;
    std::deque<std::function<void()>> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
};

// Runs jobs immediately on the calling thread (deterministic tests, single-threaded tools).
class InlineExecutor final : public IChunkExecutor {
public:
    void submit(std::function<void()> job) override { job(); }
};

// --- streamer ---------------------------------------------------------------------------------
enum class ChunkState : u8 { Unloaded, Loading, Loaded, Unloading };

// Base for whatever a chunk loads (ChunkData, or game-specific). Owned by the streamer while Loaded.
struct ChunkPayload {
    virtual ~ChunkPayload() = default;
};

struct StreamingViewer {
    glm::vec3 position{0.f};
    glm::vec3 forward{0.f, 0.f, -1.f};
    f32 radiusScale = 1.f; // e.g. < 1 for secondary viewers (remote players, cinematic cameras)
    u32 id = 0;            // stable id, used for teleport detection
};

struct ChunkStreamerSettings {
    f32 chunkSize = 128.f;           // metres, chunks are square cells on the XZ plane
    f32 loadRadius = 512.f;          // chunks closer than this (to the chunk rect) are requested
    f32 unloadRadius = 640.f;        // chunks farther than this from every viewer are unloaded (hysteresis)
    u32 maxLoadRequestsPerUpdate = 4;
    u32 maxInFlightLoads = 8;
    u32 maxActivationsPerUpdate = 4; // finished loads handed to onLoaded per update (main-thread cost)
    u32 maxUnloadsPerUpdate = 8;
    f32 viewDirectionWeight = 0.5f;  // [0,1]: chunks in front of a viewer get up to this priority bonus
    f32 teleportDistance = 256.f;    // viewer jump that counts as a teleport
    u32 teleportBudgetMultiplier = 4; // request/activation budget boost while a teleport area is loading
    u32 failedRetryUpdates = 60;     // updates to wait before retrying a failed load
};

struct ChunkCallbacks {
    // Worker thread. Return nullptr on failure. Poll `cancelled` in long loads and bail out early.
    std::function<std::unique_ptr<ChunkPayload>(ChunkCoord, const std::atomic<bool>& cancelled)> load;
    // Main thread (inside update()): the chunk became Loaded — create entities, upload GPU data, ...
    std::function<void(ChunkCoord, ChunkPayload&)> onLoaded;
    // Main thread: the chunk is leaving — remove it from the world (also called from ~ChunkStreamer).
    std::function<void(ChunkCoord, ChunkPayload&)> onUnload;
    // Optional, worker thread: persist the payload before it is destroyed (chunk stays Unloading).
    std::function<void(ChunkCoord, ChunkPayload&)> save;
};

struct ChunkStreamerStats {
    u32 loading = 0, loaded = 0, unloading = 0;
    u32 requested = 0, activated = 0, unloaded = 0, cancelled = 0; // during the last update
    bool teleported = false;
};

class ChunkStreamer {
public:
    // executor == nullptr → an internal ThreadPoolExecutor(2).
    ChunkStreamer(const ChunkStreamerSettings& settings, ChunkCallbacks callbacks, std::shared_ptr<IChunkExecutor> executor = nullptr);
    // Cancels in-flight loads, waits for running jobs, then unloads every Loaded chunk (onUnload + save, on the
    // destroying thread) so the world never keeps entities of a dead streamer.
    ~ChunkStreamer();
    ChunkStreamer(const ChunkStreamer&) = delete;
    ChunkStreamer& operator=(const ChunkStreamer&) = delete;

    void update(std::span<const StreamingViewer> viewers);
    // Block until all submitted jobs finished and were applied (loading screens, tests, shutdown).
    void flush(std::span<const StreamingViewer> viewers = {});
    void unloadAll();

    [[nodiscard]] ChunkState state(ChunkCoord c) const;
    [[nodiscard]] ChunkPayload* payload(ChunkCoord c) const;
    [[nodiscard]] bool isAreaReady(glm::vec3 position, f32 radius) const; // every chunk within radius is Loaded
    [[nodiscard]] ChunkCoord chunkAt(glm::vec3 position) const;
    [[nodiscard]] glm::vec2 chunkOrigin(ChunkCoord c) const { return glm::vec2(f32(c.x), f32(c.z)) * m_settings.chunkSize; }
    [[nodiscard]] const ChunkStreamerSettings& settings() const { return m_settings; }
    [[nodiscard]] const ChunkStreamerStats& stats() const { return m_stats; }
    void forEachChunk(const std::function<void(ChunkCoord, ChunkState)>& fn) const;
    // Coloured outlines: Loading yellow, Loaded green, Unloading red.
    void debugDraw(const DebugLineFn& line, f32 y = 0.f) const;

private:
    struct Chunk {
        ChunkState state = ChunkState::Unloaded;
        std::unique_ptr<ChunkPayload> payload;
        std::shared_ptr<std::atomic<bool>> cancel;
        u64 generation = 0;
        bool reloadAfterUnload = false;
    };
    struct Completion {
        ChunkCoord coord;
        u64 generation = 0;
        std::unique_ptr<ChunkPayload> payload;
        bool isSave = false;
    };
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<Completion> completed;
        u32 pending = 0;
    };

    void drainCompletions(u32 activationBudget);
    void requestLoad(ChunkCoord c, Chunk& chunk);
    void beginUnload(ChunkCoord c, Chunk& chunk);
    [[nodiscard]] f32 distanceToChunk(ChunkCoord c, glm::vec3 p) const;

    ChunkStreamerSettings m_settings;
    ChunkCallbacks m_callbacks;
    std::shared_ptr<IChunkExecutor> m_executor;
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash> m_chunks;
    std::unordered_map<ChunkCoord, u64, ChunkCoordHash> m_failedUntil; // update index
    std::vector<Completion> m_ready; // finished loads waiting for activation budget
    std::unordered_map<u32, glm::vec3> m_lastViewerPos;
    u64 m_generation = 0;
    u64 m_updateIndex = 0;
    u32 m_inFlightLoads = 0;
    u32 m_teleportBoostUpdates = 0;
    ChunkStreamerStats m_stats{};
};

} // namespace ox::world
