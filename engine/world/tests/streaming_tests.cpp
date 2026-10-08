#include <oxwald/world/chunk_data.hpp>
#include <oxwald/world/streaming.hpp>
#include <oxwald/world/terrain_gen.hpp>

#include "crc32.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

using namespace ox;
using namespace ox::world;

namespace {

// Queues jobs; the test decides when they run.
class ManualExecutor final : public IChunkExecutor {
public:
    void submit(std::function<void()> job) override { jobs.push_back(std::move(job)); }
    void runAll() {
        auto j = std::move(jobs);
        jobs.clear();
        for (auto& f : j) {
            f();
        }
    }
    std::vector<std::function<void()>> jobs;
};

struct TestPayload : ChunkPayload {
    ChunkCoord coord;
};

struct Recorder {
    std::mutex mutex;
    std::vector<ChunkCoord> loads;
    std::vector<ChunkCoord> activated;
    std::vector<ChunkCoord> unloaded;
    std::vector<ChunkCoord> saved;
    std::set<std::pair<i32, i32>> failing;

    ChunkCallbacks callbacks(bool withSave = false) {
        ChunkCallbacks cb;
        cb.load = [this](ChunkCoord c, const std::atomic<bool>&) -> std::unique_ptr<ChunkPayload> {
            std::lock_guard lk(mutex);
            loads.push_back(c);
            if (failing.count({c.x, c.z})) {
                return nullptr;
            }
            auto p = std::make_unique<TestPayload>();
            p->coord = c;
            return p;
        };
        cb.onLoaded = [this](ChunkCoord c, ChunkPayload& p) {
            EXPECT_EQ(static_cast<TestPayload&>(p).coord, c);
            activated.push_back(c);
        };
        cb.onUnload = [this](ChunkCoord c, ChunkPayload&) { unloaded.push_back(c); };
        if (withSave) {
            cb.save = [this](ChunkCoord c, ChunkPayload&) {
                std::lock_guard lk(mutex);
                saved.push_back(c);
            };
        }
        return cb;
    }
};

ChunkStreamerSettings smallSettings() {
    ChunkStreamerSettings s;
    s.chunkSize = 10.f;
    s.loadRadius = 25.f;
    s.unloadRadius = 40.f;
    s.maxLoadRequestsPerUpdate = 3;
    s.maxInFlightLoads = 100;
    s.maxActivationsPerUpdate = 100;
    s.maxUnloadsPerUpdate = 100;
    s.viewDirectionWeight = 0.f;
    s.teleportDistance = 100.f;
    s.teleportBudgetMultiplier = 4;
    s.failedRetryUpdates = 5;
    return s;
}

f32 chunkDistance(ChunkCoord c, glm::vec3 p, f32 size) {
    const glm::vec2 lo(f32(c.x) * size, f32(c.z) * size);
    const glm::vec2 q(p.x, p.z);
    return glm::length(q - glm::clamp(q, lo, lo + glm::vec2(size)));
}

usize countInRadius(glm::vec3 p, f32 r, f32 size) {
    usize n = 0;
    const i32 cx = i32(std::floor(p.x / size)), cz = i32(std::floor(p.z / size));
    for (i32 z = cz - 20; z <= cz + 20; ++z) {
        for (i32 x = cx - 20; x <= cx + 20; ++x) {
            n += chunkDistance({x, z}, p, size) <= r;
        }
    }
    return n;
}

} // namespace

TEST(ChunkStreamer, LoadsNearestFirstWithinBudget) {
    auto exec = std::make_shared<ManualExecutor>();
    Recorder rec;
    ChunkStreamer s(smallSettings(), rec.callbacks(), exec);
    const StreamingViewer v{{5.f, 0.f, 5.f}};
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.stats().requested, 3u);
    EXPECT_EQ(exec->jobs.size(), 3u);
    EXPECT_EQ(s.state({0, 0}), ChunkState::Loading);
    exec->runAll();
    ASSERT_EQ(rec.loads.size(), 3u);
    EXPECT_EQ(rec.loads[0], (ChunkCoord{0, 0}));

    // Keep going: the order of requests is non-decreasing in distance.
    for (int i = 0; i < 20; ++i) {
        s.update(std::span(&v, 1));
        EXPECT_LE(s.stats().requested, 3u);
        exec->runAll();
    }
    s.update(std::span(&v, 1));
    const usize expected = countInRadius(v.position, 25.f, 10.f);
    EXPECT_EQ(rec.loads.size(), expected);
    for (usize i = 1; i < rec.loads.size(); ++i) {
        EXPECT_LE(chunkDistance(rec.loads[i - 1], v.position, 10.f), chunkDistance(rec.loads[i], v.position, 10.f) + 1e-4f);
    }
    EXPECT_EQ(rec.activated.size(), expected);
    EXPECT_EQ(s.stats().loaded, expected);
    EXPECT_TRUE(s.isAreaReady(v.position, 25.f));
    ASSERT_NE(s.payload({0, 0}), nullptr);
    EXPECT_EQ(s.payload({100, 100}), nullptr);
}

