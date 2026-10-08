// Ray tracing area, CPU tests (no GPU / RT hardware needed): TLAS instance table (masks, SBT offsets, dirty
// tracking), BLAS build / compaction / refit scheduling against an asynchronous mock backend, LOD policy, path
// tracer accumulation resets, DDGI layout math, cvars + scalability tables, availability reasons, exclusive-group
// gating against the raster variants, and SPIR-V compilation of every ray tracing shader (incl. define variants).
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/raytracing/ddgi.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/render/features/raytracing/rt_scene.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/rhi/shader_compiler.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

using namespace ox;
using namespace ox::render;
using namespace ox::render::rt;

namespace {

TlasSource source(u32 instance, u32 blend = 0, bool shadows = true, u64 blas = 1, glm::vec3 pos = glm::vec3(0.0f)) {
    TlasSource s;
    s.gpuInstance = instance;
    s.meshInfo = instance * 2;
    s.material = 7;
    s.blend = blend;
    s.castShadows = shadows;
    s.world = glm::translate(glm::mat4(1.0f), pos);
    s.blasKey = 100 + instance;
    s.blas = blas;
    return s;
}

} // namespace

// --- TLAS instance table ---

TEST(RtTlas, MasksAndShaderBindingTableOffsets) {
    EXPECT_EQ(instanceMask(0, true), kMaskOpaque | kMaskShadowOpaque);
    EXPECT_EQ(instanceMask(1, true), kMaskAlphaTested | kMaskShadowOpaque);
    EXPECT_EQ(instanceMask(2, true), kMaskTranslucent | kMaskShadowTranslucent);
    EXPECT_EQ(instanceMask(3, false), kMaskTranslucent);
    EXPECT_EQ(instanceMask(0, false), kMaskOpaque);
    EXPECT_EQ(sbtRecordOffset(RtHitGroup::Opaque), 0u);
    EXPECT_EQ(sbtRecordOffset(RtHitGroup::AlphaTested), 2u);
    EXPECT_EQ(sbtRecordOffset(RtHitGroup::Translucent), 4u);
    EXPECT_EQ(hitGroupForBlend(3), RtHitGroup::Translucent);

    TlasInstanceTable t;
    const std::vector<TlasSource> src = {source(0, 0), source(1, 1, false), source(2, 3)};
    t.update(src);
    ASSERT_EQ(t.entries().size(), 3u);
    const auto& e = t.entries();
    EXPECT_EQ(e[0].mask, kMaskOpaque | kMaskShadowOpaque);
    EXPECT_EQ(e[1].mask, u32(kMaskAlphaTested));
    EXPECT_EQ(e[2].sbtOffset, 4u);
    EXPECT_TRUE(e[0].flags & VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
    EXPECT_TRUE(e[1].flags & VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR) << "alpha test needs candidate hits";
    EXPECT_TRUE(e[2].flags & VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
    for (const TlasEntry& x : e) {
        EXPECT_TRUE(x.flags & VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
        EXPECT_TRUE(x.flags & VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR);
    }
    // GPU lookup table mirrors the entries (custom index = table index).
    ASSERT_EQ(t.gpuInstances().size(), 3u);
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_EQ(e[i].customIndex, i);
        EXPECT_EQ(t.gpuInstances()[i].gpuInstance, e[i].gpuInstance);
        EXPECT_EQ(t.gpuInstances()[i].mask, e[i].mask);
    }
    EXPECT_EQ(t.gpuInstances()[2].flags >> 8, 3u) << "blend mode in bits 8-15";
}

TEST(RtTlas, TransformIsRowMajor3x4) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3)) * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    const glm::mat3x4 t = toTlasTransform(m);
    // Row r = (m[0][r], m[1][r], m[2][r], m[3][r]): translation in the last column.
    EXPECT_FLOAT_EQ(t[0][3], 1.0f);
    EXPECT_FLOAT_EQ(t[1][3], 2.0f);
    EXPECT_FLOAT_EQ(t[2][3], 3.0f);
    EXPECT_FLOAT_EQ(t[0][0], 2.0f);
    EXPECT_FLOAT_EQ(t[1][1], 2.0f);
    // Rotation: row-major means t[r][c] = m[c][r].
    const glm::mat4 r = glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0, 1, 0));
    const glm::mat3x4 tr = toTlasTransform(r);
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c) EXPECT_FLOAT_EQ(tr[row][c], r[c][row]);
}

