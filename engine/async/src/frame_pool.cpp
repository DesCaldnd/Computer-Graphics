#include <oxwald/async/detail/frame_pool.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OX_ASYNC_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define OX_ASYNC_ASAN 1
#endif

#if defined(OX_ASYNC_NO_FRAME_POOL) || defined(OX_ASYNC_ASAN)
#define OX_ASYNC_POOL 0
#else
#define OX_ASYNC_POOL 1
#endif

namespace ox {

namespace {

constexpr std::size_t kGranularity = 64;
constexpr std::size_t kMaxPooled = 2048;
constexpr std::size_t kClassCount = kMaxPooled / kGranularity;
constexpr std::size_t kChunkSize = 64 * 1024;

std::atomic<u64> g_live{0};
std::atomic<u64> g_total{0};
std::atomic<u64> g_pooled{0};
std::atomic<u64> g_heap{0};
std::atomic<u64> g_chunks{0};

#if OX_ASYNC_POOL
struct FreeNode {
    FreeNode* next;
};

// Chunks stay reachable from here forever (never freed), so leak checkers do not report pooled frames.
struct ChunkRegistry {
    std::mutex mutex;
    std::vector<void*> chunks;
};
ChunkRegistry& chunkRegistry() {
    static auto* registry = new ChunkRegistry(); // intentionally immortal: frames may be freed during static teardown
    return *registry;
}

struct ThreadPool {
    std::array<FreeNode*, kClassCount> free{};

    void refill(std::size_t cls) {
        const std::size_t size = (cls + 1) * kGranularity;
        auto* chunk = static_cast<std::byte*>(::operator new(kChunkSize));
        g_chunks.fetch_add(1, std::memory_order_relaxed);
        {
            auto& reg = chunkRegistry();
            std::lock_guard lock(reg.mutex);
            reg.chunks.push_back(chunk);
        }
        const std::size_t n = kChunkSize / size;
        for (std::size_t i = 0; i < n; ++i) {
            auto* node = reinterpret_cast<FreeNode*>(chunk + i * size);
            node->next = free[cls];
            free[cls] = node;
        }
    }
};
thread_local ThreadPool t_pool;
#endif

} // namespace

CoroutineFrameStats coroutineFrameStats() {
    CoroutineFrameStats s;
    s.liveFrames = g_live.load(std::memory_order_relaxed);
    s.totalAllocations = g_total.load(std::memory_order_relaxed);
    s.pooledAllocations = g_pooled.load(std::memory_order_relaxed);
    s.heapAllocations = g_heap.load(std::memory_order_relaxed);
    s.chunkAllocations = g_chunks.load(std::memory_order_relaxed);
    s.poolEnabled = OX_ASYNC_POOL != 0;
    return s;
}

namespace detail {

void* framePoolAllocate(std::size_t size) {
    g_live.fetch_add(1, std::memory_order_relaxed);
    g_total.fetch_add(1, std::memory_order_relaxed);
#if OX_ASYNC_POOL
    if (size != 0 && size <= kMaxPooled) {
        const std::size_t cls = (size - 1) / kGranularity;
        FreeNode*& head = t_pool.free[cls];
        if (!head) {
            t_pool.refill(cls);
        }
        FreeNode* node = head;
        head = node->next;
        g_pooled.fetch_add(1, std::memory_order_relaxed);
        return node;
    }
#endif
    g_heap.fetch_add(1, std::memory_order_relaxed);
    return ::operator new(size);
}

void framePoolFree(void* p, std::size_t size) noexcept {
    if (!p) {
        return;
    }
    g_live.fetch_sub(1, std::memory_order_relaxed);
#if OX_ASYNC_POOL
    if (size != 0 && size <= kMaxPooled) {
        const std::size_t cls = (size - 1) / kGranularity;
        auto* node = static_cast<FreeNode*>(p);
        node->next = t_pool.free[cls];
        t_pool.free[cls] = node;
        return;
    }
#endif
    ::operator delete(p);
}

} // namespace detail
} // namespace ox