TEST(ChunkStreamer, InFlightAndActivationBudgets) {
    auto exec = std::make_shared<ManualExecutor>();
    Recorder rec;
    ChunkStreamerSettings st = smallSettings();
    st.maxLoadRequestsPerUpdate = 10;
    st.maxInFlightLoads = 2;
    st.maxActivationsPerUpdate = 1;
    ChunkStreamer s(st, rec.callbacks(), exec);
    const StreamingViewer v{{5.f, 0.f, 5.f}};
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.stats().requested, 2u);
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.stats().requested, 0u); // still 2 in flight
    exec->runAll();
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.stats().activated, 1u); // one activation per update
    EXPECT_EQ(s.stats().loaded, 1u);
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.stats().loaded, 2u);
    exec->runAll(); // the streamer waits for submitted jobs on destruction
}

TEST(ChunkStreamer, ViewDirectionRaisesPriority) {
    auto exec = std::make_shared<ManualExecutor>();
    Recorder rec;
    ChunkStreamerSettings st = smallSettings();
    st.viewDirectionWeight = 0.9f;
    st.maxLoadRequestsPerUpdate = 2;
    ChunkStreamer s(st, rec.callbacks(), exec);
    StreamingViewer v{{5.f, 0.f, 5.f}, {1.f, 0.f, 0.f}};
    s.update(std::span(&v, 1));
    exec->runAll();
    ASSERT_EQ(rec.loads.size(), 2u);
    EXPECT_EQ(rec.loads[0], (ChunkCoord{0, 0}));
    EXPECT_EQ(rec.loads[1], (ChunkCoord{1, 0})); // the neighbour in front beats the ones beside/behind
}

TEST(ChunkStreamer, HysteresisAndUnload) {
    Recorder rec;
    ChunkStreamer s(smallSettings(), rec.callbacks(), std::make_shared<InlineExecutor>());
    StreamingViewer v{{5.f, 0.f, 5.f}};
    s.flush(std::span(&v, 1));
    EXPECT_EQ(s.state({2, 0}), ChunkState::Loaded); // distance 15
    // Move 20 m in -X: chunk (2,0) is now 35 m away — beyond load radius but inside unload radius.
    v.position.x -= 20.f;
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.state({2, 0}), ChunkState::Loaded);
    for (const ChunkCoord c : rec.unloaded) {
        EXPECT_GT(chunkDistance(c, v.position, 10.f), 40.f); // only beyond the unload radius
    }
    const usize unloadedSoFar = rec.unloaded.size();
    // 10 m more: 45 m > unload radius → unloaded.
    v.position.x -= 10.f;
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.state({2, 0}), ChunkState::Unloaded);
    EXPECT_NE(std::find(rec.unloaded.begin(), rec.unloaded.end(), ChunkCoord{2, 0}), rec.unloaded.end());
    EXPECT_GT(rec.unloaded.size(), unloadedSoFar);
    for (const ChunkCoord c : rec.unloaded) {
        EXPECT_GT(chunkDistance(c, v.position, 10.f), 40.f);
    }
    // Moving back reloads it.
    v.position.x += 30.f;
    s.flush(std::span(&v, 1));
    EXPECT_EQ(s.state({2, 0}), ChunkState::Loaded);
    s.unloadAll();
    EXPECT_EQ(s.state({0, 0}), ChunkState::Unloaded);
}