TEST(RtTlas, DirtyTrackingUpdateVersusRebuild) {
    TlasInstanceTable t;
    std::vector<TlasSource> src = {source(5), source(2), source(9)};
    TlasInstanceTable::Result r = t.update(src);
    EXPECT_TRUE(r.rebuild) << "first build";
    EXPECT_EQ(r.added, 3u);
    // Stable order: sorted by GpuInstance slot regardless of input order.
    EXPECT_EQ(t.entries()[0].gpuInstance, 2u);
    EXPECT_EQ(t.entries()[2].gpuInstance, 9u);
    EXPECT_EQ(*t.entryOf(9), 2u);

    r = t.update(src);
    EXPECT_FALSE(r.changed) << "identical frame: nothing to build";
    EXPECT_FALSE(r.rebuild);

    src[0].world = glm::translate(glm::mat4(1.0f), glm::vec3(0, 1, 0));
    r = t.update(src);
    EXPECT_TRUE(r.changed);
    EXPECT_FALSE(r.rebuild) << "moved instance only → TLAS update";
    EXPECT_EQ(r.moved, 1u);
    EXPECT_EQ(t.updatesSinceRebuild(), 1u);

    src[1].castShadows = false; // mask change: update is enough
    r = t.update(src);
    EXPECT_FALSE(r.rebuild);
    EXPECT_EQ(r.retagged, 1u);

    src[1].blend = 1; // opaque → alpha tested: instance flags change → rebuild
    r = t.update(src);
    EXPECT_TRUE(r.rebuild);

    src.push_back(source(7));
    r = t.update(src);
    EXPECT_TRUE(r.rebuild);
    EXPECT_EQ(r.added, 1u);
    EXPECT_EQ(*t.entryOf(7), 2u) << "custom indices follow the sorted order";

    src.erase(src.begin()); // remove instance 5
    r = t.update(src);
    EXPECT_TRUE(r.rebuild);
    EXPECT_EQ(r.removed, 1u);
    EXPECT_FALSE(t.entryOf(5).has_value());

    src[0].blas = 42; // BLAS compacted / rebuilt → new handle
    r = t.update(src);
    EXPECT_TRUE(r.rebuild);
    EXPECT_EQ(r.blasSwapped, 1u);

    src[1].blas = 0; // BLAS not built yet (streaming budget): skipped
    r = t.update(src);
    EXPECT_EQ(r.skipped, 1u);
    EXPECT_EQ(t.entries().size(), src.size() - 1);
}

TEST(RtTlas, PeriodicRebuildAfterManyUpdates) {
    TlasInstanceTable t;
    t.maxUpdatesBeforeRebuild = 4;
    std::vector<TlasSource> src = {source(0), source(1)};
    t.update(src);
    for (int frame = 1; frame <= 4; ++frame) {
        src[0].world = glm::translate(glm::mat4(1.0f), glm::vec3(f32(frame), 0, 0));
        EXPECT_FALSE(t.update(src).rebuild) << frame;
    }
    src[0].world = glm::translate(glm::mat4(1.0f), glm::vec3(9, 0, 0));
    EXPECT_TRUE(t.update(src).rebuild) << "refits degrade the BVH: rebuild after maxUpdatesBeforeRebuild";
    t.forceRebuild();
    EXPECT_TRUE(t.update(src).rebuild);
}

// --- BLAS scheduling with an asynchronous mock backend ---

namespace {

class MockBlasBackend final : public IBlasBackend {
public:
    struct Rec {
        u64 completeFrame = 0;
        u64 queryFrame = 0;
        u64 bytes = 0;
        u64 compacted = 0;
        bool compaction = false;
        bool deformable = false;
        u32 refits = 0;
    };
    u64 frame = 0;
    u32 buildLatency = 2, queryLatency = 1, compactLatency = 1;
    bool internalCompaction = false;
    std::map<Handle, Rec> live;
    std::vector<Handle> destroyed;
    std::vector<u64> buildOrder; // request keys
    u32 builds = 0, compactions = 0, refits = 0;
    Handle next = 1;

