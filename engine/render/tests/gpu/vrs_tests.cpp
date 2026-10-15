// Variable rate shading GPU tests (labels render;gpu). Skipped where VK_KHR_fragment_shading_rate attachment rates are
// unavailable (MoltenVK), except NoOpWithoutTheExtension which runs everywhere.
#include "render_fixture.hpp"

#include "features/vrs/vrs_internal.hpp"
#include "vrs_image_metrics.hpp"

#include <oxwald/core/math.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/vrs/vrs.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <sstream>
#include <cstdlib>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

namespace fs = std::filesystem;

namespace {

class VrsTest : public RenderTest {
protected:
    void SetUp() override {
        RenderTest::SetUp();
        if (IsSkipped() || !device) return;
        const vrs::Availability a = vrs::availability(device->caps());
        if (!a.available) GTEST_SKIP() << "variable rate shading unavailable: " << a.reason;
        tileSize = a.texelSize;
    }

    rhi::TextureHandle texture(VkFormat format, u32 w, u32 h, std::span<const u8> data, rhi::TextureUsage extra = {}) {
        rhi::TextureDesc d;
        d.format = format;
        d.width = w;
        d.height = h;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::TransferDst |
                  rhi::TextureUsage::TransferSrc | extra;
        d.name = "vrs.test";
        const rhi::TextureHandle t = device->createTexture(d);
        if (!data.empty()) device->uploadTexture(t, data);
        owned.push_back(t);
        return t;
    }
    void TearDown() override {
        if (device) {
            device->waitIdle();
            for (rhi::TextureHandle t : owned) device->destroy(t);
        }
        RenderTest::TearDown();
    }

    static std::vector<u8> halfs(const std::vector<f32>& v) {
        std::vector<u8> out(v.size() * 2);
        for (usize i = 0; i < v.size(); ++i) {
            const u16 h = glm::packHalf1x16(v[i]);
            std::memcpy(out.data() + i * 2, &h, 2);
        }
        return out;
    }

    // Runs the Analyze (optional) and Generate passes on synthetic inputs and returns the generated rate image.
    std::vector<u8> runPasses(Extent2D size, rhi::TextureHandle color, rhi::TextureHandle stats, rhi::TextureHandle prevRate,
                              rhi::TextureHandle velocity, const vrs::DecisionParams& params, bool historyValid) {
        const Extent2D tiles = vrs::tileCount(size, tileSize);
        const std::vector<f32> zeros(usize(size.width) * size.height, 0.0f);
        const rhi::TextureHandle depth = texture(VK_FORMAT_R32_SFLOAT, size.width, size.height,
                                                 {reinterpret_cast<const u8*>(zeros.data()), zeros.size() * sizeof(f32)});
        const rhi::TextureHandle out = texture(VK_FORMAT_R8_UINT, tiles.width, tiles.height, {},
                                               rhi::TextureUsage::ShadingRateAttachment);
        vrs::Pipelines pl;
        EXPECT_TRUE(pl.create(*device));
        rhi::RenderGraph graph;
        device->beginFrame();
        graph.reset();
        const rhi::RGTexture gStats = graph.importTexture(*device, stats);
        const rhi::RGTexture gPrev = graph.importTexture(*device, prevRate);
        if (color) {
            vrs::AnalyzeInputs a;
            a.color = graph.importTexture(*device, color);
            a.rate = gPrev;
            a.rateValid = false;
            a.outStats = gStats;
            a.renderSize = size;
            a.tileSize = tileSize;
            vrs::addAnalyzePass(graph, pl, a);
        }
        vrs::GenerateInputs g;
        g.depth = graph.importTexture(*device, depth);
        g.velocity = graph.importTexture(*device, velocity);
        g.prevStats = gStats;
        g.prevRate = gPrev;
        g.outRate = graph.importTexture(*device, out, rhi::Access::TransferRead);
        g.historyValid = historyValid;
        g.params = params;
        g.renderSize = size;
        g.tileSize = tileSize;
        vrs::addGeneratePass(graph, pl, g);
        graph.execute(*device);
        device->endFrame();
        device->waitIdle();
        std::vector<u8> rate = device->readTexture(out);
        graph.releaseResources(*device);
        pl.destroy(*device);
        return rate;
    }

    rhi::TextureHandle zeroVelocity(Extent2D size) {
        const std::vector<f32> v(usize(size.width) * size.height * 2, 0.0f);
        return texture(VK_FORMAT_R16G16_SFLOAT, size.width, size.height, halfs(v));
    }

    vrs::DecisionParams defaultParams() const {
        vrs::DecisionParams p;
        p.threshold = 0.1f;
        p.hysteresis = 0.25f;
        p.supportedMask = device->caps().shadingRateMask;
        return p;
    }

