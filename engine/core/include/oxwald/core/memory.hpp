#pragma once

// Allocators and generational handles. None of these types is thread-safe: use one instance per
// thread (e.g. a FrameAllocator per worker) or guard externally.

#include <oxwald/core/assert.hpp>
#include <oxwald/core/types.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace ox {

inline constexpr usize kDefaultAlignment = alignof(std::max_align_t);

[[nodiscard]] constexpr bool isPowerOfTwo(usize x) { return x != 0 && (x & (x - 1)) == 0; }
[[nodiscard]] constexpr usize alignUp(usize value, usize alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// ---------------------------------------------------------------------------------------------
// FrameAllocator: linear bump allocator for per-frame scratch memory. Memory is never freed
// individually; reset() recycles everything at once. When the current block is full a new block
// is chained (growth fallback); on reset() multiple blocks are coalesced into one big block so the
// steady state is a single allocation. Destructors are never run, so only trivially destructible
// types may be created.

struct FrameAllocatorStats {
    usize usedBytes = 0;      // bytes handed out since the last reset (including alignment padding)
    usize capacityBytes = 0;  // sum of all block sizes
    usize peakUsedBytes = 0;  // max usedBytes observed over the lifetime
    usize blockCount = 0;
    usize allocationCount = 0; // since the last reset
};

class FrameAllocator {
public:
    explicit FrameAllocator(usize blockSize = 1024 * 1024);
    ~FrameAllocator();
    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;
    FrameAllocator(FrameAllocator&& other) noexcept;
    FrameAllocator& operator=(FrameAllocator&& other) noexcept;

    // Never returns nullptr (size 0 returns a valid unique-ish pointer). alignment must be a power of two.
    [[nodiscard]] void* allocate(usize size, usize alignment = kDefaultAlignment);

    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "FrameAllocator never runs destructors");
        return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    // Default-initialised array (indeterminate values for trivial types).
    template <class T>
    [[nodiscard]] std::span<T> allocArray(usize count) {
        static_assert(std::is_trivially_destructible_v<T>, "FrameAllocator never runs destructors");
        if (count == 0) {
            return {};
        }
        T* p = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
        std::uninitialized_default_construct_n(p, count);
        return {p, count};
    }

    template <class T>
    [[nodiscard]] std::span<T> copyArray(std::span<const T> src) {
        std::span<T> dst = allocArray<T>(src.size());
        std::uninitialized_copy(src.begin(), src.end(), dst.begin());
        return dst;
    }

    void reset();
    // Frees every block (next allocation creates a fresh one).
    void release();

    [[nodiscard]] FrameAllocatorStats stats() const;
    [[nodiscard]] usize blockSize() const { return m_blockSize; }
    [[nodiscard]] bool owns(const void* p) const;

private:
    struct Block {
        std::byte* data = nullptr;
        usize size = 0;
    };
    static Block allocBlock(usize size);
    static void freeBlock(Block& b);

    std::vector<Block> m_blocks;
    usize m_current = 0; // index of the block being bumped
    usize m_offset = 0;  // offset inside m_blocks[m_current]
    usize m_usedBeforeCurrent = 0;
    usize m_allocationCount = 0;
    usize m_peak = 0;
    usize m_blockSize;
};

// N frame allocators used round-robin, one per frame in flight: data allocated in frame F stays
// valid until beginFrame() is called with an index that maps to the same slot (F + N).
class MultiFrameAllocator {
public:
    explicit MultiFrameAllocator(u32 framesInFlight = 2, usize blockSize = 1024 * 1024);

    // Selects allocator frameIndex % N and resets it.
    void beginFrame(u64 frameIndex);
    [[nodiscard]] FrameAllocator& current() { return m_allocators[m_current]; }
    [[nodiscard]] FrameAllocator& frame(u32 slot) { return m_allocators.at(slot); }
    [[nodiscard]] u32 frameCount() const { return static_cast<u32>(m_allocators.size()); }
    [[nodiscard]] u32 currentSlot() const { return m_current; }