TEST(ChunkStreamer, CancelsLoadsThatLeaveRangeAndHandlesTeleport) {
    auto exec = std::make_shared<ManualExecutor>();
    Recorder rec;
    ChunkStreamer s(smallSettings(), rec.callbacks(), exec);
    StreamingViewer v{{5.f, 0.f, 5.f}};
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.stats().loading, 3u);
    // Teleport far away before the jobs ran.
    v.position = {1005.f, 0.f, 5.f};
    s.update(std::span(&v, 1));
    EXPECT_TRUE(s.stats().teleported);
    EXPECT_EQ(s.stats().cancelled, 3u);
    EXPECT_EQ(s.state({0, 0}), ChunkState::Unloaded);
    EXPECT_EQ(s.stats().requested, 12u); // budget × teleport multiplier
    EXPECT_FALSE(s.isAreaReady(v.position, 15.f));
    exec->runAll();
    s.update(std::span(&v, 1));
    // Cancelled loads were discarded, never activated.
    for (const ChunkCoord c : rec.activated) {
        EXPECT_GE(c.x, 90);
    }
    while (!exec->jobs.empty() || s.stats().loading > 0) {
        exec->runAll();
        s.update(std::span(&v, 1));
    }
    EXPECT_TRUE(s.isAreaReady(v.position, 25.f));
}

TEST(ChunkStreamer, SaveKeepsChunkUnloadingUntilDone) {
    auto exec = std::make_shared<ManualExecutor>();
    Recorder rec;
    ChunkStreamer s(smallSettings(), rec.callbacks(true), exec);
    StreamingViewer v{{5.f, 0.f, 5.f}};
    for (int i = 0; i < 20; ++i) {
        s.update(std::span(&v, 1));
        exec->runAll();
    }
    EXPECT_EQ(s.state({0, 0}), ChunkState::Loaded);
    v.position = {505.f, 0.f, 5.f};
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.state({0, 0}), ChunkState::Unloading);
    EXPECT_GT(s.stats().unloading, 0u);
    EXPECT_EQ(s.payload({0, 0}), nullptr);
    exec->runAll();
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.state({0, 0}), ChunkState::Unloaded);
    EXPECT_NE(std::find(rec.saved.begin(), rec.saved.end(), ChunkCoord{0, 0}), rec.saved.end());
    exec->runAll();
}

TEST(ChunkStreamer, FailedLoadsRetryLater) {
    auto exec = std::make_shared<ManualExecutor>();
    Recorder rec;
    rec.failing.insert({0, 0});
    ChunkStreamerSettings st = smallSettings();
    st.maxLoadRequestsPerUpdate = 100;
    ChunkStreamer s(st, rec.callbacks(), exec);
    const StreamingViewer v{{5.f, 0.f, 5.f}};
    s.update(std::span(&v, 1));
    exec->runAll();
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.state({0, 0}), ChunkState::Unloaded);
    const auto countLoads = [&] { return std::count(rec.loads.begin(), rec.loads.end(), ChunkCoord{0, 0}); };
    EXPECT_EQ(countLoads(), 1);
    for (int i = 0; i < 3; ++i) {
        s.update(std::span(&v, 1));
        exec->runAll();
    }
    EXPECT_EQ(countLoads(), 1); // backing off
    rec.failing.clear();
    for (int i = 0; i < 6; ++i) {
        s.update(std::span(&v, 1));
        exec->runAll();
    }
    s.update(std::span(&v, 1));
    EXPECT_EQ(s.state({0, 0}), ChunkState::Loaded);
}

TEST(ChunkStreamer, ThreadPoolExecutorAndMultipleViewers) {
    Recorder rec;
    std::vector<StreamingViewer> viewers{{{5.f, 0.f, 5.f}, {0, 0, -1}, 1.f, 1}, {{305.f, 0.f, 5.f}, {0, 0, -1}, 0.5f, 2}};
    {
        ChunkStreamerSettings st = smallSettings();
        st.maxLoadRequestsPerUpdate = 8;
        ChunkStreamer s(st, rec.callbacks()); // default thread pool
        s.flush(viewers);
        EXPECT_TRUE(s.isAreaReady(viewers[0].position, 25.f));
        EXPECT_TRUE(s.isAreaReady(viewers[1].position, 12.5f)); // scaled radius
        EXPECT_EQ(s.stats().loaded, countInRadius(viewers[0].position, 25.f, 10.f) + countInRadius(viewers[1].position, 12.5f, 10.f));
        int lines = 0;
        s.debugDraw([&](glm::vec3, glm::vec3, glm::vec4) { ++lines; });
        EXPECT_EQ(lines, i32(s.stats().loaded) * 4);
        // Destroy with loads in flight: must not crash or leak callbacks into a dead streamer.
        viewers[0].position = {2005.f, 0.f, 5.f};
        s.update(viewers);
    }
    SUCCEED();
}