    Handle build(const BlasRequest& r, bool allowCompaction) override {
        const Handle h = next++;
        Rec rec;
        rec.completeFrame = frame + buildLatency;
        rec.queryFrame = rec.completeFrame + queryLatency;
        rec.bytes = u64(r.indexCount / 3) * 64;
        rec.compacted = rec.bytes / 2;
        rec.compaction = allowCompaction;
        rec.deformable = r.deformable;
        live[h] = rec;
        buildOrder.push_back(r.key);
        ++builds;
        return h;
    }
    bool isComplete(Handle h) override { return live.count(h) && frame >= live[h].completeFrame; }
    std::optional<u64> compactedSize(Handle h) override {
        if (!live.count(h) || !live[h].compaction || frame < live[h].queryFrame) return std::nullopt;
        return live[h].compacted;
    }
    Handle compact(Handle h, u64 size) override {
        const Handle c = next++;
        Rec rec;
        rec.completeFrame = frame + compactLatency;
        rec.bytes = size;
        live[c] = rec;
        ++compactions;
        return c;
    }
    void refit(Handle h, const BlasRequest&) override {
        EXPECT_TRUE(live.count(h)) << "refit of a destroyed BLAS";
        EXPECT_TRUE(live[h].deformable) << "refit needs an update-capable BLAS";
        ++live[h].refits;
        ++refits;
    }
    void destroy(Handle h) override {
        EXPECT_TRUE(live.count(h)) << "double destroy";
        live.erase(h);
        destroyed.push_back(h);
    }
    u64 memorySize(Handle h) override { return live.count(h) ? live[h].bytes : 0; }
    bool compactsInternally() const override { return internalCompaction; }
};

BlasRequest request(u64 key, u32 triangles, f32 priority = 0.0f, bool deformable = false) {
    BlasRequest r;
    r.key = key;
    r.meshInfo = u32(key);
    r.indexCount = triangles * 3;
    r.vertexCount = triangles * 3;
    r.priority = priority;
    r.deformable = deformable;
    return r;
}

struct SchedulerRig {
    MockBlasBackend backend;
    BlasScheduler scheduler{backend};
    void frame(u64 f, const std::vector<BlasRequest>& requests, const std::vector<u64>& deformed = {}) {
        backend.frame = f;
        scheduler.beginFrame(f);
        for (const BlasRequest& r : requests) scheduler.request(r);
        for (u64 k : deformed) scheduler.markDeformed(k);
        scheduler.update();
    }
};

} // namespace

TEST(RtBlas, BuildBudgetAndPriorityOrder) {
    SchedulerRig rig;
    rig.scheduler.config().maxBuildsPerFrame = 2;
    std::vector<BlasRequest> reqs = {request(1, 100, 0.1f), request(2, 100, 0.9f), request(3, 100, 0.5f),
                                     request(4, 100, 0.2f), request(5, 100, 0.0f)};
    rig.frame(0, reqs);
    EXPECT_EQ(rig.backend.builds, 2u) << "per-frame build budget";
    EXPECT_EQ(rig.backend.buildOrder, (std::vector<u64>{2, 3})) << "highest priority (screen coverage) first";
    EXPECT_EQ(rig.scheduler.stats().queued, 3u);
    EXPECT_EQ(rig.scheduler.ready(2), 0u) << "not usable before the build completes";
    rig.frame(1, reqs);
    rig.frame(2, reqs);
    EXPECT_NE(rig.scheduler.ready(2), 0u) << "usable once complete";
    EXPECT_TRUE(rig.scheduler.handlesChanged()) << "TLAS must rebuild when a BLAS becomes available";
    for (u64 f = 3; f < 10; ++f) rig.frame(f, reqs);
    for (u64 k = 1; k <= 5; ++k) EXPECT_NE(rig.scheduler.ready(k), 0u) << k;
}

TEST(RtBlas, TriangleBudgetButFirstBuildAlwaysRuns) {
    SchedulerRig rig;
    rig.scheduler.config().maxTrianglesPerFrame = 1000;
    rig.frame(0, {request(1, 5000, 1.0f), request(2, 10, 0.5f)});
    EXPECT_EQ(rig.backend.builds, 1u) << "a mesh above the budget still builds (alone)";
    rig.frame(1, {request(1, 5000, 1.0f), request(2, 10, 0.5f)});
    EXPECT_EQ(rig.backend.builds, 2u);
}

