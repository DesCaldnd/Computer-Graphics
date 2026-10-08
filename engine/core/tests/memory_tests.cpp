#include <oxwald/core/memory.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <unordered_set>

using namespace ox;

namespace {

bool aligned(const void* p, usize a) { return reinterpret_cast<std::uintptr_t>(p) % a == 0; }

struct Vec4 {
    float x, y, z, w;
};

} // namespace

TEST(Memory, FrameAllocatorAlignmentAndBump) {
    FrameAllocator fa(4096);
    void* a = fa.allocate(3, 1);
    void* b = fa.allocate(8, 8);
    void* c = fa.allocate(16, 256);
    EXPECT_TRUE(aligned(b, 8));
    EXPECT_TRUE(aligned(c, 256));
    EXPECT_NE(a, b);
    EXPECT_TRUE(fa.owns(a));
    EXPECT_TRUE(fa.owns(c));
    int local = 0;
    EXPECT_FALSE(fa.owns(&local));
    const auto s = fa.stats();
    EXPECT_EQ(s.allocationCount, 3u);
    EXPECT_EQ(s.blockCount, 1u);
    EXPECT_GE(s.usedBytes, 3u + 8u + 16u);

    Vec4* v = fa.create<Vec4>(Vec4{1, 2, 3, 4});
    EXPECT_EQ(v->z, 3.0f);
    std::span<int> arr = fa.allocArray<int>(100);
    ASSERT_EQ(arr.size(), 100u);
    for (usize i = 0; i < arr.size(); ++i) {
        arr[i] = static_cast<int>(i);
    }
    EXPECT_EQ(arr[99], 99);
    EXPECT_TRUE(fa.allocArray<int>(0).empty());
    const int src[] = {7, 8, 9};
    auto copy = fa.copyArray<int>(std::span<const int>(src));
    EXPECT_EQ(copy[2], 9);
}

TEST(Memory, FrameAllocatorGrowsAndCoalesces) {
    FrameAllocator fa(1024);
    std::set<void*> ptrs;
    for (int i = 0; i < 50; ++i) {
        void* p = fa.allocate(100, 16);
        std::memset(p, i, 100);
        ptrs.insert(p);
    }
    EXPECT_EQ(ptrs.size(), 50u);
    EXPECT_GT(fa.stats().blockCount, 1u);
    void* big = fa.allocate(10000, 64); // larger than a block
    EXPECT_TRUE(aligned(big, 64));
    const usize capacity = fa.stats().capacityBytes;
    const usize peak = fa.stats().peakUsedBytes;
    EXPECT_GE(peak, 50u * 100u + 10000u);

    fa.reset();
    EXPECT_EQ(fa.stats().blockCount, 1u);
    EXPECT_EQ(fa.stats().capacityBytes, capacity);
    EXPECT_EQ(fa.stats().usedBytes, 0u);
    EXPECT_EQ(fa.stats().allocationCount, 0u);
    // Same workload now fits in the single coalesced block.
    for (int i = 0; i < 50; ++i) {
        (void)fa.allocate(100, 16);
    }
    (void)fa.allocate(10000, 64);
    EXPECT_EQ(fa.stats().blockCount, 1u);
    EXPECT_EQ(fa.stats().peakUsedBytes, peak);

    FrameAllocator moved = std::move(fa);
    EXPECT_EQ(moved.stats().blockCount, 1u);
    EXPECT_EQ(fa.stats().blockCount, 0u); // NOLINT: moved-from is valid and empty
    moved.release();
    EXPECT_EQ(moved.stats().capacityBytes, 0u);
}

TEST(Memory, MultiFrameAllocatorRotates) {
    FrameAllocatorRing<3> ring(1024);
    EXPECT_EQ(ring.frameCount(), 3u);
    ring.beginFrame(0);
    int* f0 = ring.create<int>(10);
    ring.beginFrame(1);
    int* f1 = ring.create<int>(11);
    ring.beginFrame(2);
    (void)ring.create<int>(12);
    EXPECT_EQ(*f0, 10); // still alive: N frames in flight
    EXPECT_EQ(*f1, 11);
    EXPECT_EQ(ring.frame(0).stats().allocationCount, 1u);
    ring.beginFrame(3); // reuses slot 0
    EXPECT_EQ(ring.currentSlot(), 0u);
    EXPECT_EQ(ring.current().stats().allocationCount, 0u);
    EXPECT_EQ(ring.frame(1).stats().allocationCount, 1u);
}

