// Split rendering planner (CPU only): transfer path selection from capabilities, cut selection from cost models,
// hysteresis, timing filters.
#include <oxwald/rhi/split_scheduler.hpp>

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::rhi;

namespace {

CrossDeviceCaps sameAdapterCaps() {
    CrossDeviceCaps c;
    c.sameDevice = true;
    c.luidMatch = true;
    c.primaryExportOpaque = c.secondaryImportOpaque = true;
    c.primaryHostPointer = c.secondaryHostPointer = true;
    c.hostPointerAlignment = 4096;
    return c;
}

CrossDeviceCaps dgpuPlusIgpuCaps() {
    // NVIDIA + Intel: different UUIDs, both expose VK_EXT_external_memory_host.
    CrossDeviceCaps c;
    c.primaryExportOpaque = c.secondaryImportOpaque = true; // exportable, but useless across devices
    c.primaryHostPointer = c.secondaryHostPointer = true;
    c.hostPointerAlignment = 4096;
    return c;
}

SplitSegmentCost segment(const char* name, f32 primaryMs, f32 secondaryMs, u64 up, u64 down, bool portable = true) {
    SplitSegmentCost s;
    s.name = name;
    s.primaryMs = primaryMs;
    s.secondaryMs = secondaryMs;
    s.uploadBytes = up;
    s.downloadBytes = down;
    s.portable = portable;
    return s;
}

constexpr u64 k1080pRgba8 = 1920ull * 1080 * 4;

} // namespace

TEST(SplitTransferPath, AutoPrefersSharedMemoryOnSameDevice) {
    const TransferPathChoice c = selectTransferPath(sameAdapterCaps(), CrossDeviceTransfer::Auto);
    EXPECT_EQ(c.path, CrossDeviceTransfer::ExternalMemory);
}

TEST(SplitTransferPath, DifferentGpusUseHostPointerNotOpaqueHandles) {
    const CrossDeviceCaps caps = dgpuPlusIgpuCaps();
    EXPECT_FALSE(transferPathAvailable(caps, CrossDeviceTransfer::ExternalMemory));
    EXPECT_EQ(selectTransferPath(caps, CrossDeviceTransfer::Auto).path, CrossDeviceTransfer::HostPointer);
    const TransferPathChoice forced = selectTransferPath(caps, CrossDeviceTransfer::ExternalMemory);
    EXPECT_EQ(forced.path, CrossDeviceTransfer::HostPointer);
    EXPECT_NE(forced.reason.find("unavailable"), std::string::npos) << forced.reason;
    EXPECT_NE(forced.reason.find("deviceUUID"), std::string::npos) << forced.reason;
}

TEST(SplitTransferPath, StagingIsTheUniversalFallback) {
    CrossDeviceCaps none;
    EXPECT_EQ(selectTransferPath(none, CrossDeviceTransfer::Auto).path, CrossDeviceTransfer::Staging);
    EXPECT_EQ(selectTransferPath(none, CrossDeviceTransfer::HostPointer).path, CrossDeviceTransfer::Staging);
    EXPECT_EQ(selectTransferPath(sameAdapterCaps(), CrossDeviceTransfer::Staging).path, CrossDeviceTransfer::Staging);
    CrossDeviceCaps onlyPrimaryHost;
    onlyPrimaryHost.primaryHostPointer = true;
    const TransferPathChoice c = selectTransferPath(onlyPrimaryHost, CrossDeviceTransfer::HostPointer);
    EXPECT_EQ(c.path, CrossDeviceTransfer::Staging);
    EXPECT_NE(c.reason.find("secondary"), std::string::npos) << c.reason;
}

TEST(SplitTransferPath, PrimaryWithoutExtensionsCannotShare) {
    CrossDeviceCaps c = sameAdapterCaps();
    c.primaryExportOpaque = false;
    c.primaryHostPointer = false;
    EXPECT_EQ(selectTransferPath(c, CrossDeviceTransfer::Auto).path, CrossDeviceTransfer::Staging);
}