TEST(RtBlas, CompactionLifecycleForStaticMeshes) {
    SchedulerRig rig;
    const std::vector<BlasRequest> reqs = {request(1, 1000)};
    rig.frame(0, reqs);
    EXPECT_EQ(rig.scheduler.state(1), BlasScheduler::State::Building);
    rig.frame(1, reqs);
    rig.frame(2, reqs); // build complete → compaction queued (size query not ready yet)
    const IBlasBackend::Handle original = rig.scheduler.ready(1);
    ASSERT_NE(original, 0u);
    EXPECT_EQ(rig.scheduler.state(1), BlasScheduler::State::CompactionQueued);
    EXPECT_EQ(rig.backend.compactions, 0u);
    rig.frame(3, reqs); // query result available → compaction copy starts
    EXPECT_EQ(rig.scheduler.state(1), BlasScheduler::State::Compacting);
    EXPECT_EQ(rig.backend.compactions, 1u);
    EXPECT_EQ(rig.scheduler.ready(1), original) << "the original keeps being traced while the copy runs";
    EXPECT_TRUE(rig.backend.destroyed.empty());
    const u64 before = rig.scheduler.stats().memoryBytes;
    rig.frame(4, reqs); // copy complete → swap, old one destroyed (deferred by the backend)
    EXPECT_EQ(rig.scheduler.state(1), BlasScheduler::State::Compacted);
    EXPECT_NE(rig.scheduler.ready(1), original);
    EXPECT_EQ(rig.backend.destroyed, std::vector<IBlasBackend::Handle>{original});
    EXPECT_TRUE(rig.scheduler.handlesChanged());
    EXPECT_LT(rig.scheduler.stats().memoryBytes, before);
    EXPECT_EQ(rig.scheduler.stats().savedByCompaction, 1000u * 64 / 2);
}

TEST(RtBlas, CompactionBudgetPerFrame) {
    SchedulerRig rig;
    rig.scheduler.config().maxCompactionsPerFrame = 1;
    std::vector<BlasRequest> reqs = {request(1, 10), request(2, 10), request(3, 10)};
    for (u64 f = 0; f <= 3; ++f) rig.frame(f, reqs);
    EXPECT_EQ(rig.backend.compactions, 1u);
    rig.frame(4, reqs);
    EXPECT_EQ(rig.backend.compactions, 2u);
}

TEST(RtBlas, InternalCompactionBackendSkipsTheStateMachine) {
    SchedulerRig rig;
    rig.backend.internalCompaction = true; // rhi::Device::createBlas compacts synchronously
    rig.backend.buildLatency = 0;
    rig.frame(0, {request(1, 10)});
    EXPECT_NE(rig.scheduler.ready(1), 0u) << "synchronous backends are usable the same frame";
    EXPECT_EQ(rig.scheduler.state(1), BlasScheduler::State::Built);
    rig.frame(1, {request(1, 10)});
    EXPECT_EQ(rig.backend.compactions, 0u);
}

TEST(RtBlas, DeformablesRefitAndPeriodicallyRebuild) {
    SchedulerRig rig;
    rig.scheduler.config().maxRefitsBeforeRebuild = 3;
    const std::vector<BlasRequest> reqs = {request(7, 500, 1.0f, true)};
    for (u64 f = 0; f <= 2; ++f) rig.frame(f, reqs, {7});
    const IBlasBackend::Handle h = rig.scheduler.ready(7);
    ASSERT_NE(h, 0u);
    EXPECT_EQ(rig.scheduler.state(7), BlasScheduler::State::Built) << "skinned BLASes are never compacted";
    EXPECT_FALSE(rig.backend.live[h].compaction);
    EXPECT_EQ(rig.backend.refits, 1u) << "deformed in the frame the build completed: refit to the current pose";
    rig.frame(3, reqs); // not deformed this frame → no refit
    EXPECT_EQ(rig.backend.refits, 1u);
    for (u64 f = 4; f <= 5; ++f) rig.frame(f, reqs, {7});
    EXPECT_EQ(rig.backend.refits, 3u);
    rig.frame(6, reqs, {7}); // 4th deformation → full rebuild, old BLAS stays usable meanwhile
    EXPECT_EQ(rig.scheduler.stats().rebuildsThisFrame, 1u);
    EXPECT_EQ(rig.backend.refits, 3u);
    EXPECT_EQ(rig.scheduler.ready(7), h);
    rig.frame(7, reqs);
    rig.frame(8, reqs);
    EXPECT_NE(rig.scheduler.ready(7), h) << "rebuilt BLAS swapped in";
    EXPECT_FALSE(rig.backend.live.count(h)) << "old BLAS destroyed after the swap";
}

TEST(RtBlas, EvictsUnusedAndClearDestroysEverything) {
    SchedulerRig rig;
    rig.scheduler.config().evictAfterFrames = 5;
    rig.frame(0, {request(1, 10), request(2, 10)});
    for (u64 f = 1; f <= 4; ++f) rig.frame(f, {request(1, 10), request(2, 10)});
    ASSERT_NE(rig.scheduler.ready(2), 0u);
    for (u64 f = 5; f <= 10; ++f) rig.frame(f, {request(1, 10)});
    EXPECT_FALSE(rig.scheduler.state(2).has_value()) << "mesh 2 not requested for 6 frames";
    EXPECT_TRUE(rig.scheduler.state(1).has_value());
    rig.scheduler.clear();
    EXPECT_TRUE(rig.backend.live.empty()) << "clear() releases every BLAS (r.RayTracing off)";
}