// --- chunk serialization -----------------------------------------------------------------------

namespace {
ChunkData makeChunk(HeightFormat fmt) {
    ChunkData c;
    c.coord = {-3, 7};
    HeightfieldDesc d;
    d.resolution = 33;
    d.worldSize = 64.f;
    d.heightScale = 120.f;
    d.heightOffset = -10.f;
    d.origin = {-192.f, 448.f};
    d.format = fmt;
    Heightfield hf(d);
    TerrainNoiseSettings ns;
    generateNoise(hf, ns);
    hf.setHole(3, 4, true);
    c.heightfield = std::move(hf);
    SplatMap splat(17, 5, d.origin, d.worldSize);
    splat.setWeights(2, 3, {0.25f, 0.25f, 0.5f});
    c.splat = std::move(splat);
    for (int i = 0; i < 10; ++i) {
        VegetationInstance v;
        v.position = {f32(i), f32(i) * 2.f, -f32(i)};
        v.rotation = glm::angleAxis(f32(i) * 0.3f, glm::vec3(0, 1, 0));
        v.scale = 1.f + f32(i) * 0.01f;
        v.tint = 0x11223344u + u32(i);
        v.random = f32(i) / 10.f;
        v.layer = u16(i % 3);
        v.prototype = u16(i);
        c.vegetation.push_back(v);
    }
    c.userData = {1, 2, 3, 4, 5};
    return c;
}

void expectEqual(const ChunkData& a, const ChunkData& b) {
    EXPECT_EQ(a.coord, b.coord);
    ASSERT_TRUE(b.heightfield);
    EXPECT_EQ(a.heightfield->desc().resolution, b.heightfield->desc().resolution);
    EXPECT_EQ(a.heightfield->desc().format, b.heightfield->desc().format);
    EXPECT_EQ(a.heightfield->desc().origin, b.heightfield->desc().origin);
    EXPECT_EQ(a.heightfield->desc().heightOffset, b.heightfield->desc().heightOffset);
    EXPECT_TRUE(std::equal(a.heightfield->rawBytes().begin(), a.heightfield->rawBytes().end(), b.heightfield->rawBytes().begin(),
                           b.heightfield->rawBytes().end()));
    EXPECT_TRUE(b.heightfield->isHole(3, 4));
    EXPECT_FALSE(b.heightfield->isHole(4, 4));
    ASSERT_TRUE(b.splat);
    EXPECT_EQ(b.splat->layerCount(), 5u);
    EXPECT_TRUE(std::equal(a.splat->raw().begin(), a.splat->raw().end(), b.splat->raw().begin(), b.splat->raw().end()));
    EXPECT_EQ(a.vegetation, b.vegetation);
    EXPECT_EQ(a.userData, b.userData);
}
} // namespace

TEST(ChunkData, SerializationRoundTrip) {
    for (const HeightFormat fmt : {HeightFormat::Float32, HeightFormat::UNorm16}) {
        const ChunkData c = makeChunk(fmt);
        const std::vector<u8> bytes = serializeChunk(c);
        ChunkData back;
        std::string err;
        ASSERT_TRUE(deserializeChunk(bytes, back, &err)) << err;
        expectEqual(c, back);
    }
    // Empty chunk.
    ChunkData empty;
    empty.coord = {1, 2};
    ChunkData back;
    ASSERT_TRUE(deserializeChunk(serializeChunk(empty), back));
    EXPECT_EQ(back.coord, (ChunkCoord{1, 2}));
    EXPECT_FALSE(back.heightfield);
    EXPECT_TRUE(back.vegetation.empty());
}