    // Rate image the main passes used in the last rendered frame (empty when none).
    std::vector<u8> lastRateImage(u32* width = nullptr) {
        rhi::RenderGraph& g = renderer->view(view)->graph();
        for (u32 i = 0; i < g.passCount(); ++i) {
            if (g.passName(i) != "ForwardOpaque") continue;
            const rhi::RGTexture t = g.passShadingRate(i);
            if (!t.valid()) return {};
            const rhi::TextureHandle h = g.physicalTexture(t);
            if (width) *width = device->desc(h).width;
            return device->readTexture(h);
        }
        return {};
    }
    std::vector<std::string> passesWithShadingRate() {
        std::vector<std::string> out;
        rhi::RenderGraph& g = renderer->view(view)->graph();
        for (u32 i = 0; i < g.passCount(); ++i) {
            if (g.passShadingRate(i).valid()) out.push_back(g.passName(i));
        }
        return out;
    }
    bool hasPass(std::string_view name) {
        rhi::RenderGraph& g = renderer->view(view)->graph();
        for (u32 i = 0; i < g.passCount(); ++i) {
            if (g.passName(i) == name) return true;
        }
        return false;
    }

    void buildScene() {
        const Uuid grey = material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.6f);
        const Uuid red = material({0.8f, 0.15f, 0.1f, 1.0f}, 0.0f, 0.35f);
        const Uuid metal = material({0.9f, 0.9f, 0.9f, 1.0f}, 1.0f, 0.2f);
        const Uuid glass = material({0.3f, 0.6f, 1.0f, 0.35f}, 0.0f, 0.1f, glm::vec3(0.0f), assets::BlendMode::Transparent);
        mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(30.0f));
        for (int i = 0; i < 5; ++i) {
            mesh(Primitive::Sphere, i & 1 ? red : metal, {-4.0f + 2.0f * f32(i), 0.6f, -1.0f}, glm::vec3(1.2f));
            mesh(Primitive::Cube, grey, {-4.0f + 2.0f * f32(i), 0.5f, 2.0f}, glm::vec3(0.8f));
        }
        mesh(Primitive::Cube, glass, {0, 1.0f, 0.5f}, glm::vec3(1.5f, 1.5f, 0.2f));
        sun({-0.4f, -0.8f, -0.45f}, 30000.0f);
        pointLight({2, 1.5f, 1}, 3000.0f, 6.0f, {1.0f, 0.8f, 0.6f});
        environment();
    }

    // GoldenTest.ManyPointLightsClustered: 256 point lights over a rough floor, 5 spheres.
    void manyLightsScene() {
        const Uuid ground = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.6f);
        const Uuid obj = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.3f);
        mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(40.0f));
        for (int i = 0; i < 5; ++i) mesh(Primitive::Sphere, obj, {(f32(i) - 2.0f) * 3.0f, 0.75f, 0.0f}, glm::vec3(1.5f));
        Random rng(42);
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                const glm::vec3 color{0.3f + 0.7f * rng.nextFloat(), 0.3f + 0.7f * rng.nextFloat(), 0.3f + 0.7f * rng.nextFloat()};
                pointLight({(f32(x) - 7.5f) * 1.6f, 0.4f, (f32(z) - 7.5f) * 1.6f}, 400.0f, 2.2f, color, false);
            }
        }
    }

    // TranslucencyTest.RefractiveSphereAbsorption: a checker floor seen through a green refractive sphere.
    void refractiveScene() {
        std::vector<u8> px(64 * 64 * 4);
        for (u32 y = 0; y < 64; ++y) {
            for (u32 x = 0; x < 64; ++x) {
                const u8 c = ((x / 8) + (y / 8)) & 1 ? 230 : 20;
                const usize i = (usize(y) * 64 + x) * 4;
                px[i] = px[i + 1] = px[i + 2] = c;
                px[i + 3] = 255;
            }
        }
        assets::TextureData t;
        t.format = assets::TextureFormat::RGBA8Srgb;
        t.width = t.height = 64;
        t.mipCount = 1;
        t.filter = assets::TextureFilter::Nearest;
        assets::TextureMip mip;
        mip.width = mip.height = 64;
        mip.data.resize(px.size());
        std::memcpy(mip.data.data(), px.data(), px.size());
        t.mips.push_back(std::move(mip));
        const Uuid tex = Uuid::fromName("test.vrs.checker");
        renderer->resources().addTexture(tex, t);
        assets::MaterialAsset floor;
        floor.roughness = 0.7f;
        floor.albedoTexture = tex;
        floor.uvTiling = glm::vec2(8.0f);
        const Uuid floorId = Uuid::fromName("test.vrs.floor");
        renderer->resources().addMaterial(floorId, floor);
        assets::MaterialAsset glass;
        glass.roughness = 0.02f;
        glass.blendMode = assets::BlendMode::Refractive;
        glass.ior = 1.5f;
        glass.absorptionColor = {0.45f, 0.9f, 0.55f};
        glass.absorptionDistance = 1.0f;
        const Uuid glassId = Uuid::fromName("test.vrs.glass");
        renderer->resources().addMaterial(glassId, glass);
        mesh(Primitive::Plane, floorId, {0, 0, 0}, glm::vec3(10.0f));
        mesh(Primitive::Sphere, glassId, {0, 0.75f, 0}, glm::vec3(1.5f));
        sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 30000.0f);
        environment();
    }

    struct Quality {
        f64 meanError = 0;  // mean |difference| over RGB, 0..255 (the golden comparator's measure)
        f64 worstTile = 0;  // largest mean |difference| of one 16×16 block, 0..255 (local blockiness)
        f64 psnr = 0, ssim = 0, flip = 0;
        f64 coarse = 0;     // fraction of rate image tiles coarser than 1×1
    };
    static Quality compare(const Image& ref, const Image& img) {
        Quality q;
        using namespace ox::vrs_metrics;
        const ImageView a{ref.width, ref.height, ref.rgba.data()}, b{img.width, img.height, img.rgba.data()};
        q.psnr = psnr(a, b);
        q.ssim = ssim(a, b);
        q.flip = flip(a, b);
        u64 sum = 0;
        for (u32 ty = 0; ty < ref.height / 16; ++ty) {
            for (u32 tx = 0; tx < ref.width / 16; ++tx) {
                u64 tile = 0;
                for (u32 y = ty * 16; y < ty * 16 + 16; ++y) {
                    for (u32 x = tx * 16; x < tx * 16 + 16; ++x) {
                        const usize i = (usize(y) * ref.width + x) * 4;
                        for (int c = 0; c < 3; ++c) tile += u64(std::abs(int(ref.rgba[i + c]) - int(img.rgba[i + c])));
                    }
                }
                sum += tile;
                q.worstTile = std::max(q.worstTile, f64(tile) / (16.0 * 16.0 * 3.0));
            }
        }
        q.meanError = f64(sum) / (f64(ref.width / 16 * 16) * f64(ref.height / 16 * 16) * 3.0);
        return q;
    }

    static u32 coarseTiles(const std::vector<u8>& rate) {
        return u32(std::count_if(rate.begin(), rate.end(), [](u8 r) { return r != 0; }));
    }

    u32 tileSize = 16;
    std::vector<rhi::TextureHandle> owned;
};

} // namespace