TEST(RtBlas, KeysAndLodPolicy) {
    EXPECT_EQ(blasKey(1, 0, 36, 0, 24, false), blasKey(1, 0, 36, 0, 24, false));
    EXPECT_NE(blasKey(1, 0, 36, 0, 24, false), blasKey(1, 0, 36, 0, 24, true));
    EXPECT_NE(blasKey(1, 0, 36, 0, 24, false), blasKey(1, 36, 18, 0, 24, false)) << "different LOD ranges";
    EXPECT_NE(blasKey(1, 0, 36, 0, 24, false), 0u);
    EXPECT_EQ(selectBlasLod(4, -1), 3u) << "coarsest by default";
    EXPECT_EQ(selectBlasLod(4, 1), 1u);
    EXPECT_EQ(selectBlasLod(2, 5), 1u) << "clamped";
    EXPECT_EQ(selectBlasLod(1, -1), 0u);
    EXPECT_EQ(selectBlasLod(0, -1), 0u);
}

// --- path tracer accumulation ---

TEST(RtPathTracer, AccumulationResetsOnAnyChange) {
    AccumulationTracker t;
    EXPECT_TRUE(t.update(1, 2, 3)) << "first frame starts a new accumulation";
    t.addSamples(1);
    EXPECT_FALSE(t.update(1, 2, 3));
    t.addSamples(1);
    EXPECT_EQ(t.sampleCount(), 2u);
    EXPECT_TRUE(t.update(9, 2, 3)) << "camera moved";
    EXPECT_EQ(t.sampleCount(), 0u);
    t.addSamples(4);
    EXPECT_TRUE(t.update(9, 5, 3)) << "scene changed";
    EXPECT_TRUE(t.update(9, 5, 6)) << "settings changed";
    EXPECT_FALSE(t.update(9, 5, 6));
    t.reset();
    EXPECT_TRUE(t.update(9, 5, 6)) << "explicit reset (resize)";
    const glm::mat4 a = glm::perspective(1.0f, 1.0f, 0.1f, 10.0f);
    glm::mat4 b = a;
    b[3][0] += 1e-4f;
    EXPECT_NE(hashMatrix(a), hashMatrix(b));
    EXPECT_EQ(hashMatrix(a), hashMatrix(glm::mat4(a)));
}

// --- DDGI layout ---

TEST(RtDdgi, StorageAddressingIsABijectionAndScrollStable) {
    const glm::ivec3 counts{6, 3, 5};
    const f32 spacing = 2.0f;
    const glm::ivec3 minA = ddgiMinCoord(counts, spacing, {0.3f, 1.0f, -0.4f});
    std::set<u32> used;
    for (int z = 0; z < counts.z; ++z)
        for (int y = 0; y < counts.y; ++y)
            for (int x = 0; x < counts.x; ++x) {
                const glm::ivec3 c = minA + glm::ivec3(x, y, z);
                const u32 s = ddgiStorageIndex(c, counts);
                EXPECT_LT(s, ddgiProbeCount(counts));
                EXPECT_TRUE(used.insert(s).second) << "two probes share a slot";
                EXPECT_EQ(ddgiWorldCoord(s, minA, counts), c) << "slot → world coordinate inverse";
            }
    EXPECT_EQ(used.size(), usize(ddgiProbeCount(counts)));
    // Moving the camera by one probe keeps the slots of probes that stay inside the window.
    const glm::ivec3 minB = ddgiMinCoord(counts, spacing, {0.3f + spacing, 1.0f, -0.4f});
    EXPECT_EQ(minB, minA + glm::ivec3(1, 0, 0));
    u32 kept = 0;
    for (u32 s = 0; s < ddgiProbeCount(counts); ++s) {
        const glm::ivec3 a = ddgiWorldCoord(s, minA, counts), b = ddgiWorldCoord(s, minB, counts);
        if (a == b) ++kept;
        else EXPECT_EQ(b.x, a.x + counts.x) << "only the slice that left the window is reused";
    }
    EXPECT_EQ(kept, u32((counts.x - 1) * counts.y * counts.z));
    // The camera sits in the middle cell.
    const glm::vec3 lo = ddgiProbePosition(minA, spacing), hi = ddgiProbePosition(minA + counts - 1, spacing);
    EXPECT_LT(lo.x, 0.3f);
    EXPECT_GT(hi.x, 0.3f);
}