TEST(SplitScheduler, LightPostTailOnSlowIgpuIsNotProfitable) {
    // RTX 3080 + UHD 630 at 1080p: FXAA ~0.1 ms on the dGPU, ~4 ms on the iGPU; 8 MB each way over PCIe.
    SplitCostModel m;
    m.bandwidth = defaultTransferBandwidth(CrossDeviceTransfer::HostPointer, false);
    m.primaryFrameMs = 6.0f;
    const SplitSegmentCost segs[] = {segment("FXAA", 0.1f, 4.0f, k1080pRgba8, k1080pRgba8)};
    const SplitDecision d = chooseSplit(segs, m);
    EXPECT_FALSE(d.offloadsAnything) << d.reason;
    EXPECT_FALSE(d.segments[0].offload);
    EXPECT_LT(d.segments[0].primarySavedMs, 0.0f); // the two PCIe copies cost more than the pass
    EXPECT_NE(d.reason.find("not profitable"), std::string::npos) << d.reason;
}

TEST(SplitScheduler, HeavyPassWithSmallTransfersIsOffloaded) {
    // e.g. a cloud noise / probe update: 3 ms on the primary, tiny result.
    SplitCostModel m;
    m.bandwidth = defaultTransferBandwidth(CrossDeviceTransfer::HostPointer, false);
    m.primaryFrameMs = 16.0f;
    const SplitSegmentCost segs[] = {segment("CloudNoise", 3.0f, 9.0f, 64 * 1024, 512 * 1024)};
    const SplitDecision d = chooseSplit(segs, m);
    ASSERT_TRUE(d.offloadsAnything) << d.reason;
    EXPECT_TRUE(d.segments[0].offload);
    EXPECT_GT(d.primarySavedMs, 2.8f);
    EXPECT_LT(d.secondaryLoadMs, 0.9f * d.newPrimaryFrameMs);
}

TEST(SplitScheduler, SecondaryMustKeepUpWithTheFrame) {
    // Big saving on the primary, but the secondary would need 20 ms per frame while the frame is ~10 ms.
    SplitCostModel m;
    m.bandwidth = defaultTransferBandwidth(CrossDeviceTransfer::HostPointer, false);
    m.primaryFrameMs = 12.0f;
    const SplitSegmentCost segs[] = {segment("SSAO", 2.0f, 20.0f, 1024 * 1024, 1024 * 1024)};
    const SplitDecision d = chooseSplit(segs, m);
    EXPECT_FALSE(d.offloadsAnything) << d.reason;
    EXPECT_NE(d.reason.find("bottleneck"), std::string::npos) << d.reason;
}

TEST(SplitScheduler, PicksTheBestFeasibleSubset) {
    SplitCostModel m;
    m.bandwidth = defaultTransferBandwidth(CrossDeviceTransfer::HostPointer, false);
    m.bandwidth.fixedMs = 0.0f;
    m.primaryFrameMs = 10.0f;
    // Secondary budget ~ 0.9 * (10 - saved). A+B together exceed it; B+C fit and save more than A alone.
    const SplitSegmentCost segs[] = {
        segment("A", 2.0f, 7.0f, 0, 0),
        segment("B", 1.5f, 3.0f, 0, 0),
        segment("C", 1.0f, 2.0f, 0, 0),
        segment("NotPortable", 5.0f, 1.0f, 0, 0, false),
    };
    const SplitDecision d = chooseSplit(segs, m);
    ASSERT_TRUE(d.offloadsAnything) << d.reason;
    EXPECT_FALSE(d.segments[3].offload);
    // Feasible best: A+C (saved 3.0, load 9.0 > 0.9*7=6.3 -> no), B+C (2.5, load 5 <= 0.9*7.5) , A alone (2, 7 <= 7.2).
    EXPECT_FALSE(d.segments[0].offload);
    EXPECT_TRUE(d.segments[1].offload);
    EXPECT_TRUE(d.segments[2].offload);
    EXPECT_NEAR(d.primarySavedMs, 2.5f, 1e-4f);
    EXPECT_NEAR(d.newPrimaryFrameMs, 7.5f, 1e-4f);
}

