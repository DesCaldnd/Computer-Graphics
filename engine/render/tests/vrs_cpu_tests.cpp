// Variable rate shading: CPU reference of the per-tile rate decision, supported-rate fitting, availability, pass
// lists and the image metrics used by the measurements.
#include <oxwald/render/features/vrs/vrs.hpp>

#include "vrs_image_metrics.hpp"

#include <gtest/gtest.h>

using namespace ox;
using namespace ox::render::vrs;

namespace {

// NVIDIA Ampere: 1x1 1x2 2x1 2x2 2x4 4x2 4x4.
constexpr u16 kNvidiaMask = u16((1u << encodeRate(0, 0)) | (1u << encodeRate(0, 1)) | (1u << encodeRate(1, 0)) |
                                (1u << encodeRate(1, 1)) | (1u << encodeRate(1, 2)) | (1u << encodeRate(2, 1)) |
                                (1u << encodeRate(2, 2)));

DecisionParams params() {
    DecisionParams p;
    p.threshold = 0.1f;
    p.hysteresis = 0.25f;
    p.supportedMask = kNvidiaMask;
    return p;
}

TileInput flatTile() {
    TileInput t;
    t.statsValid = true;
    t.meanLuma = 0.5f;
    return t;
}

} // namespace

TEST(VrsDecision, FlatRegionIsCoarseEdgesAreFull) {
    const DecisionParams p = params();
    EXPECT_EQ(decideRate(flatTile(), p), encodeRate(2, 2));
    TileInput edge = flatTile();
    edge.errorX = edge.errorY = 0.5f;
    EXPECT_EQ(decideRate(edge, p), encodeRate(0, 0));
    // Vertical stripes: horizontal gradients only → full rate across, coarse along (1x4 unsupported → 1x2).
    TileInput stripes = flatTile();
    stripes.errorX = 0.5f;
    EXPECT_EQ(decideRate(stripes, p), encodeRate(0, 1));
    // No statistics (first frame, disocclusion) → full rate.
    TileInput none;
    EXPECT_EQ(decideRate(none, p), encodeRate(0, 0));
}

TEST(VrsDecision, MaxRateAndSupportedFallback) {
    DecisionParams p = params();
    p.maxLog2 = 1;
    EXPECT_EQ(decideRate(flatTile(), p), encodeRate(1, 1));
    EXPECT_EQ(fitSupportedRate(2, 0, kNvidiaMask), encodeRate(1, 0)); // 4x1 → 2x1
    EXPECT_EQ(fitSupportedRate(2, 2, u16(1u << encodeRate(1, 1) | 1u)), encodeRate(1, 1));
    EXPECT_EQ(fitSupportedRate(2, 2, 1u), encodeRate(0, 0));
}

TEST(VrsDecision, HysteresisKeepsTheRateInsideTheBand) {
    const DecisionParams p = params();
    // jnd at mean 0.5 = 0.1 · 0.6 = 0.06; strict 0.045, loose 0.075 (half rate thresholds on the error).
    TileInput t = flatTile();
    t.errorX = t.errorY = 0.06f; // between strict and loose for half rate, above quarter
    t.prevValid = true;
    t.prevRate = encodeRate(0, 0);
    EXPECT_EQ(decideRate(t, p), encodeRate(0, 0)) << "stays at full rate inside the band";
    t.prevRate = encodeRate(1, 1);
    EXPECT_EQ(decideRate(t, p), encodeRate(1, 1)) << "stays coarse inside the band";
    t.errorX = t.errorY = 0.04f; // below strict → coarsen
    t.prevRate = encodeRate(0, 0);
    EXPECT_EQ(decideRate(t, p), encodeRate(1, 1));
    t.errorX = t.errorY = 0.08f; // above loose → refine at once
    t.prevRate = encodeRate(2, 2);
    EXPECT_EQ(decideRate(t, p), encodeRate(0, 0));
    // Without hysteresis the same oscillating error flips the rate every frame.
    DecisionParams h0 = p;
    h0.hysteresis = 0.0f;
    u32 rate = 0, flips = 0, flipsHyst = 0, rateH = 0;
    for (int i = 0; i < 20; ++i) {
        TileInput o = flatTile();
        o.errorX = o.errorY = (i & 1) ? 0.055f : 0.065f;
        o.prevValid = true;
        o.prevRate = rate;
        const u32 r = decideRate(o, h0);
        flips += r != rate;
        rate = r;
        o.prevRate = rateH;
        const u32 rh = decideRate(o, p);
        flipsHyst += rh != rateH;
        rateH = rh;
    }
    EXPECT_GE(flips, 18u);
    EXPECT_EQ(flipsHyst, 0u);
}

TEST(VrsDecision, MotionLowersTheError) {
    DecisionParams p = params();
    TileInput t = flatTile();
    t.errorX = t.errorY = 0.1f; // full rate when static
    EXPECT_EQ(decideRate(t, p), encodeRate(0, 0));
    t.motionPixelsX = 40.0f; // fast horizontal pan: 0.1 / (1 + 0.15·40) = 0.014 → coarse horizontally
    EXPECT_EQ(decideRate(t, p), encodeRate(1, 0)) << "4x1 is not supported: 2x1";
    p.motionScale = 0.0f;
    EXPECT_EQ(decideRate(t, p), encodeRate(0, 0));
}