TEST(RtDdgi, AtlasTilesDoNotOverlap) {
    const glm::ivec3 counts{4, 2, 3};
    for (u32 texels : {kDdgiIrradianceTexels, kDdgiDepthTexels}) {
        const glm::uvec2 size = ddgiAtlasSize(counts, texels);
        EXPECT_EQ(size, glm::uvec2(8 * (texels + 2), 3 * (texels + 2)));
        std::set<std::pair<u32, u32>> origins;
        for (u32 s = 0; s < ddgiProbeCount(counts); ++s) {
            const glm::uvec2 o = ddgiTileInterior(s, counts, texels);
            EXPECT_GE(o.x, 1u);
            EXPECT_GE(o.y, 1u);
            EXPECT_LE(o.x + texels + 1, size.x) << "border inside the atlas";
            EXPECT_LE(o.y + texels + 1, size.y);
            EXPECT_EQ((o.x - 1) % (texels + 2), 0u);
            EXPECT_TRUE(origins.insert({o.x, o.y}).second);
        }
    }
}

TEST(RtDdgi, SphericalFibonacciDirectionsCoverTheSphere) {
    const u32 n = 256;
    glm::vec3 mean(0.0f);
    u32 up = 0;
    for (u32 i = 0; i < n; ++i) {
        const glm::vec3 d = ddgiRayDirection(i, n);
        EXPECT_NEAR(glm::length(d), 1.0f, 1e-4f);
        mean += d;
        if (d.z > 0.0f) ++up;
    }
    mean /= f32(n);
    EXPECT_LT(glm::length(mean), 0.02f) << "uniform: no directional bias";
    EXPECT_EQ(up, n / 2);
}

// --- settings, scalability, availability ---

TEST(RtSettings, ScalabilityTablesAndSnapshot) {
    registerRayTracingCVars();
    const QualityLevel before = scalability::currentLevel(Scalability::RayTracing);
    scalability::setGroup(Scalability::RayTracing, QualityLevel::Low);
    RtSettings lo = RtSettings::fromCVars();
    EXPECT_TRUE(lo.shadows);
    EXPECT_FALSE(lo.reflections);
    EXPECT_FALSE(lo.gi);
    EXPECT_EQ(lo.shadowResolutionScale, 50);
    EXPECT_FLOAT_EQ(lo.reflectionMaxRoughness, 0.3f);
    EXPECT_EQ(lo.giRaysPerProbe, 64);
    scalability::setGroup(Scalability::RayTracing, QualityLevel::Ultra);
    RtSettings ul = RtSettings::fromCVars();
    EXPECT_TRUE(ul.gi && ul.reflections && ul.ao && ul.translucency && ul.volumetrics);
    EXPECT_TRUE(ul.shadowReSTIR);
    EXPECT_EQ(ul.shadowSpp, 2);
    EXPECT_EQ(ul.aoSpp, 4);
    EXPECT_EQ(ul.giProbesXZ, 32);
    EXPECT_EQ(ul.translucencyMaxBounces, 6);
    EXPECT_EQ(ul.blasLod, 0);
    EXPECT_EQ(scalability::currentLevel(Scalability::RayTracing), QualityLevel::Ultra);
    EXPECT_FALSE(scalability::cvars(Scalability::RayTracing).empty());
    // Every cvar of the area is listed for the settings UI.
    const std::vector<std::string> names = rayTracingCVarNames();
    for (const char* n : {"r.RayTracing.Shadows", "r.RayTracing.Reflections", "r.RayTracing.AO", "r.RayTracing.GI",
                          "r.RayTracing.Translucency", "r.RayTracing.Volumetrics", "r.PathTracing"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), n), names.end()) << n;
    }
    scalability::setGroup(Scalability::RayTracing, before == QualityLevel::Custom ? QualityLevel::High : before);
}

