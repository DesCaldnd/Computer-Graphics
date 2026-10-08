#include <oxwald/core/memory.hpp>

#include <algorithm>
#include <functional>

namespace ox {

namespace {

// Blocks are aligned generously so typical SIMD/cache-line requests never need padding at offset 0.
constexpr usize kBlockAlignment = 64;

} // namespace

FrameAllocator::FrameAllocator(usize blockSize) : m_blockSize(std::max<usize>(blockSize, 256)) {}

FrameAllocator::~FrameAllocator() { release(); }

FrameAllocator::FrameAllocator(FrameAllocator&& other) noexcept
    : m_blocks(std::move(other.m_blocks)), m_current(other.m_current), m_offset(other.m_offset),
      m_usedBeforeCurrent(other.m_usedBeforeCurrent), m_allocationCount(other.m_allocationCount), m_peak(other.m_peak),
      m_blockSize(other.m_blockSize) {
    other.m_blocks.clear();
    other.m_current = other.m_offset = other.m_usedBeforeCurrent = other.m_allocationCount = 0;
}

FrameAllocator& FrameAllocator::operator=(FrameAllocator&& other) noexcept {
    if (this != &other) {
        release();
        m_blocks = std::move(other.m_blocks);
        m_current = other.m_current;
        m_offset = other.m_offset;
        m_usedBeforeCurrent = other.m_usedBeforeCurrent;
        m_allocationCount = other.m_allocationCount;
        m_peak = other.m_peak;
        m_blockSize = other.m_blockSize;
        other.m_blocks.clear();
        other.m_current = other.m_offset = other.m_usedBeforeCurrent = other.m_allocationCount = 0;
    }
    return *this;
}

FrameAllocator::Block FrameAllocator::allocBlock(usize size) {
    Block b;
    b.size = alignUp(size, kBlockAlignment);
    b.data = static_cast<std::byte*>(::operator new(b.size, std::align_val_t{kBlockAlignment}));
    return b;
}

void FrameAllocator::freeBlock(Block& b) {
    if (b.data != nullptr) {
        ::operator delete(b.data, std::align_val_t{kBlockAlignment});
        b.data = nullptr;
        b.size = 0;
    }
}

void* FrameAllocator::allocate(usize size, usize alignment) {
    OX_ASSERT(isPowerOfTwo(alignment), "FrameAllocator: alignment {} is not a power of two", alignment);
    size = std::max<usize>(size, 1);
    for (;;) {
        if (m_current < m_blocks.size()) {
            Block& b = m_blocks[m_current];
            const auto base = reinterpret_cast<std::uintptr_t>(b.data);
            const usize aligned = alignUp(base + m_offset, alignment) - base;
            if (aligned + size <= b.size) {
                m_offset = aligned + size;
                ++m_allocationCount;
                m_peak = std::max(m_peak, m_usedBeforeCurrent + m_offset);
                return b.data + aligned;
            }
            // Move on to the next block; whatever is left in this one is wasted for this frame.
            if (m_current + 1 < m_blocks.size()) {
                m_usedBeforeCurrent += b.size;
                ++m_current;
                m_offset = 0;
                continue;
            }
        }
        const usize needed = size + (alignment > kBlockAlignment ? alignment : 0);
        if (!m_blocks.empty()) {
            m_usedBeforeCurrent += m_blocks[m_current].size;
        }
        m_blocks.push_back(allocBlock(std::max(m_blockSize, needed)));
        m_current = m_blocks.size() - 1;
        m_offset = 0;
    }
}

void FrameAllocator::reset() {
    if (m_blocks.size() > 1) {
        // Coalesce: next frame fits into one block of the combined capacity.
        usize total = 0;
        for (Block& b : m_blocks) {
            total += b.size;
            freeBlock(b);
        }
        m_blocks.clear();
        m_blocks.push_back(allocBlock(total));
    }
    m_current = 0;
    m_offset = 0;
    m_usedBeforeCurrent = 0;
    m_allocationCount = 0;
}

void FrameAllocator::release() {
    for (Block& b : m_blocks) {
        freeBlock(b);
    }
    m_blocks.clear();
    m_current = 0;
    m_offset = 0;
    m_usedBeforeCurrent = 0;
    m_allocationCount = 0;
}

FrameAllocatorStats FrameAllocator::stats() const {
    FrameAllocatorStats s;
    s.usedBytes = m_blocks.empty() ? 0 : m_usedBeforeCurrent + m_offset;
    for (const Block& b : m_blocks) {
        s.capacityBytes += b.size;
    }
    s.peakUsedBytes = m_peak;
    s.blockCount = m_blocks.size();
    s.allocationCount = m_allocationCount;
    return s;
}

bool FrameAllocator::owns(const void* p) const {
    const std::less<const void*> lt;
    for (const Block& b : m_blocks) {
        if (!lt(p, b.data) && lt(p, b.data + b.size)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------

MultiFrameAllocator::MultiFrameAllocator(u32 framesInFlight, usize blockSize) {
    OX_ASSERT(framesInFlight > 0, "MultiFrameAllocator needs at least one frame");
    m_allocators.reserve(framesInFlight);
    for (u32 i = 0; i < framesInFlight; ++i) {
        m_allocators.emplace_back(blockSize);
    }
}

void MultiFrameAllocator::beginFrame(u64 frameIndex) {
    m_current = static_cast<u32>(frameIndex % m_allocators.size());
    m_allocators[m_current].reset();
}

// ---------------------------------------------------------------------------------------------

PoolAllocator::PoolAllocator(usize blockSize, usize alignment, usize blocksPerPage)
    : m_alignment(std::max(alignment, alignof(FreeNode))), m_blocksPerPage(std::max<usize>(blocksPerPage, 1)) {
    OX_ASSERT(isPowerOfTwo(alignment), "PoolAllocator: alignment {} is not a power of two", alignment);
    m_blockSize = alignUp(std::max(blockSize, sizeof(FreeNode)), m_alignment);
}

PoolAllocator::~PoolAllocator() { release(); }

PoolAllocator::PoolAllocator(PoolAllocator&& other) noexcept
    : m_pages(std::move(other.m_pages)), m_free(other.m_free), m_blockSize(other.m_blockSize),
      m_alignment(other.m_alignment), m_blocksPerPage(other.m_blocksPerPage), m_live(other.m_live) {
    other.m_pages.clear();
    other.m_free = nullptr;
    other.m_live = 0;
}

PoolAllocator& PoolAllocator::operator=(PoolAllocator&& other) noexcept {
    if (this != &other) {
        release();
        m_pages = std::move(other.m_pages);
        m_free = other.m_free;
        m_blockSize = other.m_blockSize;
        m_alignment = other.m_alignment;
        m_blocksPerPage = other.m_blocksPerPage;
        m_live = other.m_live;
        other.m_pages.clear();
        other.m_free = nullptr;
        other.m_live = 0;
    }
    return *this;
}

void PoolAllocator::addPage() {
    auto* page = static_cast<std::byte*>(::operator new(m_blockSize * m_blocksPerPage, std::align_val_t{m_alignment}));
    m_pages.push_back(page);
    // Thread the new blocks onto the free list so the lowest address is handed out first.
    for (usize i = m_blocksPerPage; i-- > 0;) {
        auto* node = reinterpret_cast<FreeNode*>(page + i * m_blockSize);
        node->next = m_free;
        m_free = node;
    }
}

void* PoolAllocator::allocate() {
    if (m_free == nullptr) {
        addPage();
    }
    FreeNode* node = m_free;
    m_free = node->next;
    ++m_live;
    return node;
}

void PoolAllocator::deallocate(void* p) {
    if (p == nullptr) {
        return;
    }
    OX_ASSERT(m_live > 0, "PoolAllocator: deallocate without a live allocation");
    auto* node = static_cast<FreeNode*>(p);
    node->next = m_free;
    m_free = node;
    --m_live;
}

void PoolAllocator::release() {
    for (std::byte* page : m_pages) {
        ::operator delete(page, std::align_val_t{m_alignment});
    }
    m_pages.clear();
    m_free = nullptr;
    m_live = 0;
}

bool PoolAllocator::owns(const void* p) const {
    const std::less<const void*> lt;
    const usize pageBytes = m_blockSize * m_blocksPerPage;
    for (const std::byte* page : m_pages) {
        if (!lt(p, page) && lt(p, page + pageBytes)) {
            return (static_cast<const std::byte*>(p) - page) % static_cast<std::ptrdiff_t>(m_blockSize) == 0;
        }
    }
    return false;
}

PoolAllocatorStats PoolAllocator::stats() const {
    return {m_live, m_pages.size() * m_blocksPerPage, m_pages.size(), m_blockSize};
}

} // namespace ox