    [[nodiscard]] void* allocate(usize size, usize alignment = kDefaultAlignment) {
        return current().allocate(size, alignment);
    }
    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        return current().create<T>(std::forward<Args>(args)...);
    }
    template <class T>
    [[nodiscard]] std::span<T> allocArray(usize count) {
        return current().allocArray<T>(count);
    }

private:
    std::vector<FrameAllocator> m_allocators;
    u32 m_current = 0;
};

template <u32 N>
class FrameAllocatorRing : public MultiFrameAllocator {
    static_assert(N > 0);

public:
    explicit FrameAllocatorRing(usize blockSize = 1024 * 1024) : MultiFrameAllocator(N, blockSize) {}
};

// ---------------------------------------------------------------------------------------------
// PoolAllocator: fixed-size blocks with an intrusive free list; grows by whole pages and never
// returns pages to the system until destruction/release().

struct PoolAllocatorStats {
    usize liveBlocks = 0;
    usize capacityBlocks = 0;
    usize pageCount = 0;
    usize blockSize = 0;
};

class PoolAllocator {
public:
    explicit PoolAllocator(usize blockSize, usize alignment = kDefaultAlignment, usize blocksPerPage = 64);
    ~PoolAllocator();
    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&& other) noexcept;
    PoolAllocator& operator=(PoolAllocator&& other) noexcept;

    [[nodiscard]] void* allocate();
    void deallocate(void* p);
    // Frees all pages. Every outstanding pointer becomes dangling.
    void release();

    [[nodiscard]] bool owns(const void* p) const;
    [[nodiscard]] PoolAllocatorStats stats() const;
    [[nodiscard]] usize blockSize() const { return m_blockSize; }
    [[nodiscard]] usize liveCount() const { return m_live; }

private:
    struct FreeNode {
        FreeNode* next;
    };
    void addPage();

    std::vector<std::byte*> m_pages;
    FreeNode* m_free = nullptr;
    usize m_blockSize;
    usize m_alignment;
    usize m_blocksPerPage;
    usize m_live = 0;
};

template <class T>
class TypedPool {
public:
    explicit TypedPool(usize objectsPerPage = 64) : m_pool(sizeof(T), alignof(T), objectsPerPage) {}
    ~TypedPool() {
        if (m_pool.liveCount() != 0) {
            OX_LOG_WARN("memory", "TypedPool destroyed with {} live objects (destructors not run)", m_pool.liveCount());
        }
    }
    TypedPool(TypedPool&&) noexcept = default;
    TypedPool& operator=(TypedPool&&) noexcept = default;

    template <class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        void* mem = m_pool.allocate();
        if constexpr (std::is_nothrow_constructible_v<T, Args&&...>) {
            return ::new (mem) T(std::forward<Args>(args)...);
        } else {
            try {
                return ::new (mem) T(std::forward<Args>(args)...);
            } catch (...) {
                m_pool.deallocate(mem);
                throw;
            }
        }
    }
    void destroy(T* p) {
        if (p == nullptr) {
            return;
        }
        p->~T();
        m_pool.deallocate(p);
    }
    [[nodiscard]] usize liveCount() const { return m_pool.liveCount(); }
    [[nodiscard]] bool owns(const T* p) const { return m_pool.owns(p); }
    [[nodiscard]] PoolAllocatorStats stats() const { return m_pool.stats(); }

private:
    PoolAllocator m_pool;
};

// ---------------------------------------------------------------------------------------------
// Generational handles. generation 0 is never issued, so a value-initialised handle is invalid.

template <class Tag>
struct Handle {
    u32 index = 0;
    u32 generation = 0;

    [[nodiscard]] constexpr bool valid() const { return generation != 0; }
    constexpr explicit operator bool() const { return valid(); }
    [[nodiscard]] static constexpr Handle invalid() { return {}; }
    [[nodiscard]] constexpr u64 toU64() const { return (static_cast<u64>(generation) << 32) | index; }
    [[nodiscard]] static constexpr Handle fromU64(u64 v) {
        return {static_cast<u32>(v & 0xffffffffu), static_cast<u32>(v >> 32)};
    }
    constexpr bool operator==(const Handle&) const = default;
};