TEST(RtSettings, AvailabilityReasonsAreHumanReadable) {
    rhi::DeviceCaps mac;
    mac.gpuName = "Apple M4 Pro";
    mac.vendor = rhi::GpuVendor::Apple;
    mac.portabilitySubset = true;
    RtStatus s = rayTracingStatus(mac);
    EXPECT_FALSE(s.available);
    EXPECT_NE(s.reason.find("MoltenVK"), std::string::npos) << s.reason;
    EXPECT_NE(s.reason.find("VK_KHR_acceleration_structure"), std::string::npos) << s.reason;
    ASSERT_GE(s.effects.size(), 8u);
    for (const RtEffectStatus& e : s.effects) {
        EXPECT_FALSE(e.available) << e.name;
        EXPECT_FALSE(e.reason.empty()) << e.name;
        EXPECT_TRUE(e.cvar.starts_with("r.")) << e.name;
    }

    rhi::DeviceCaps rtx;
    rtx.gpuName = "NVIDIA GeForce RTX 4070";
    rtx.accelerationStructure = rtx.rayQuery = true;
    s = rayTracingStatus(rtx);
    EXPECT_TRUE(s.available);
    EXPECT_TRUE(s.reason.empty());
    const RtEffectStatus& pipe = s.effects.back();
    EXPECT_FALSE(pipe.available) << "no VK_KHR_ray_tracing_pipeline";
    EXPECT_NE(pipe.reason.find("ray query mode still works"), std::string::npos);
    rtx.rayTracingPipeline = true;
    EXPECT_TRUE(rayTracingStatus(rtx).effects.back().available);

    RenderSettings rs;
    rs.rayTracing = true;
    EXPECT_FALSE(rayTracingActive(rs, mac)) << "r.RayTracing has no effect without hardware support";
    EXPECT_TRUE(rayTracingActive(rs, rtx));
    rs.rayTracing = false;
    EXPECT_FALSE(rayTracingActive(rs, rtx));
}

// --- exclusive-group gating against the raster variants ---

namespace {

struct RasterStub final : IRenderFeature {
    std::string n, g;
    InjectionPoint p;
    RasterStub(std::string name, std::string group, InjectionPoint point) : n(std::move(name)), g(std::move(group)), p(point) {}
    std::string_view name() const override { return n; }
    InjectionMask injectionPoints() const override { return maskOf(p); }
    std::string_view exclusiveGroup() const override { return g; }
    void setup(FeatureContext&) override {}
};

std::set<std::string> names(const std::vector<IRenderFeature*>& fs) {
    std::set<std::string> out;
    for (IRenderFeature* f : fs) out.insert(std::string(f->name()));
    return out;
}

class CVarOverride {
public:
    CVarOverride(std::string name, std::string value) : m_name(std::move(name)) {
        ICVar* c = CVarRegistry::instance().find(m_name);
        EXPECT_NE(c, nullptr) << m_name;
        if (c) {
            m_previous = c->toString();
            CVarRegistry::instance().set(m_name, value);
        }
    }
    ~CVarOverride() {
        if (!m_previous.empty()) CVarRegistry::instance().set(m_name, m_previous);
    }

private:
    std::string m_name, m_previous;
};

} // namespace