// Flat regions → coarse, vertical stripes → full rate across the stripes, checkerboard → 1x1.
TEST_F(VrsTest, RateImageFromSyntheticInputs) {
    const Extent2D size{16 * tileSize, 4 * tileSize};
    std::vector<f32> rgba(usize(size.width) * size.height * 4, 1.0f);
    for (u32 y = 0; y < size.height; ++y) {
        for (u32 x = 0; x < size.width; ++x) {
            const u32 tx = x / tileSize;
            f32 v = 0.5f;
            if (tx >= 6 && tx < 12) v = (x & 1) ? 0.9f : 0.1f;
            if (tx >= 12) v = ((x ^ y) & 1) ? 0.9f : 0.1f;
            for (int c = 0; c < 3; ++c) rgba[(usize(y) * size.width + x) * 4 + c] = v;
        }
    }
    const rhi::TextureHandle color = texture(VK_FORMAT_R16G16B16A16_SFLOAT, size.width, size.height, halfs(rgba));
    const Extent2D tiles = vrs::tileCount(size, tileSize);
    const std::vector<u8> zeroRate(usize(tiles.width) * tiles.height, 0);
    const rhi::TextureHandle prevRate = texture(VK_FORMAT_R8_UINT, tiles.width, tiles.height, zeroRate,
                                                rhi::TextureUsage::ShadingRateAttachment);
    const rhi::TextureHandle stats = texture(VK_FORMAT_R16G16B16A16_SFLOAT, tiles.width, tiles.height, {});
    const std::vector<u8> rate = runPasses(size, color, stats, prevRate, zeroVelocity(size), defaultParams(), true);
    ASSERT_EQ(rate.size(), usize(tiles.width) * tiles.height);
    const u32 stripeRate = vrs::fitSupportedRate(0, 2, device->caps().shadingRateMask);
    for (u32 ty = 0; ty < tiles.height; ++ty) {
        for (u32 tx = 0; tx < tiles.width; ++tx) {
            const u8 r = rate[ty * tiles.width + tx];
            if (tx < 5) EXPECT_EQ(r, vrs::encodeRate(2, 2)) << "flat tile " << tx << "," << ty;
            else if (tx > 6 && tx < 11) EXPECT_EQ(r, stripeRate) << "stripe tile " << tx << "," << ty;
            else if (tx > 12) EXPECT_EQ(r, 0u) << "checker tile " << tx << "," << ty;
        }
    }
    EXPECT_EQ(vrs::rateLog2Width(stripeRate), 0u);
}