// Dense-ish slot storage addressed by generational handles. Destroying bumps the slot generation so
// stale handles fail get()/alive(). Freed slots are reused (LIFO). A slot whose generation would
// wrap is retired. Pointers returned by get() stay valid until that element is destroyed or the pool
// is cleared (storage is a deque, creating elements never moves existing ones).
template <class T, class Tag = T>
class HandlePool {
public:
    using HandleType = Handle<Tag>;

    HandlePool() = default;
    HandlePool(const HandlePool&) = delete;
    HandlePool& operator=(const HandlePool&) = delete;
    HandlePool(HandlePool&&) noexcept = default;
    HandlePool& operator=(HandlePool&&) noexcept = default;

    template <class... Args>
    HandleType create(Args&&... args) {
        u32 index;
        if (m_freeHead != kNoFree) {
            index = m_freeHead;
            m_freeHead = m_slots[index].nextFree;
        } else {
            OX_ASSERT(m_slots.size() < kNoFree, "HandlePool: too many slots");
            index = static_cast<u32>(m_slots.size());
            m_slots.emplace_back();
        }
        Slot& slot = m_slots[index];
        slot.value.emplace(std::forward<Args>(args)...);
        slot.nextFree = kNoFree;
        ++m_size;
        return {index, slot.generation};
    }

    bool destroy(HandleType h) {
        Slot* slot = slotFor(h);
        if (slot == nullptr) {
            return false;
        }
        slot->value.reset();
        --m_size;
        if (slot->generation == kMaxGeneration) {
            slot->generation = 0; // retired: never valid again, never reused
            return true;
        }
        ++slot->generation;
        slot->nextFree = m_freeHead;
        m_freeHead = h.index;
        return true;
    }

    [[nodiscard]] T* get(HandleType h) {
        Slot* slot = slotFor(h);
        return slot ? &*slot->value : nullptr;
    }
    [[nodiscard]] const T* get(HandleType h) const {
        const Slot* slot = const_cast<HandlePool*>(this)->slotFor(h);
        return slot ? &*slot->value : nullptr;
    }
    [[nodiscard]] bool alive(HandleType h) const { return get(h) != nullptr; }
    [[nodiscard]] usize size() const { return m_size; }
    [[nodiscard]] bool empty() const { return m_size == 0; }
    [[nodiscard]] usize capacity() const { return m_slots.size(); }

    // Visiting order is slot order. Do not create/destroy from inside fn.
    template <class Fn>
    void forEach(Fn&& fn) {
        for (u32 i = 0; i < m_slots.size(); ++i) {
            Slot& s = m_slots[i];
            if (s.value) {
                fn(HandleType{i, s.generation}, *s.value);
            }
        }
    }
    template <class Fn>
    void forEach(Fn&& fn) const {
        for (u32 i = 0; i < m_slots.size(); ++i) {
            const Slot& s = m_slots[i];
            if (s.value) {
                fn(HandleType{i, s.generation}, *s.value);
            }
        }
    }

    // Destroys every element; all outstanding handles become invalid (generations are kept).
    void clear() {
        for (u32 i = 0; i < m_slots.size(); ++i) {
            Slot& s = m_slots[i];
            if (s.value) {
                HandleType h{i, s.generation};
                destroy(h);
            }
        }
    }

private:
    static constexpr u32 kNoFree = 0xffffffffu;
    static constexpr u32 kMaxGeneration = 0xffffffffu;

    struct Slot {
        std::optional<T> value;
        u32 generation = 1;
        u32 nextFree = kNoFree;
    };

    Slot* slotFor(HandleType h) {
        if (!h.valid() || h.index >= m_slots.size()) {
            return nullptr;
        }
        Slot& s = m_slots[h.index];
        return (s.generation == h.generation && s.value) ? &s : nullptr;
    }

    std::deque<Slot> m_slots;
    u32 m_freeHead = kNoFree;
    usize m_size = 0;
};

} // namespace ox

template <class Tag>
struct std::hash<ox::Handle<Tag>> {
    std::size_t operator()(const ox::Handle<Tag>& h) const noexcept { return std::hash<ox::u64>{}(h.toU64()); }
};