TEST(RtGating, RayTracedVariantsReplaceRasterOnlyWithSupport) {
    FeatureRegistry reg;
    reg.emplace<RasterStub>("ShadowsRasterStub", std::string(kGroupShadows), InjectionPoint::Shadows);
    reg.emplace<RasterStub>("AOStub", std::string(kGroupAO), InjectionPoint::Lighting);
    reg.emplace<RasterStub>("ReflectionsStub", std::string(kGroupReflections), InjectionPoint::Lighting);
    reg.emplace<RasterStub>("IrradianceStub", std::string(kGroupIndirectDiffuse), InjectionPoint::Lighting);
    registerRayTracingFeatures(reg);
    CVarOverride all("r.RayTracing.GI", "true"), refl("r.RayTracing.Reflections", "true"), ao("r.RayTracing.AO", "true"),
        sh("r.RayTracing.Shadows", "true"), pt("r.PathTracing", "false"), tr("r.RayTracing.Translucency", "true"),
        vol("r.RayTracing.Volumetrics", "true");

    rhi::DeviceCaps none;
    rhi::DeviceCaps rtx;
    rtx.accelerationStructure = rtx.rayQuery = true;
    RenderSettings on;
    on.rayTracing = true;
    RenderSettings off;

    // No hardware: identical to raster even with r.RayTracing on.
    std::set<std::string> r = names(reg.resolve(on, none));
    EXPECT_EQ(r, (std::set<std::string>{"ShadowsRasterStub", "AOStub", "ReflectionsStub", "IrradianceStub"}));
    // Hardware but the checkbox off: raster.
    EXPECT_EQ(names(reg.resolve(off, rtx)), r);
    // Hardware + checkbox: every RT variant wins its group, the scene feature joins.
    r = names(reg.resolve(on, rtx));
    for (const char* n : {"RayTracingScene", "ShadowsRT", "AmbientOcclusionRT", "ReflectionsRT", "GlobalIlluminationRT",
                          "TranslucencyRT"}) {
        EXPECT_TRUE(r.count(n)) << n;
    }
    for (const char* n : {"ShadowsRasterStub", "AOStub", "ReflectionsStub", "IrradianceStub", "PathTracer"}) {
        EXPECT_FALSE(r.count(n)) << n;
    }
    {
        // A single effect off keeps its raster counterpart.
        CVarOverride a("r.RayTracing.AO", "false");
        r = names(reg.resolve(on, rtx));
        EXPECT_TRUE(r.count("AOStub"));
        EXPECT_FALSE(r.count("AmbientOcclusionRT"));
        EXPECT_TRUE(r.count("ShadowsRT"));
    }
    {
        // The per-feature toggle cvar works like for every feature.
        CVarOverride t("r.Feature.ShadowsRT", "false");
        r = names(reg.resolve(on, rtx));
        EXPECT_TRUE(r.count("ShadowsRasterStub"));
    }
    {
        // Every effect off: no TLAS work at all.
        CVarOverride a("r.RayTracing.AO", "false"), b("r.RayTracing.GI", "false"), c("r.RayTracing.Reflections", "false"),
            d("r.RayTracing.Shadows", "false"), e("r.RayTracing.Translucency", "false"), f("r.RayTracing.Volumetrics", "false");
        r = names(reg.resolve(on, rtx));
        EXPECT_FALSE(r.count("RayTracingScene"));
        CVarOverride p("r.PathTracing", "true");
        r = names(reg.resolve(on, rtx));
        EXPECT_TRUE(r.count("PathTracer"));
        EXPECT_TRUE(r.count("RayTracingScene")) << "the path tracer needs the TLAS";
    }
    EXPECT_FALSE(rayTracedRefractionActive(on, none));
    EXPECT_TRUE(rayTracedRefractionActive(on, rtx));
}

// --- shaders ---

TEST(RtShaders, EveryRayTracingShaderAndVariantCompiles) {
    namespace fs = std::filesystem;
    rhi::ShaderCompilerOptions o = rhi::ShaderCompilerOptions::defaults();
    o.cacheDirectory.clear();
    rhi::ShaderCompiler compiler(o);
    const fs::path dir = rhi::ShaderCompiler::engineShaderRoot() / "render" / "raytracing";
    ASSERT_TRUE(fs::exists(dir)) << dir;
    // Define variants used by the C++ side (the generic all-shaders test only compiles the default variant).
    const std::map<std::string, std::vector<std::vector<rhi::ShaderDefine>>> variants = {
        {"denoise_atrous.comp", {{}, {{"OX_OUT_RGBA8"}}, {{"OX_OUT_R8"}}}},
        {"denoise_upsample.comp", {{}, {{"OX_OUT_RGBA8"}}, {{"OX_OUT_R8"}}}},
        {"ddgi_update.comp", {{}, {{"OX_DDGI_DEPTH"}}}},
    };
    u32 compiled = 0;
    std::set<rhi::ShaderStage> stages;
    for (const auto& e : fs::directory_iterator(dir)) {
        const rhi::ShaderStage stage = rhi::shaderStageFromPath(e.path());
        if (stage == rhi::ShaderStage::Unknown) continue;
        auto it = variants.find(e.path().filename().string());
        const std::vector<std::vector<rhi::ShaderDefine>> defs = it != variants.end() ? it->second : std::vector<std::vector<rhi::ShaderDefine>>{{}};
        for (const auto& d : defs) {
            rhi::ShaderCompileDesc desc;
            desc.path = e.path();
            desc.defines = d;
            const rhi::ShaderCompileResult r = compiler.compile(desc);
            EXPECT_TRUE(r.success) << e.path() << (d.empty() ? "" : " " + d[0].name) << ":\n" << r.errors;
            EXPECT_LE(r.reflection.pushConstantSize, rhi::kMaxPushConstantSize) << e.path();
            ++compiled;
        }
        stages.insert(stage);
    }
    EXPECT_GE(compiled, 25u);
    // Ray query compute shaders and the full RT pipeline stage set.
    for (rhi::ShaderStage s : {rhi::ShaderStage::Compute, rhi::ShaderStage::RayGen, rhi::ShaderStage::ClosestHit,
                               rhi::ShaderStage::AnyHit, rhi::ShaderStage::Miss}) {
        EXPECT_TRUE(stages.count(s)) << rhi::shaderStageName(s);
    }
}