TEST(ChunkData, DetectsCorruptionAndSkipsUnknownSections) {
    const ChunkData c = makeChunk(HeightFormat::UNorm16);
    std::vector<u8> bytes = serializeChunk(c);
    ChunkData out;
    std::string err;
    std::vector<u8> corrupt = bytes;
    corrupt[corrupt.size() / 2] ^= 0x5A;
    EXPECT_FALSE(deserializeChunk(corrupt, out, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(deserializeChunk(std::span(bytes).first(bytes.size() - 7), out));
    std::vector<u8> badMagic = bytes;
    badMagic[0] = 'X';
    EXPECT_FALSE(deserializeChunk(badMagic, out));

    // Append a section from a "future" version: it is skipped.
    std::vector<u8> future = bytes;
    u32 count;
    std::memcpy(&count, future.data() + 16, 4);
    ++count;
    std::memcpy(future.data() + 16, &count, 4);
    const u8 payload[] = {9, 8, 7};
    const u32 tag = 0x5A5A5A5A, size = 3, crc = detail::crc32(payload, 3);
    for (const u32 v : {tag, size}) {
        future.insert(future.end(), reinterpret_cast<const u8*>(&v), reinterpret_cast<const u8*>(&v) + 4);
    }
    future.insert(future.end(), payload, payload + 3);
    future.insert(future.end(), reinterpret_cast<const u8*>(&crc), reinterpret_cast<const u8*>(&crc) + 4);
    ASSERT_TRUE(deserializeChunk(future, out, &err)) << err;
    expectEqual(c, out);
}

TEST(ChunkData, StreamerCanLoadSerializedChunks) {
    std::map<std::pair<i32, i32>, std::vector<u8>> disk;
    for (i32 z = -2; z <= 2; ++z) {
        for (i32 x = -2; x <= 2; ++x) {
            ChunkData c = makeChunk(HeightFormat::UNorm16);
            c.coord = {x, z};
            disk[{x, z}] = serializeChunk(c);
        }
    }
    ChunkCallbacks cb;
    cb.load = [&](ChunkCoord c, const std::atomic<bool>&) -> std::unique_ptr<ChunkPayload> {
        auto it = disk.find({c.x, c.z});
        if (it == disk.end()) {
            return std::make_unique<ChunkData>(); // ocean
        }
        auto d = std::make_unique<ChunkData>();
        return deserializeChunk(it->second, *d) ? std::move(d) : nullptr;
    };
    int withTerrain = 0;
    cb.onLoaded = [&](ChunkCoord c, ChunkPayload& p) {
        auto& d = static_cast<ChunkData&>(p);
        if (d.heightfield) {
            EXPECT_EQ(d.coord, c);
            ++withTerrain;
        }
    };
    ChunkStreamerSettings st;
    st.chunkSize = 64.f;
    st.loadRadius = 100.f;
    st.unloadRadius = 150.f;
    ChunkStreamer s(st, cb);
    const StreamingViewer v{{32.f, 0.f, 32.f}};
    s.flush(std::span(&v, 1));
    EXPECT_TRUE(s.isAreaReady(v.position, 100.f));
    EXPECT_GT(withTerrain, 9);
}

TEST(ChunkStreamer, DestructionUnloadsLoadedChunks) {
    auto exec = std::make_shared<ManualExecutor>();
    Recorder rec;
    usize loaded = 0;
    {
        ChunkStreamer s(smallSettings(), rec.callbacks(true), exec);
        StreamingViewer v{{5.f, 0.f, 5.f}};
        for (int i = 0; i < 20; ++i) {
            s.update(std::span(&v, 1));
            exec->runAll();
        }
        loaded = rec.activated.size();
        ASSERT_GT(loaded, 0u);
        EXPECT_TRUE(rec.unloaded.empty());
        // Teleport: old chunks unload, new ones load; loads finished but not yet activated stay Loading and must
        // not be reported as unloaded by the destructor.
        v.position = {505.f, 0.f, 5.f};
        s.update(std::span(&v, 1));
        exec->runAll();
        s.update(std::span(&v, 1));
        exec->runAll(); // the manual executor must not hold jobs when the streamer waits for them
        rec.unloaded.clear();
        rec.saved.clear();
        loaded = 0;
        s.forEachChunk([&](ChunkCoord, ChunkState st) { loaded += st == ChunkState::Loaded; });
        ASSERT_GT(loaded, 0u);
    }
    EXPECT_EQ(rec.unloaded.size(), loaded);
    EXPECT_EQ(rec.saved.size(), loaded);
}