TEST(Memory, PoolAllocatorReusesBlocks) {
    PoolAllocator pool(24, 16, 4);
    std::vector<void*> ptrs;
    for (int i = 0; i < 10; ++i) {
        void* p = pool.allocate();
        EXPECT_TRUE(aligned(p, 16));
        EXPECT_TRUE(pool.owns(p));
        std::memset(p, 0xab, 24);
        ptrs.push_back(p);
    }
    EXPECT_EQ(pool.stats().liveBlocks, 10u);
    EXPECT_EQ(pool.stats().pageCount, 3u);
    EXPECT_EQ(pool.stats().capacityBlocks, 12u);
    EXPECT_EQ(std::set<void*>(ptrs.begin(), ptrs.end()).size(), 10u);

    pool.deallocate(ptrs[3]);
    EXPECT_EQ(pool.allocate(), ptrs[3]); // LIFO free list
    for (void* p : ptrs) {
        pool.deallocate(p);
    }
    EXPECT_EQ(pool.liveCount(), 0u);
    EXPECT_EQ(pool.stats().pageCount, 3u); // pages are retained
    pool.deallocate(nullptr);
}

TEST(Memory, TypedPoolRunsConstructorsAndDestructors) {
    static int alive = 0;
    struct Tracked {
        std::string name;
        explicit Tracked(std::string n) : name(std::move(n)) { ++alive; }
        ~Tracked() { --alive; }
    };
    TypedPool<Tracked> pool(2);
    std::vector<Tracked*> objs;
    for (int i = 0; i < 5; ++i) {
        objs.push_back(pool.create("obj" + std::to_string(i)));
    }
    EXPECT_EQ(alive, 5);
    EXPECT_EQ(objs[4]->name, "obj4");
    EXPECT_TRUE(pool.owns(objs[2]));
    for (Tracked* t : objs) {
        pool.destroy(t);
    }
    EXPECT_EQ(alive, 0);
    EXPECT_EQ(pool.liveCount(), 0u);
}

TEST(Memory, HandlePoolGenerations) {
    struct Tag {};
    HandlePool<std::string, Tag> pool;
    auto a = pool.create("alpha");
    auto b = pool.create("beta");
    EXPECT_TRUE(a.valid());
    EXPECT_NE(a, b);
    EXPECT_EQ(pool.size(), 2u);
    ASSERT_NE(pool.get(a), nullptr);
    EXPECT_EQ(*pool.get(a), "alpha");
    std::string* bPtr = pool.get(b);

    EXPECT_TRUE(pool.destroy(a));
    EXPECT_FALSE(pool.destroy(a)); // stale
    EXPECT_FALSE(pool.alive(a));
    EXPECT_EQ(pool.get(a), nullptr);

    auto c = pool.create("gamma"); // reuses a's slot with a new generation
    EXPECT_EQ(c.index, a.index);
    EXPECT_NE(c.generation, a.generation);
    EXPECT_FALSE(pool.alive(a));
    EXPECT_TRUE(pool.alive(c));
    EXPECT_EQ(pool.get(b), bPtr); // stable pointers

    EXPECT_FALSE(pool.alive(Handle<Tag>::invalid()));
    EXPECT_FALSE(pool.alive(Handle<Tag>{999, 1}));
    EXPECT_EQ(Handle<Tag>::fromU64(c.toU64()), c);

    std::unordered_set<Handle<Tag>> set{a, b, c};
    EXPECT_EQ(set.size(), 3u);

    usize visited = 0;
    pool.forEach([&](Handle<Tag> h, std::string& s) {
        EXPECT_TRUE(pool.alive(h));
        s += "!";
        ++visited;
    });
    EXPECT_EQ(visited, 2u);
    EXPECT_EQ(*pool.get(c), "gamma!");
    const auto& cpool = pool;
    cpool.forEach([&](Handle<Tag>, const std::string& s) { EXPECT_EQ(s.back(), '!'); });

    pool.clear();
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_FALSE(pool.alive(b));
    EXPECT_FALSE(pool.alive(c));
    auto d = pool.create("delta");
    EXPECT_TRUE(pool.alive(d));
    EXPECT_LE(pool.capacity(), 2u);
}