// The generate shader makes the same decision as the CPU reference (random statistics and previous rates), and
// Motion mode coarsens along the motion.
TEST_F(VrsTest, GenerateShaderMatchesCpuReference) {
    const Extent2D size{24 * tileSize, 12 * tileSize};
    const Extent2D tiles = vrs::tileCount(size, tileSize);
    const usize n = usize(tiles.width) * tiles.height;
    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> u01(0.0f, 1.0f), uErr(0.0f, 0.15f);
    std::vector<u32> supported;
    for (u32 r = 0; r < 16; ++r) {
        if ((device->caps().shadingRateMask >> r) & 1u) supported.push_back(r);
    }
    std::vector<f32> stats(n * 4);
    std::vector<u8> prev(n);
    for (usize i = 0; i < n; ++i) {
        stats[i * 4] = u01(rng);
        stats[i * 4 + 1] = uErr(rng);
        stats[i * 4 + 2] = uErr(rng);
        stats[i * 4 + 3] = 1.0f;
        prev[i] = u8(supported[rng() % supported.size()]);
    }
    const std::vector<u8> statsHalf = halfs(stats);
    const rhi::TextureHandle statsTex = texture(VK_FORMAT_R16G16B16A16_SFLOAT, tiles.width, tiles.height, statsHalf);
    const rhi::TextureHandle prevTex = texture(VK_FORMAT_R8_UINT, tiles.width, tiles.height, prev,
                                               rhi::TextureUsage::ShadingRateAttachment);
    const vrs::DecisionParams p = defaultParams();
    const std::vector<u8> rate = runPasses(size, {}, statsTex, prevTex, zeroVelocity(size), p, true);
    ASSERT_EQ(rate.size(), n);
    u32 mismatches = 0, coarse = 0;
    for (usize i = 0; i < n; ++i) {
        vrs::TileInput t;
        u16 h[4];
        std::memcpy(h, statsHalf.data() + i * 8, 8);
        t.statsValid = true;
        t.meanLuma = glm::unpackHalf1x16(h[0]);
        t.errorX = glm::unpackHalf1x16(h[1]);
        t.errorY = glm::unpackHalf1x16(h[2]);
        t.prevValid = true;
        t.prevRate = prev[i];
        const u32 cpu = vrs::decideRate(t, p);
        mismatches += cpu != rate[i];
        coarse += rate[i] != 0;
    }
    EXPECT_LE(mismatches, n / 200) << mismatches << " of " << n << " tiles differ from the CPU reference";
    EXPECT_GT(coarse, n / 4);

    // Motion mode: uniform horizontal motion of 20 px per frame → 4 (or the nearest supported) across, 1 along y.
    std::vector<f32> v(usize(size.width) * size.height * 2, 0.0f);
    for (usize i = 0; i < v.size(); i += 2) v[i] = 20.0f / f32(size.width);
    const rhi::TextureHandle vel = texture(VK_FORMAT_R16G16_SFLOAT, size.width, size.height, halfs(v));
    vrs::DecisionParams pm = p;
    pm.mode = vrs::Mode::Motion;
    const std::vector<u8> moving = runPasses(size, {}, statsTex, prevTex, vel, pm, false);
    const u32 expected = vrs::fitSupportedRate(2, 0, device->caps().shadingRateMask);
    for (usize i = 0; i < n; ++i) EXPECT_EQ(moving[i], expected) << "tile " << i;
}

// Pipelines are VRS-capable, the rate image goes only to the listed passes and never to excluded ones; no
// validation errors (checked by the fixture).
TEST_F(VrsTest, PassExclusionListRespected) {
    buildScene();
    CVarScope on("r.VRS", "true");
    CVarScope aa("r.AntiAliasing", "2");
    const CameraParams cam = camera({0, 2.5f, 7}, {0, 0.5f, 0}, 13.0f);
    render(cam, {.width = 512, .height = 320, .frames = 3});
    const std::vector<std::string> with = passesWithShadingRate();
    ASSERT_FALSE(with.empty());
    EXPECT_NE(std::find(with.begin(), with.end(), "ForwardOpaque"), with.end());
    for (const std::string& p : with) {
        EXPECT_FALSE(vrs::isExcludedPass(p)) << p;
        EXPECT_NE(std::find(vrs::defaultPasses().begin(), vrs::defaultPasses().end(), p), vrs::defaultPasses().end()) << p;
    }
    EXPECT_TRUE(hasPass("VRS.Generate"));
    EXPECT_TRUE(hasPass("VRS.Analyze"));
    {
        // Excluded passes stay excluded even when listed; passes at another resolution are skipped.
        CVarScope list("r.VRS.Passes", "DepthPrepass,Shadow.Cascades,Tonemap,TAA,ForwardOpaque,Final");
        render(cam, {.width = 512, .height = 320, .frames = 1});
        EXPECT_EQ(passesWithShadingRate(), std::vector<std::string>{"ForwardOpaque"});
    }
    std::string list;
    for (const std::string& p : with) list += p + " ";
    std::printf("[ VRS ] passes with a shading rate image: %s\n", list.c_str());
}

