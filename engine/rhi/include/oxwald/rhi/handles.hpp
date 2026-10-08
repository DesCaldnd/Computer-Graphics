#pragma once

#include <oxwald/core/assert.hpp>
#include <oxwald/core/types.hpp>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace ox::rhi {

// Generational handle: index into a pool slot + generation that changes every time the slot is reused.
// A stale handle (resource destroyed, slot reused) never resolves to the new object.
template <class Tag>
struct Handle {
    u32 index = 0;
    u32 generation = 0; // 0 = null handle

    [[nodiscard]] constexpr bool valid() const { return generation != 0; }
    constexpr explicit operator bool() const { return valid(); }
    constexpr bool operator==(const Handle&) const = default;
    [[nodiscard]] constexpr u64 packed() const { return (u64(generation) << 32) | index; }
};

struct BufferTag;
struct TextureTag;
struct SamplerTag;
struct PipelineTag;
struct AccelStructTag;

using BufferHandle = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;
using SamplerHandle = Handle<SamplerTag>;
using PipelineHandle = Handle<PipelineTag>;
using AccelStructHandle = Handle<AccelStructTag>;

template <class T, class Tag>
class HandlePool {
public:
    using HandleType = Handle<Tag>;

    HandleType allocate(T value) {
        u32 index;
        if (!m_freeList.empty()) {
            index = m_freeList.back();
            m_freeList.pop_back();
        } else {
            index = u32(m_slots.size());
            m_slots.push_back({});
        }
        Slot& slot = m_slots[index];
        slot.value.emplace(std::move(value));
        ++m_alive;
        return {index, slot.generation};
    }

    [[nodiscard]] T* get(HandleType h) {
        if (!h.valid() || h.index >= m_slots.size()) {
            return nullptr;
        }
        Slot& s = m_slots[h.index];
        return (s.generation == h.generation && s.value) ? &*s.value : nullptr;
    }
    [[nodiscard]] const T* get(HandleType h) const { return const_cast<HandlePool*>(this)->get(h); }

    [[nodiscard]] T& at(HandleType h) {
        T* p = get(h);
        OX_ASSERT(p, "stale or null handle (index {}, generation {})", h.index, h.generation);
        return *p;
    }

    // Returns the removed value so the caller can schedule its GPU objects for deferred destruction.
    std::optional<T> release(HandleType h) {
        T* p = get(h);
        if (!p) {
            return std::nullopt;
        }
        Slot& s = m_slots[h.index];
        std::optional<T> out(std::move(*s.value));
        s.value.reset();
        s.generation = s.generation == ~0u ? 1u : s.generation + 1u;
        m_freeList.push_back(h.index);
        --m_alive;
        return out;
    }

    template <class Fn>
    void forEach(Fn&& fn) {
        for (u32 i = 0; i < m_slots.size(); ++i) {
            if (m_slots[i].value) {
                fn(HandleType{i, m_slots[i].generation}, *m_slots[i].value);
            }
        }
    }

    [[nodiscard]] usize size() const { return m_alive; }
    [[nodiscard]] usize capacity() const { return m_slots.size(); }

private:
    struct Slot {
        std::optional<T> value;
        u32 generation = 1;
    };
    std::vector<Slot> m_slots;
    std::vector<u32> m_freeList;
    usize m_alive = 0;
};

// Index allocator for bindless descriptor slots (free list, lowest indices reused first-in-last-out).
class IndexAllocator {
public:
    explicit IndexAllocator(u32 capacity = 0, u32 reservedLow = 0) { reset(capacity, reservedLow); }
    void reset(u32 capacity, u32 reservedLow = 0) {
        m_capacity = capacity;
        m_reserved = reservedLow;
        m_next = reservedLow;
        m_free.clear();
    }
    // Returns ~0u when exhausted.
    u32 allocate() {
        if (!m_free.empty()) {
            u32 i = m_free.back();
            m_free.pop_back();
            return i;
        }
        return m_next < m_capacity ? m_next++ : ~0u;
    }
    void free(u32 index) {
        if (index != ~0u) {
            m_free.push_back(index);
        }
    }
    [[nodiscard]] u32 capacity() const { return m_capacity; }
    [[nodiscard]] u32 used() const { return m_next - m_reserved - u32(m_free.size()); }

private:
    u32 m_capacity = 0;
    u32 m_reserved = 0;
    u32 m_next = 0;
    std::vector<u32> m_free;
};

inline constexpr u32 kInvalidBindlessIndex = ~0u;

} // namespace ox::rhi

template <class Tag>
struct std::hash<ox::rhi::Handle<Tag>> {
    size_t operator()(const ox::rhi::Handle<Tag>& h) const noexcept { return std::hash<ox::u64>{}(h.packed()); }
};
