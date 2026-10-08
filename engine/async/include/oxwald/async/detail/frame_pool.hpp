#pragma once

// Pooled allocation for coroutine frames and scheduler records.
//
// Frames are carved from 64 KiB chunks into size classes (64-byte granularity, up to 2 KiB); larger frames fall
// back to ::operator new. Free lists are thread-local, so a frame freed on another thread simply migrates to that
// thread's list. Chunks are never returned to the OS (they stay reachable, so LeakSanitizer stays quiet).
// The pool is compiled out under AddressSanitizer (or with OX_ASYNC_NO_FRAME_POOL) so ASan sees every frame.

#include <oxwald/core/types.hpp>

#include <cstddef>
#include <new>

namespace ox {

struct CoroutineFrameStats {
    u64 liveFrames = 0;       // allocated and not yet freed (frames + pooled records)
    u64 totalAllocations = 0; // since process start
    u64 pooledAllocations = 0;
    u64 heapAllocations = 0;  // allocations that went to ::operator new (large frames or pool disabled)
    u64 chunkAllocations = 0; // 64 KiB pool chunks
    bool poolEnabled = false;
};

[[nodiscard]] CoroutineFrameStats coroutineFrameStats();

namespace detail {

void* framePoolAllocate(std::size_t size);
void framePoolFree(void* p, std::size_t size) noexcept;

// std-compatible allocator on top of the frame pool (used with std::allocate_shared for task records).
template <class T>
struct PoolAllocator {
    using value_type = T;
    PoolAllocator() noexcept = default;
    template <class U>
    PoolAllocator(const PoolAllocator<U>&) noexcept {} // NOLINT: rebinding conversion
    T* allocate(std::size_t n) { return static_cast<T*>(framePoolAllocate(n * sizeof(T))); }
    void deallocate(T* p, std::size_t n) noexcept { framePoolFree(p, n * sizeof(T)); }
    template <class U>
    bool operator==(const PoolAllocator<U>&) const noexcept {
        return true;
    }
};

} // namespace detail
} // namespace ox