// VRS on is close to VRS off (PSNR / SSIM / FLIP), and it actually coarsened part of the image.
TEST_F(VrsTest, ImageWithinPsnrOfVrsOff) {
    buildScene();
    const CameraParams cam = camera({0, 2.5f, 7}, {0, 0.5f, 0}, 13.0f);
    const Options o{.width = 640, .height = 384, .frames = 4};
    Image off, on;
    {
        CVarScope v("r.VRS", "false");
        off = render(cam, o);
    }
    {
        CVarScope v("r.VRS", "true");
        on = render(cam, o);
    }
    const std::vector<u8> rate = lastRateImage();
    ASSERT_FALSE(rate.empty());
    const f64 coarse = f64(coarseTiles(rate)) / f64(rate.size());
    using namespace ox::vrs_metrics;
    const ImageView a{off.width, off.height, off.rgba.data()}, b{on.width, on.height, on.rgba.data()};
    const f64 p = psnr(a, b), s = ssim(a, b), f = flip(a, b);
    std::printf("[ VRS ] coarse tiles %.0f %%, PSNR %.2f dB, SSIM %.4f, FLIP %.4f\n", coarse * 100.0, p, s, f);
    writePng(fs::temp_directory_path() / "oxwald_render_out" / "vrs_off.png", off);
    writePng(fs::temp_directory_path() / "oxwald_render_out" / "vrs_on.png", on);
    EXPECT_GT(coarse, 0.2) << "VRS did not coarsen anything";
    EXPECT_GT(p, 35.0);
    EXPECT_GT(s, 0.97);
    EXPECT_LT(f, 0.05);
    {
        CVarScope v("r.VRS", "true");
        CVarScope m("r.VRS.Mode", "DebugOverlay");
        const Image overlay = render(cam, {.width = 640, .height = 384, .frames = 1});
        EXPECT_TRUE(hasPass("VRS.Overlay"));
        EXPECT_LT(psnr({overlay.width, overlay.height, overlay.rgba.data()}, b), 30.0) << "overlay visible";
        writePng(fs::temp_directory_path() / "oxwald_render_out" / "vrs_overlay.png", overlay);
    }
}

// A static scene with TAA jitter: the rate image is stable from frame to frame thanks to the hysteresis.
TEST_F(VrsTest, TemporalStabilityWithHysteresis) {
    buildScene();
    CVarScope on("r.VRS", "true");
    CVarScope aa("r.AntiAliasing", "2");
    CVarScope refresh("r.VRS.RefreshPeriod", "0"); // the periodic refresh changes tiles on purpose
    const CameraParams cam = camera({0, 2.5f, 7}, {0, 0.5f, 0}, 13.0f);
    auto changes = [&](const char* hysteresis) {
        CVarScope h("r.VRS.Hysteresis", hysteresis);
        render(cam, {.width = 640, .height = 384, .frames = 6});
        std::vector<u8> prev = lastRateImage();
        u32 changed = 0;
        for (int i = 0; i < 12; ++i) {
            render(cam, {.width = 640, .height = 384, .frames = 1});
            const std::vector<u8> cur = lastRateImage();
            for (usize k = 0; k < cur.size() && k < prev.size(); ++k) changed += cur[k] != prev[k];
            prev = cur;
        }
        return f64(changed) / f64(12 * std::max<usize>(prev.size(), 1));
    };
    const f64 without = changes("0");
    const f64 with = changes("0.25");
    std::printf("[ VRS ] tiles changing per frame: hysteresis 0: %.2f %%, 0.25: %.2f %%\n", without * 100.0, with * 100.0);
    EXPECT_LE(with, without);
    EXPECT_LT(with, 0.02);
}