TEST(SplitScheduler, ForceOffloadsEveryPortableSegment) {
    SplitCostModel m;
    const SplitSegmentCost segs[] = {segment("FXAA", 0.1f, 4.0f, k1080pRgba8, k1080pRgba8),
                                     segment("Opaque", 1.0f, 1.0f, 0, 0, false)};
    const SplitDecision d = chooseSplit(segs, m, true);
    EXPECT_TRUE(d.segments[0].offload);
    EXPECT_FALSE(d.segments[1].offload);
    EXPECT_TRUE(d.offloadsAnything);
}

TEST(SplitScheduler, StagingCountsCpuCopiesAgainstTheBudget) {
    SplitCostModel m;
    m.bandwidth = defaultTransferBandwidth(CrossDeviceTransfer::Staging, false);
    const SplitSegmentPlan p = estimateSegment(segment("X", 1.0f, 1.0f, 6'000'000, 6'000'000), m);
    EXPECT_NEAR(p.cpuCopyMs, 2.0f, 0.01f); // 12 MB at 6 GB/s
    m.bandwidth = defaultTransferBandwidth(CrossDeviceTransfer::HostPointer, false);
    EXPECT_EQ(estimateSegment(segment("X", 1.0f, 1.0f, 6'000'000, 6'000'000), m).cpuCopyMs, 0.0f);
}

TEST(SplitScheduler, ManySegmentsUseGreedy) {
    SplitCostModel m;
    m.bandwidth.fixedMs = 0.0f;
    m.bandwidth.cpuCopyGBps = 0.0f;
    m.primaryFrameMs = 100.0f;
    std::vector<SplitSegmentCost> segs;
    for (int i = 0; i < 20; ++i) segs.push_back(segment("S", 1.0f, 2.0f, 0, 0));
    const SplitDecision d = chooseSplit(segs, m);
    EXPECT_TRUE(d.offloadsAnything);
    // load 2k <= 0.9 * (100 - k)  ->  k <= 31: all 20 fit.
    EXPECT_NEAR(d.primarySavedMs, 20.0f, 1e-3f);
}

TEST(SplitController, RequiresConsecutiveConfirmations) {
    SplitController c(3);
    SplitDecision on;
    on.segments.resize(1);
    on.segments[0].offload = true;
    SplitDecision off;
    off.segments.resize(1);
    EXPECT_FALSE(c.update(on)[0]);
    EXPECT_FALSE(c.update(on)[0]);
    EXPECT_TRUE(c.update(on)[0]);
    EXPECT_TRUE(c.update(off)[0]); // one dissenting evaluation is not enough
    EXPECT_TRUE(c.update(on)[0]);  // ... and resets the count
    EXPECT_TRUE(c.update(off)[0]);
    EXPECT_TRUE(c.update(off)[0]);
    EXPECT_FALSE(c.update(off)[0]);
}

TEST(SplitController, SegmentCountChangeResets) {
    SplitController c(1);
    SplitDecision on;
    on.segments.resize(1);
    on.segments[0].offload = true;
    EXPECT_TRUE(c.update(on)[0]);
    on.segments.resize(2);
    const auto& p = c.update(on);
    ASSERT_EQ(p.size(), 2u);
    EXPECT_TRUE(p[0]); // confirmations = 1: applied right away
}

TEST(TimingFilter, ExponentialAverage) {
    TimingFilter f(0.5f);
    EXPECT_FALSE(f.valid());
    EXPECT_EQ(f.value(7.0f), 7.0f);
    f.add(2.0f);
    EXPECT_EQ(f.value(), 2.0f);
    f.add(4.0f);
    EXPECT_EQ(f.value(), 3.0f);
    f.add(-1.0f); // rejected
    EXPECT_EQ(f.samples(), 2u);
    f.reset();
    EXPECT_FALSE(f.valid());
}