TEST(VrsDecision, MotionFoveatedAndDepthOfFieldModes) {
    DecisionParams p = params();
    p.mode = Mode::Motion;
    TileInput t; // no statistics needed
    t.motionPixelsX = 7.0f;
    t.motionPixelsY = 20.0f;
    EXPECT_EQ(decideRate(t, p), encodeRate(1, 2));
    p.mode = Mode::Foveated;
    t = {};
    t.peripheryDistance = 0.3f;
    EXPECT_EQ(decideRate(t, p), encodeRate(0, 0));
    t.peripheryDistance = 0.6f;
    EXPECT_EQ(decideRate(t, p), encodeRate(1, 1));
    t.peripheryDistance = 0.9f;
    EXPECT_EQ(decideRate(t, p), encodeRate(2, 2));
    p.mode = Mode::Adaptive;
    t = flatTile();
    t.errorX = t.errorY = 1.0f;
    t.minCocPixels = 4.0f;
    EXPECT_EQ(decideRate(t, p), encodeRate(1, 1)) << "blurred by depth of field";
    p.depthOfField = false;
    EXPECT_EQ(decideRate(t, p), encodeRate(0, 0));
}

TEST(VrsAvailability, ReasonsWithoutSupport) {
    rhi::DeviceCaps caps;
    caps.gpuName = "Test GPU";
    Availability a = availability(caps);
    EXPECT_FALSE(a.available);
    EXPECT_NE(a.reason.find("VK_KHR_fragment_shading_rate"), std::string::npos);
    caps.fragmentShadingRate = true;
    caps.pipelineShadingRate = true;
    EXPECT_FALSE(availability(caps).available);
    caps.attachmentShadingRate = true;
    caps.shadingRateTexelMinWidth = caps.shadingRateTexelMinHeight = 16;
    caps.shadingRateTexelMaxWidth = caps.shadingRateTexelMaxHeight = 16;
    caps.shadingRateMask = kNvidiaMask;
    a = availability(caps);
    EXPECT_TRUE(a.available) << a.reason;
    EXPECT_EQ(a.texelSize, 16u);
    caps.shadingRateTexelMinWidth = caps.shadingRateTexelMinHeight = 4;
    caps.shadingRateTexelMaxWidth = caps.shadingRateTexelMaxHeight = 32;
    EXPECT_EQ(chooseTexelSize(caps), 8u);
}

TEST(VrsPasses, ExclusionList) {
    for (const std::string& p : defaultPasses()) EXPECT_FALSE(isExcludedPass(p)) << p;
    for (const char* p : {"DepthPrepass", "DepthPrepass.Late", "Shadow.Cascades", "Shadow.Points", "World.Prepass",
                          "World.ShadowCascades", "Tonemap", "Final", "TAA", "DLSS", "UI", "EditorGrid",
                          "SelectionOutline", "DebugLines", "VRS.Overlay", "Particles.LowResDepth", "Pick"}) {
        EXPECT_TRUE(isExcludedPass(p)) << p;
    }
}

TEST(VrsPasses, ParsePassList) {
    EXPECT_EQ(parsePassList(""), defaultPasses());
    EXPECT_EQ(parsePassList(" , "), defaultPasses());
    EXPECT_EQ(parsePassList("ForwardOpaque"), std::vector<std::string>{"ForwardOpaque"});
    const std::vector<std::string> two{"ForwardOpaque", "Water.Surface"};
    EXPECT_EQ(parsePassList(" ForwardOpaque ,,	Water.Surface "), two);
}

TEST(VrsMetrics, IdenticalAndDifferentImages) {
    const u32 w = 64, h = 48;
    std::vector<u8> a(w * h * 4), b;
    for (u32 i = 0; i < w * h; ++i) {
        a[i * 4] = u8((i * 7) & 255);
        a[i * 4 + 1] = u8((i / w) * 5);
        a[i * 4 + 2] = 128;
        a[i * 4 + 3] = 255;
    }
    b = a;
    using namespace ox::vrs_metrics;
    const ImageView va{w, h, a.data()}, vb{w, h, b.data()};
    EXPECT_GE(psnr(va, vb), 99.0);
    EXPECT_NEAR(ssim(va, vb), 1.0, 1e-6);
    EXPECT_NEAR(flip(va, vb), 0.0, 1e-6);
    for (u32 i = 0; i < w * h * 4; i += 4) b[i] = u8(std::min(255, b[i] + 20));
    const double p = psnr(va, vb), s = ssim(va, vb), f = flip(va, vb);
    EXPECT_LT(p, 30.0);
    EXPECT_LT(s, 1.0);
    EXPECT_GT(f, 0.01);
    EXPECT_LT(f, 0.5);
}