// The golden scenes (many point lights, refractive sphere) rendered with the VRS threshold / max rate of every Shading
// scalability level (r.VRS forced on, also for High / Ultra where the preset keeps it off but a player may switch it
// on) against r.VRS off. Regression for the blocky floor of GoldenTest.ManyPointLightsClustered and the blurred
// refraction of TranslucencyTest.RefractiveSphereAbsorption with the old defaults (0.08 / 0.12 / 0.16, on at High).
// High / Ultra must be visually lossless, Medium close to it, Low within the golden comparator's tolerance.
// OX_VRS_SWEEP="0.03,0.05,..." additionally prints the metrics for these thresholds (MaxRate 2 and 4).
TEST_F(VrsTest, ScalabilityPresetsKeepGoldenScenesClean) {
    struct Scene {
        const char* name;
        bool refractive;
        CameraParams cam;
    };
    const Scene scenes[] = {
        {"many_point_lights", false, camera({0, 11, 15}, {0, 0, 0}, 5.0f, 50.0f)},
        {"refractive_sphere", true, camera({0.0f, 2.4f, 3.2f}, {0, 0.5f, 0}, 13.5f)},
    };
    struct Size {
        u32 w, h, frames;
    };
    const Size sizes[] = {{256, 256, 4}, {1280, 720, 8}};
    struct Setting {
        std::string label, vrs, threshold, maxRate;
    };
    auto cv = [](const char* n) { return CVarRegistry::instance().find(n); };
    std::vector<Setting> settings;
    for (QualityLevel l : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
        settings.push_back({std::string(scalability::levelName(l)), cv("r.VRS")->levelString(l),
                            cv("r.VRS.Threshold")->levelString(l), cv("r.VRS.MaxRate")->levelString(l)});
    }
    const usize presets = settings.size();
    // VRS is part of the Low and Medium presets only.
    EXPECT_EQ(settings[0].vrs, "true");
    EXPECT_EQ(settings[1].vrs, "true");
    EXPECT_EQ(settings[2].vrs, "false");
    EXPECT_EQ(settings[3].vrs, "false");
    EXPECT_EQ(cv("r.VRS")->defaultString(), "false") << "a bare Renderer must render without VRS";
    if (const char* sweep = std::getenv("OX_VRS_SWEEP")) {
        std::stringstream ss(sweep);
        std::string t;
        while (std::getline(ss, t, ',')) {
            for (const char* m : {"2", "4"}) settings.push_back({"t" + t + "/max" + m, "true", t, m});
        }
    }
    const fs::path out = fs::temp_directory_path() / "oxwald_render_out";
    for (const Scene& sc : scenes) {
        for (const Size& sz : sizes) {
            renderer->destroyView(view); // fresh history per configuration
            view = 0;
            world = std::make_unique<World>();
            if (sc.refractive) refractiveScene();
            else manyLightsScene();
            const Options o{.width = sz.w, .height = sz.h, .frames = sz.frames};
            Image off;
            {
                CVarScope v("r.VRS", "false");
                off = render(sc.cam, o);
            }
            writePng(out / std::format("vrs_{}_{}_off.png", sc.name, sz.w), off);
            for (usize i = 0; i < settings.size(); ++i) {
                const Setting& s = settings[i];
                renderer->destroyView(view);
                view = 0;
                CVarScope v("r.VRS", "true"), t("r.VRS.Threshold", s.threshold), m("r.VRS.MaxRate", s.maxRate);
                const Image img = render(sc.cam, o);
                Quality q = compare(off, img);
                const std::vector<u8> rate = lastRateImage();
                q.coarse = rate.empty() ? 0.0 : f64(coarseTiles(rate)) / f64(rate.size());
                std::printf("[ VRS ] %-18s %4ux%-4u %-12s coarse %5.1f %%  mean err %.3f  worst tile %6.2f  PSNR %.2f  "
                            "SSIM %.4f  FLIP %.4f\n",
                            sc.name, sz.w, sz.h, s.label.c_str(), q.coarse * 100.0, q.meanError, q.worstTile, q.psnr,
                            q.ssim, q.flip);
                writePng(out / std::format("vrs_{}_{}_{}.png", sc.name, sz.w, i), img);
                // The opaque statistics are only combined (and the pass only kept by the graph) with refraction.
                bool analyzedOpaque = false;
                for (const PassTiming& pt : renderer->stats().passes) analyzedOpaque |= pt.name.ends_with("VRS.AnalyzeOpaque");
                EXPECT_EQ(analyzedOpaque, sc.refractive) << sc.name;
                if (i < presets) {
                    // Golden comparator: mean error 2 (of 255). Bounds per level (Low, Medium, High, Ultra), measured
                    // values in docs/dev/modules/render_vrs.md.
                    constexpr f64 kMean[] = {2.0, 1.0, 0.5, 0.5}, kWorst[] = {8.0, 4.0, 3.0, 3.0},
                                  kFlip[] = {0.02, 0.01, 0.006, 0.006};
                    EXPECT_LT(q.meanError, kMean[i]) << sc.name << " " << sz.w << " " << s.label;
                    EXPECT_LT(q.worstTile, kWorst[i]) << sc.name << " " << sz.w << " " << s.label;
                    EXPECT_LT(q.flip, kFlip[i]) << sc.name << " " << sz.w << " " << s.label;
                }
            }
        }
    }
}

// Toggling r.VRS and its mode at runtime rebuilds the graph without leaking textures or buffers.
TEST_F(VrsTest, RuntimeToggleWithoutLeaks) {
    buildScene();
    const CameraParams cam = camera({0, 2.5f, 7}, {0, 0.5f, 0}, 13.0f);
    rhi::GpuMemoryStats first{};
    for (int i = 0; i < 6; ++i) {
        {
            CVarScope v("r.VRS", "true");
            CVarScope m("r.VRS.Mode", i % 2 ? "Motion" : "DebugOverlay");
            render(cam, {.width = 320, .height = 192, .frames = 2});
            EXPECT_TRUE(hasPass("VRS.Generate"));
        }
        {
            CVarScope v("r.VRS", "false");
            render(cam, {.width = 320, .height = 192, .frames = 2});
            EXPECT_FALSE(hasPass("VRS.Generate"));
            EXPECT_TRUE(passesWithShadingRate().empty());
        }
        device->waitIdle();
        if (i == 1) first = device->memoryStats();
    }
    render(cam, {.width = 320, .height = 192, .frames = 4});
    device->waitIdle();
    const rhi::GpuMemoryStats last = device->memoryStats();
    EXPECT_LE(last.textureCount, first.textureCount + 2);
    EXPECT_LE(last.bufferCount, first.bufferCount + 2);
}

// Without the extension (DeviceDesc::enableFragmentShadingRate = false, like MoltenVK) r.VRS is a no-op: no VRS
// passes, pipelines without VRS state, identical image.
TEST_F(RenderTest, VrsNoOpWithoutTheExtension) {
    renderer.reset();
    device.reset();
    rhi::DeviceDesc desc;
    desc.appName = "ox_render_gpu_tests";
    desc.validation = true;
    desc.enableFragmentShadingRate = false;
    desc.shaderOptions.cacheDirectory = fs::temp_directory_path() / "oxwald_render_test_shader_cache";
    std::string error;
    device = rhi::Device::create(desc, &error);
    ASSERT_TRUE(device) << error;
    renderer = Renderer::create(*device);
    EXPECT_FALSE(device->caps().fragmentShadingRate);
    EXPECT_FALSE(vrs::availability(device->caps()).available);
    mesh(Primitive::Sphere, material({0.8f, 0.3f, 0.2f, 1.0f}, 0.0f, 0.4f), {0, 0.5f, 0});
    mesh(Primitive::Plane, material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.6f), {0, 0, 0}, glm::vec3(10.0f));
    sun({-0.4f, -0.8f, -0.45f}, 30000.0f);
    environment();
    const CameraParams cam = camera({0, 2, 5}, {0, 0.5f, 0}, 13.0f);
    Image on, off;
    {
        CVarScope v("r.VRS", "true");
        on = render(cam, {.width = 256, .height = 256, .frames = 3});
        rhi::RenderGraph& g = renderer->view(view)->graph();
        for (u32 i = 0; i < g.passCount(); ++i) {
            EXPECT_FALSE(g.passName(i).starts_with("VRS.")) << g.passName(i);
            EXPECT_FALSE(g.passShadingRate(i).valid()) << g.passName(i);
        }
    }
    {
        CVarScope v("r.VRS", "false");
        renderer->destroyView(view); // same frame history as the first run
        view = 0;
        off = render(cam, {.width = 256, .height = 256, .frames = 3});
    }
    EXPECT_EQ(on.rgba, off.rgba);
}

// With r.VRS off the image is bit-identical to a device without the extension: the VRS pipeline state (flag +
// combiner KEEP/REPLACE) alone changes nothing. Scene: implicit-derivative texture sampling seen through refraction,
// the content that changed most when VRS was accidentally on in the golden tests.
TEST_F(RenderTest, VrsOffMatchesDeviceWithoutTheExtension) {
    auto shot = [&](bool fsr) {
        renderer.reset();
        if (target) device->destroy(target);
        target = {};
        view = 0;
        device.reset();
        rhi::DeviceDesc desc;
        desc.appName = "ox_render_gpu_tests";
        desc.validation = true;
        desc.enableFragmentShadingRate = fsr;
        desc.shaderOptions.cacheDirectory = fs::temp_directory_path() / "oxwald_render_test_shader_cache";
        std::string error;
        device = rhi::Device::create(desc, &error);
        EXPECT_TRUE(device) << error;
        if (!device) return Image{};
        renderer = Renderer::create(*device);
        world = std::make_unique<World>();
        materialCounter = 0;
        // Checker texture (implicit-derivative sampling) seen through a refractive sphere (Translucency.Refractive).
        std::vector<u8> px(64 * 64 * 4);
        for (u32 y = 0; y < 64; ++y) {
            for (u32 x = 0; x < 64; ++x) {
                const u8 c = ((x / 8) + (y / 8)) & 1 ? 230 : 20;
                const usize i = (usize(y) * 64 + x) * 4;
                px[i] = px[i + 1] = px[i + 2] = c;
                px[i + 3] = 255;
            }
        }
        assets::TextureData t;
        t.format = assets::TextureFormat::RGBA8Srgb;
        t.width = t.height = 64;
        t.mipCount = 1;
        assets::TextureMip mip;
        mip.width = mip.height = 64;
        mip.data.resize(px.size());
        std::memcpy(mip.data.data(), px.data(), px.size());
        t.mips.push_back(std::move(mip));
        const Uuid tex = Uuid::fromName("test.vrs.checker");
        renderer->resources().addTexture(tex, t);
        assets::MaterialAsset floor;
        floor.roughness = 0.7f;
        floor.albedoTexture = tex;
        floor.uvTiling = glm::vec2(8.0f);
        const Uuid floorId = Uuid::fromName("test.vrs.floor");
        renderer->resources().addMaterial(floorId, floor);
        assets::MaterialAsset glass;
        glass.roughness = 0.02f;
        glass.blendMode = assets::BlendMode::Refractive;
        glass.ior = 1.5f;
        glass.absorptionColor = {0.45f, 0.9f, 0.55f};
        glass.absorptionDistance = 1.0f;
        const Uuid glassId = Uuid::fromName("test.vrs.glass");
        renderer->resources().addMaterial(glassId, glass);
        mesh(Primitive::Plane, floorId, {0, 0, 0}, glm::vec3(10.0f));
        mesh(Primitive::Sphere, glassId, {0, 0.75f, 0}, glm::vec3(1.5f));
        sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 30000.0f);
        environment();
        CVarScope off("r.VRS", "false");
        return render(camera({0.0f, 2.4f, 3.2f}, {0, 0.5f, 0}, 13.5f), {.width = 256, .height = 256, .frames = 3});
    };
    const Image with = shot(true), without = shot(false);
    ASSERT_FALSE(with.rgba.empty());
    ASSERT_EQ(with.rgba.size(), without.rgba.size());
    usize differing = 0;
    for (usize i = 0; i < with.rgba.size(); ++i) differing += with.rgba[i] != without.rgba[i];
    EXPECT_EQ(differing, 0u) << "r.VRS off must not change the image on a device with VK_KHR_fragment_shading_rate";
}

// Opt-in (OX_VRS_PERF=1, validation off): cost of having the extension enabled with r.VRS off. Same scene on a device
// with the extension and without it (VRS pipeline variants are created only on first use inside a VRS pass, so with
// r.VRS off both devices run the same pipelines).
TEST_F(RenderTest, VrsPipelineStateOverhead) {
    const char* env = std::getenv("OX_VRS_PERF");
    if (!env || std::string(env) == "0") GTEST_SKIP() << "set OX_VRS_PERF=1 and OX_VULKAN_VALIDATION=0";
    auto measure = [&](bool fsr) {
        renderer.reset();
        if (target) device->destroy(target);
        target = {};
        view = 0;
        device.reset();
        rhi::DeviceDesc desc;
        desc.appName = "ox_render_gpu_tests";
        desc.enableFragmentShadingRate = fsr;
        std::string error;
        device = rhi::Device::create(desc, &error);
        renderer = Renderer::create(*device);
        world = std::make_unique<World>();
        materialCounter = 0;
        const Uuid grey = material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.6f), red = material({0.8f, 0.15f, 0.1f, 1.0f}, 0.0f, 0.35f);
        mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(60.0f));
        for (int i = 0; i < 400; ++i) mesh(i % 3 ? Primitive::Sphere : Primitive::Cube, i & 1 ? red : grey,
                                           {f32(i % 20) * 2.0f - 20.0f, 0.6f, -f32(i / 20) * 2.0f}, glm::vec3(1.0f));
        for (int i = 0; i < 32; ++i) pointLight({f32(i % 8) * 5.0f - 20.0f, 2.0f, -f32(i / 8) * 8.0f}, 4000.0f, 10.0f);
        sun({-0.4f, -0.8f, -0.45f}, 30000.0f);
        environment();
        CVarScope off("r.VRS", "false");
        const CameraParams cam = camera({0, 4, 8}, {0, 0, -10}, 13.0f);
        render(cam, {.width = 1920, .height = 1080, .frames = 40});
        std::vector<f64> frame;
        std::map<std::string, std::vector<f64>> passes;
        for (int i = 0; i < 60; ++i) {
            render(cam, {.width = 1920, .height = 1080, .frames = 1});
            frame.push_back(renderer->stats().gpuFrameMs);
            for (const PassTiming& p : renderer->stats().passes) passes[p.name.substr(p.name.find('/') + 1)].push_back(p.gpuMs);
        }
        auto median = [](std::vector<f64> v) {
            if (v.empty()) return 0.0;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        };
        std::string s = std::format("frame {:.3f}", median(frame));
        for (const char* n : {"DepthPrepass", "Shadow.Cascades", "Shadow.Points", "ForwardOpaque", "Sky", "Tonemap"}) {
            s += std::format(", {} {:.3f}", n, median(passes[n]));
        }
        return s;
    };
    for (int round = 0; round < 3; ++round) {
        const std::string a = measure(true), b = measure(false);
        std::printf("[ VRS ] r.VRS off, median ms. VRS-capable pipelines: %s\n[ VRS ]                    plain pipelines: %s\n",
                    a.c_str(), b.c_str());
    }
}
