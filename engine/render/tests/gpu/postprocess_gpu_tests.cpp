// GPU tests of the postprocess-upscalers area (labels render;gpu). Goldens: data/golden/postprocess_*.png.
#include "render_fixture.hpp"

#include <oxwald/render/components/postprocess.hpp>
#include <oxwald/render/features/postprocess/postprocess.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

namespace {

#define EXPECT_GOLDEN(name, img, ...)                                                                                  \
    do {                                                                                                               \
        GoldenResult gr_ = compareGolden(name, img, ##__VA_ARGS__);                                                    \
        EXPECT_TRUE(gr_.matched) << gr_.message << "\n" << asciiArt(img);                                              \
    } while (0)

f64 psnr(const Image& a, const Image& b) {
    f64 se = 0.0;
    const usize n = usize(a.width) * a.height;
    for (usize i = 0; i < n; ++i) {
        for (u32 c = 0; c < 3; ++c) {
            const f64 d = f64(a.rgba[i * 4 + c]) - f64(b.rgba[i * 4 + c]);
            se += d * d;
        }
    }
    const f64 mse = se / (f64(n) * 3.0);
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Box-filtered downsample (supersampled reference).
Image downsample(const Image& big, u32 factor) {
    Image out;
    out.width = big.width / factor;
    out.height = big.height / factor;
    out.rgba.resize(usize(out.width) * out.height * 4);
    for (u32 y = 0; y < out.height; ++y) {
        for (u32 x = 0; x < out.width; ++x) {
            for (u32 c = 0; c < 4; ++c) {
                u32 sum = 0;
                for (u32 j = 0; j < factor; ++j) {
                    for (u32 i = 0; i < factor; ++i) sum += big.rgba[(usize(y * factor + j) * big.width + x * factor + i) * 4 + c];
                }
                out.rgba[(usize(y) * out.width + x) * 4 + c] = u8((sum + factor * factor / 2) / (factor * factor));
            }
        }
    }
    return out;
}

// Mean absolute error (0..255) over the pixels where the reference has edges (the aliasing-prone part).
f64 edgeError(const Image& img, const Image& ref) {
    f64 sum = 0.0;
    u64 count = 0;
    for (u32 y = 1; y + 1 < ref.height; ++y) {
        for (u32 x = 1; x + 1 < ref.width; ++x) {
            const f32 l = ref.luminance(x, y);
            const f32 g = std::abs(ref.luminance(x + 1, y) - l) + std::abs(ref.luminance(x, y + 1) - l) +
                          std::abs(ref.luminance(x - 1, y) - l) + std::abs(ref.luminance(x, y - 1) - l);
            if (g < 0.08f) continue;
            for (u32 c = 0; c < 3; ++c) sum += std::abs(f64(img.at(x, y)[c]) - f64(ref.at(x, y)[c]));
            ++count;
        }
    }
    return count ? sum / (f64(count) * 3.0) : 0.0;
}

f64 meanLuminance(const Image& img) {
    f64 s = 0.0;
    for (u32 y = 0; y < img.height; ++y) {
        for (u32 x = 0; x < img.width; ++x) s += img.luminance(x, y);
    }
    return s / (f64(img.width) * img.height);
}

// Mean absolute horizontal + vertical luminance gradient inside a rect (sharpness measure).
f64 sharpness(const Image& img, u32 x0, u32 y0, u32 x1, u32 y1) {
    f64 s = 0.0;
    u64 n = 0;
    for (u32 y = y0; y < y1; ++y) {
        for (u32 x = x0; x < x1; ++x) {
            s += std::abs(img.luminance(x + 1, y) - img.luminance(x, y)) + std::abs(img.luminance(x, y + 1) - img.luminance(x, y));
            ++n;
        }
    }
    return n ? s / f64(n) : 0.0;
}

class PostProcessTest : public RenderTest {
protected:
    // Like RenderTest::render, with a per-frame snapshot hook and delta time (motion, adaptation).
    Image renderFrames(const CameraParams& cam, const Options& o,
                       const std::function<void(RenderSnapshot&, u32)>& perFrame = {}, f32 deltaTime = 0.0f,
                       bool keepView = false) {
        ensureTarget(o.width, o.height, o.format);
        if (!keepView && view) {
            renderer->destroyView(view);
            view = 0;
        }
        ensureView(o.editor);
        renderer->resources().flush();
        for (u32 f = 0; f < o.frames; ++f) {
            extractSnapshot();
            snapshot.deltaTime = deltaTime;
            if (perFrame) perFrame(snapshot, f);
            device->beginFrame();
            renderer->beginFrame(snapshot);
            ViewRenderRequest req;
            req.view = view;
            req.camera = cam;
            req.target.texture = target;
            req.target.finalAccess = rhi::Access::TransferRead;
            renderer->renderView(req);
            renderer->endFrame();
            device->endFrame();
        }
        device->waitIdle();
        Image img;
        img.width = o.width;
        img.height = o.height;
        img.rgba = device->readTexture(target);
        for (usize i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
        return img;
    }

    // GPU time of a render graph pass in the last retired frame; < 0 when it did not run.
    f64 passMs(std::string_view pass) const {
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.ends_with(std::string("/") + std::string(pass))) return p.gpuMs;
        }
        return -1.0;
    }
    bool ranPass(std::string_view pass) const { return passMs(pass) >= 0.0; }

    // Image for inspection only (no golden: e.g. DLSS output depends on the driver's model version).
    static void writeOut(const std::string& name, const Image& img) {
        writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / (name + ".png"), img);
    }

    // Replaces the fixture's device + renderer; `upscalerExtensions` = with the NGX (DLSS) extensions, as SetUp does.
    void recreateDevice(bool upscalerExtensions) {
        device->waitIdle();
        renderer.reset();
        if (target) device->destroy(target);
        device.reset();
        target = {};
        view = 0;
        rhi::DeviceDesc desc;
        desc.appName = "ox_render_gpu_tests";
        desc.validation = true;
        desc.shaderOptions.cacheDirectory = std::filesystem::temp_directory_path() / "oxwald_render_test_shader_cache";
        if (upscalerExtensions) appendUpscalerVulkanExtensions(desc);
        device = rhi::Device::create(desc);
        ASSERT_TRUE(device);
        renderer = Renderer::create(*device);
    }

    Entity volume(const PostProcessSettings& s) {
        Entity e = world->create("PostProcessVolume");
        auto& v = e.add<PostProcessVolumeComponent>();
        v.unbound = true;
        v.settings = s;
        return e;
    }

    Uuid checkerTexture() {
        assets::TextureData t;
        t.format = assets::TextureFormat::RGBA8Srgb;
        t.width = t.height = 64;
        t.mipCount = 1;
        assets::TextureMip mip;
        mip.width = mip.height = 64;
        mip.data.resize(64 * 64 * 4);
        for (u32 y = 0; y < 64; ++y) {
            for (u32 x = 0; x < 64; ++x) {
                const bool on = ((x / 8) + (y / 8)) % 2 == 0;
                for (u32 c = 0; c < 3; ++c) mip.data[(y * 64 + x) * 4 + c] = std::byte(on ? 230 : 25);
                mip.data[(y * 64 + x) * 4 + 3] = std::byte(255);
            }
        }
        t.mips.push_back(std::move(mip));
        const Uuid id = Uuid::fromName("test.pp.checker");
        renderer->resources().addTexture(id, t);
        return id;
    }

    Uuid texturedMaterial(const Uuid& albedo) {
        assets::MaterialAsset m;
        m.baseColor = glm::vec4(1.0f);
        m.roughness = 0.8f;
        m.albedoTexture = albedo;
        const Uuid id = Uuid::fromName(std::format("test.pp.material.{}", materialCounter++));
        renderer->resources().addMaterial(id, m);
        return id;
    }

    // Thin bright bars and wires at various angles: the aliasing stress scene.
    void thinGeometryScene() {
        const Uuid bar = material({0.95f, 0.9f, 0.8f, 1.0f}, 0.0f, 0.5f);
        const Uuid ground = material({0.08f, 0.09f, 0.1f, 1.0f}, 0.0f, 0.9f);
        mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(40.0f));
        for (int i = 0; i < 7; ++i) {
            const f32 a = glm::radians(-50.0f + 15.0f * f32(i));
            mesh(Primitive::Cube, bar, {-3.0f + f32(i), 1.5f, 0.0f}, {0.03f, 3.0f, 0.03f},
                 glm::angleAxis(a, glm::vec3(0, 0, 1)));
        }
        for (int i = 0; i < 4; ++i) {
            mesh(Primitive::Cube, bar, {0.0f, 0.6f + 0.35f * f32(i), 0.5f}, {8.0f, 0.015f, 0.015f},
                 glm::angleAxis(glm::radians(3.0f + 4.0f * f32(i)), glm::vec3(0, 0, 1)));
        }
        sun({-0.3f, -0.8f, -0.5f}, 20000.0f, glm::vec3(1.0f), false);
        environment(0.5f, 1.0f);
    }
    CameraParams thinCamera() { return camera({0, 1.6f, 6.0f}, {0, 1.5f, 0}, 12.5f, 50.0f); }

    void colorfulScene() {
        const Uuid ground = material({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f);
        mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(30.0f));
        const glm::vec4 colors[5] = {{0.9f, 0.1f, 0.1f, 1}, {0.1f, 0.8f, 0.2f, 1}, {0.1f, 0.2f, 0.9f, 1},
                                     {0.9f, 0.8f, 0.1f, 1}, {0.8f, 0.2f, 0.8f, 1}};
        for (int i = 0; i < 5; ++i) mesh(Primitive::Sphere, material(colors[i], 0.0f, 0.4f), {-2.4f + 1.2f * f32(i), 0.5f, 0}, glm::vec3(1.0f));
        sun({-0.4f, -0.7f, -0.6f}, 20000.0f, glm::vec3(1.0f), true);
        environment(1.0f, 1.0f);
    }
    CameraParams colorfulCamera() { return camera({0, 2.0f, 5.0f}, {0, 0.4f, 0}, 12.5f, 50.0f); }
};

} // namespace

// --- anti-aliasing -------------------------------------------------------------------------------------------

TEST_F(PostProcessTest, TaaVersusNoAaOnThinGeometry) {
    thinGeometryScene();
    const CameraParams cam = thinCamera();
    const Image reference = downsample(renderFrames(cam, {.width = 1024, .height = 1024, .frames = 1}), 4);
    const Image none = renderFrames(cam, {.frames = 2});
    Image taa, fxaa;
    {
        CVarScope aa("r.AntiAliasing", "2");
        taa = renderFrames(cam, {.frames = 24});
    }
    {
        CVarScope aa("r.AntiAliasing", "1");
        fxaa = renderFrames(cam, {.frames = 2});
    }
    const f64 eNone = edgeError(none, reference), eTaa = edgeError(taa, reference), eFxaa = edgeError(fxaa, reference);
    std::printf("thin geometry edge error vs 16x SSAA: none %.2f, FXAA %.2f, TAA %.2f; PSNR none %.2f FXAA %.2f TAA %.2f dB\n",
                eNone, eFxaa, eTaa, psnr(none, reference), psnr(fxaa, reference), psnr(taa, reference));
    EXPECT_LT(eTaa, eNone * 0.7) << "TAA should approach the supersampled reference on thin geometry";
    EXPECT_LT(eFxaa, eNone);
    EXPECT_GT(psnr(taa, reference), psnr(none, reference) + 1.0);
    EXPECT_GOLDEN("postprocess_taa_thin", taa);
    EXPECT_GOLDEN("postprocess_noaa_thin", none);
    EXPECT_GOLDEN("postprocess_fxaa_thin", fxaa);
}

TEST_F(PostProcessTest, TaaRejectsHistoryOfMovingObject) {
    // A bright object moves across: no ghost trail may stay behind at its start position.
    const Uuid ground = material({0.1f, 0.1f, 0.1f, 1.0f}, 0.0f, 0.9f);
    const Uuid bright = material({0.9f, 0.9f, 0.9f, 1.0f}, 0.0f, 0.5f, glm::vec3(4.0f));
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(40.0f));
    Entity mover = mesh(Primitive::Cube, bright, {-2.0f, 0.5f, 0}, glm::vec3(1.0f));
    environment(0.3f, 1.0f);
    CVarScope aa("r.AntiAliasing", "2");
    const CameraParams cam = camera({0, 2.0f, 6.0f}, {0, 0.5f, 0}, 10.0f, 50.0f);
    auto xs = [](u32 f) { return f < 12 ? -2.0f : -2.0f + 0.25f * f32(f - 11); };
    const u32 moverId = encodeEntityId(u32(entt::to_integral(mover.handle())));
    const Image img = renderFrames(cam, {.frames = 20}, [&](RenderSnapshot& s, u32 f) {
        // 12 frames still at x = -2, then 0.25 m per frame to the right (the velocity buffer covers it).
        for (SnapshotMesh& m : s.meshes) {
            if (m.entityId != moverId) continue;
            m.world = glm::translate(glm::mat4(1.0f), {xs(f), 0.5f, 0});
            m.prevWorld = glm::translate(glm::mat4(1.0f), {xs(f == 0 ? 0 : f - 1), 0.5f, 0});
        }
    });
    // Old position (x=-2) is far from the final one (x=0): background there must be dark again.
    const glm::vec2 oldUv(0.5f - 0.27f, 0.62f);
    const f32 oldLum = img.luminance(u32(oldUv.x * 256), u32(oldUv.y * 256));
    EXPECT_LT(oldLum, 0.35f) << asciiArt(img);
    EXPECT_GOLDEN("postprocess_taa_moving", img);
}

// --- upscalers -----------------------------------------------------------------------------------------------

TEST_F(PostProcessTest, Fsr1HalfResolutionVersusNative) {
    colorfulScene();
    const CameraParams cam = colorfulCamera();
    // FSR 1 expects anti-aliased input: TAA runs at render resolution in every variant.
    CVarScope aa("r.AntiAliasing", "2");
    const Image native = renderFrames(cam, {.frames = 16});
    Image fsr, bilinear;
    {
        CVarScope up("r.Upscaler", "FSR1");
        CVarScope q("r.Upscaler.Quality", "Performance"); // 50 %
        fsr = renderFrames(cam, {.frames = 16});
    }
    {
        CVarScope sp("r.ScreenPercentage", "50");
        bilinear = renderFrames(cam, {.frames = 16});
    }
    EXPECT_GOLDEN("postprocess_bilinear_50", bilinear);
    const f64 pFsr = psnr(fsr, native), pBilinear = psnr(bilinear, native);
    std::printf("FSR1 50%% PSNR vs native: %.2f dB (bilinear %.2f dB)\n", pFsr, pBilinear);
    EXPECT_GT(pFsr, 28.0);
    EXPECT_GT(pFsr, pBilinear);
    EXPECT_GOLDEN("postprocess_fsr1_50", fsr);
    const RenderStats& s = renderer->stats();
    (void)s;
}

TEST_F(PostProcessTest, TaauHalfResolutionConverges) {
    thinGeometryScene();
    const CameraParams cam = thinCamera();
    const Image reference = downsample(renderFrames(cam, {.width = 1024, .height = 1024, .frames = 1}), 4);
    Image taau, bilinear;
    {
        CVarScope up("r.Upscaler", "TAAU");
        CVarScope q("r.Upscaler.Quality", "Performance");
        taau = renderFrames(cam, {.frames = 40});
    }
    {
        CVarScope sp("r.ScreenPercentage", "50");
        bilinear = renderFrames(cam, {.frames = 2});
    }
    const f64 pTaau = psnr(taau, reference), pBilinear = psnr(bilinear, reference);
    std::printf("TAAU 50%% PSNR vs 16x SSAA: %.2f dB (bilinear 50%%: %.2f dB)\n", pTaau, pBilinear);
    EXPECT_GT(pTaau, pBilinear + 1.0);
    EXPECT_GOLDEN("postprocess_taau_50", taau);
}

TEST_F(PostProcessTest, DlssSelectionFallsBackToTaauWhereUnavailable) {
    UpscalerAvailability a = upscalerAvailability(UpscalerType::DLSS, device.get());
    if (a.available) {
        // RTX machine: the same GPU through a device created without the NGX extensions must report DLSS unavailable
        // up front (NGX is not called: no validation errors) and fall back like any other GPU.
        recreateDevice(false);
        a = upscalerAvailability(UpscalerType::DLSS, device.get());
        EXPECT_NE(a.reason.find("appendUpscalerVulkanExtensions"), std::string::npos) << a.reason;
    }
    EXPECT_FALSE(a.available);
    EXPECT_FALSE(a.reason.empty());
    std::printf("DLSS unavailable: %s\n", a.reason.c_str());
    colorfulScene();
    CVarScope up("r.Upscaler", "DLSS");
    CVarScope q("r.Upscaler.Quality", "Performance");
    const Image img = renderFrames(colorfulCamera(), {.frames = 4});
    EXPECT_GT(meanLuminance(img), 0.1);
    EXPECT_TRUE(ranPass("TAAU")) << "r.Upscaler=DLSS without DLSS must run TAAU";
    EXPECT_FALSE(ranPass("DLSS"));
}

TEST_F(PostProcessTest, DlssRendersOnRtx) {
    const UpscalerAvailability a = upscalerAvailability(UpscalerType::DLSS, device.get());
    if (!a.available) GTEST_SKIP() << "DLSS unavailable: " << a.reason;
    colorfulScene();
    const CameraParams cam = colorfulCamera();
    const Options o{.width = 512, .height = 512, .frames = 32};
    const Image native = renderFrames(cam, {.width = 512, .height = 512, .frames = 2});
    Image taau;
    {
        CVarScope up("r.Upscaler", "TAAU");
        CVarScope q("r.Upscaler.Quality", "Quality");
        taau = renderFrames(cam, o);
    }
    CVarScope up("r.Upscaler", "DLSS");
    CVarScope q("r.Upscaler.Quality", "Quality");
    const Image img = renderFrames(cam, o);
    EXPECT_TRUE(ranPass("DLSS"));
    EXPECT_FALSE(ranPass("TAAU"));
    writeOut("postprocess_dlss_quality", img);
    writeOut("postprocess_dlss_reference_native", native);
    writeOut("postprocess_dlss_reference_taau_quality", taau);
    const f64 pDlss = psnr(img, native), pTaau = psnr(taau, native);
    std::printf("DLSS Quality (67 %%) PSNR vs native: %.2f dB (TAAU 67 %%: %.2f dB), mean luminance %.4f (native %.4f), "
                "DLSS pass %.3f ms\n", pDlss, pTaau, meanLuminance(img), meanLuminance(native), passMs("DLSS"));
    EXPECT_GT(pDlss, 35.0) << "not the native image: flipped, shifted or badly exposed?";
    EXPECT_NEAR(meanLuminance(img), meanLuminance(native), 0.01);
}

TEST_F(PostProcessTest, DlssQualityModesSetRenderResolutionAndMipBias) {
    const UpscalerAvailability a = upscalerAvailability(UpscalerType::DLSS, device.get());
    if (!a.available) GTEST_SKIP() << "DLSS unavailable: " << a.reason;
    colorfulScene();
    const CameraParams cam = colorfulCamera();
    const Image native = renderFrames(cam, {.width = 960, .height = 540, .frames = 2});
    struct Mode {
        const char* name;
        f32 scale;
    };
    const Mode modes[] = {{"UltraPerformance", 1.0f / 3.0f}, {"Performance", 0.5f}, {"Balanced", 0.58f},
                          {"Quality", 2.0f / 3.0f},          {"Native", 1.0f}};
    CVarScope up("r.Upscaler", "DLSS");
    bool keep = false; // one view for all modes: the NGX feature is recreated on every mode change
    auto check = [&](const Mode& m, u32 width, u32 height) {
        const Image img = renderFrames(cam, {.width = width, .height = height, .frames = 16}, {}, 0.0f, keep);
        keep = true;
        const RenderView* v = renderer->view(view);
        ASSERT_NE(v, nullptr);
        const Extent2D re = v->renderExtent();
        std::printf("DLSS %-16s %ux%u -> %ux%u (%.1f %%), mip bias %.3f, DLSS pass %.3f ms\n", m.name, re.width,
                    re.height, width, height, 100.0f * f32(re.width) / f32(width), v->mipBias(), passMs("DLSS"));
        EXPECT_EQ(v->outputExtent().width, width);
        EXPECT_EQ(v->outputExtent().height, height);
        EXPECT_NEAR(f32(re.width), f32(width) * m.scale, 1.5f) << m.name;
        EXPECT_NEAR(f32(re.height), f32(height) * m.scale, 1.5f) << m.name;
        EXPECT_NEAR(v->mipBias(), std::log2(f32(re.width) / f32(width)) - 1.0f, 1e-3f) << m.name;
        EXPECT_TRUE(ranPass("DLSS")) << m.name;
        EXPECT_FALSE(ranPass("TAAU")) << m.name;
        if (width == native.width) {
            EXPECT_GT(psnr(img, native), 27.0) << m.name;
            EXPECT_NEAR(meanLuminance(img), meanLuminance(native), 0.015) << m.name;
            writeOut(std::format("postprocess_dlss_mode_{}", m.name), img);
        }
    };
    for (const Mode& m : modes) {
        CVarScope q("r.Upscaler.Quality", m.name);
        check(m, 960, 540);
    }
    // Output resize with the view (and its NGX feature) alive.
    CVarScope q("r.Upscaler.Quality", "Quality");
    check(modes[3], 640, 360);
    check(modes[3], 960, 540);
}

TEST_F(PostProcessTest, DlssStaticSceneConvergesAndStaysStable) {
    const UpscalerAvailability a = upscalerAvailability(UpscalerType::DLSS, device.get());
    if (!a.available) GTEST_SKIP() << "DLSS unavailable: " << a.reason;
    // Jitter convention check: with a wrong jitter sign (or scale) the accumulated image of a static scene shimmers
    // from frame to frame and thin features blur instead of resolving towards the supersampled reference.
    thinGeometryScene();
    const CameraParams cam = thinCamera();
    const Image reference = downsample(renderFrames(cam, {.width = 2048, .height = 2048, .frames = 1}), 4);
    Image bilinear, taau;
    {
        CVarScope sp("r.ScreenPercentage", "50");
        bilinear = renderFrames(cam, {.width = 512, .height = 512, .frames = 2});
    }
    {
        CVarScope up("r.Upscaler", "TAAU");
        CVarScope q("r.Upscaler.Quality", "Performance");
        taau = renderFrames(cam, {.width = 512, .height = 512, .frames = 48});
    }
    CVarScope up("r.Upscaler", "DLSS");
    CVarScope q("r.Upscaler.Quality", "Performance");
    const Image previous = renderFrames(cam, {.width = 512, .height = 512, .frames = 47});
    const Image dlss = renderFrames(cam, {.width = 512, .height = 512, .frames = 1}, {}, 0.0f, true); // 48th frame
    writeOut("postprocess_dlss_static_performance", dlss);
    writeOut("postprocess_dlss_static_reference_ssaa", reference);
    writeOut("postprocess_dlss_static_reference_taau", taau);
    writeOut("postprocess_dlss_static_reference_bilinear", bilinear);
    const f64 pDlss = psnr(dlss, reference), pTaau = psnr(taau, reference), pBilinear = psnr(bilinear, reference);
    const f64 stability = psnr(dlss, previous);
    std::printf("DLSS 50%% static: PSNR vs 16x SSAA %.2f dB (TAAU %.2f, bilinear %.2f), edge error %.2f (TAAU %.2f, "
                "bilinear %.2f), frame 47 vs 48: %.2f dB\n", pDlss, pTaau, pBilinear, edgeError(dlss, reference),
                edgeError(taau, reference), edgeError(bilinear, reference), stability);
    EXPECT_GT(stability, 40.0) << "a converged static image must not shimmer";
    EXPECT_GT(pDlss, pBilinear + 1.0);
    EXPECT_LT(edgeError(dlss, reference), edgeError(bilinear, reference));
}

TEST_F(PostProcessTest, DlssFollowsCameraMotion) {
    const UpscalerAvailability a = upscalerAvailability(UpscalerType::DLSS, device.get());
    if (!a.available) GTEST_SKIP() << "DLSS unavailable: " << a.reason;
    // Motion vector convention check: a camera moving diagonally (right and up) past textured boxes. Wrong motion
    // vectors (sign of either axis, scale, space) reproject the history to the wrong place: the frame smears and falls
    // far below TAAU, which uses the same velocity buffer.
    const Uuid checker = texturedMaterial(checkerTexture());
    mesh(Primitive::Plane, material({0.3f, 0.3f, 0.3f, 1.0f}, 0.0f, 0.9f), {0, 0, 0}, glm::vec3(40.0f));
    for (int i = 0; i < 5; ++i) mesh(Primitive::Cube, checker, {-4.0f + 2.0f * f32(i), 0.5f, -f32(i % 2)}, glm::vec3(1.0f));
    sun({-0.4f, -0.7f, -0.6f}, 20000.0f, glm::vec3(1.0f), false);
    environment(1.0f, 1.0f);
    const u32 still = 24, moving = 16;
    auto cameraAt = [&](u32 f) {
        const f32 d = f < still ? 0.0f : 0.075f * f32(f - still + 1); // 0.075 m per frame ≈ 7 output pixels
        return camera({-0.6f + d, 1.5f + 0.6f * d, 5.0f}, {-0.6f + d, 0.5f + 0.6f * d, 0.0f}, 12.5f, 50.0f);
    };
    const CameraParams last = cameraAt(still + moving - 1);
    const Image reference = downsample(renderFrames(last, {.width = 2048, .height = 2048, .frames = 1}), 4);
    auto sequence = [&] {
        Image img;
        for (u32 f = 0; f < still + moving; ++f) {
            img = renderFrames(cameraAt(f), {.width = 512, .height = 512, .frames = 1}, {}, 1.0f / 60.0f, f > 0);
        }
        return img;
    };
    Image taau, dlss;
    {
        CVarScope up("r.Upscaler", "TAAU");
        CVarScope q("r.Upscaler.Quality", "Quality");
        taau = sequence();
    }
    {
        CVarScope up("r.Upscaler", "DLSS");
        CVarScope q("r.Upscaler.Quality", "Quality");
        dlss = sequence();
    }
    writeOut("postprocess_dlss_moving_quality", dlss);
    writeOut("postprocess_dlss_moving_reference_ssaa", reference);
    writeOut("postprocess_dlss_moving_reference_taau", taau);
    const f64 pDlss = psnr(dlss, reference), pTaau = psnr(taau, reference);
    std::printf("moving camera, last frame vs 16x SSAA: DLSS %.2f dB, TAAU %.2f dB; checker sharpness DLSS %.4f, TAAU "
                "%.4f, SSAA %.4f\n", pDlss, pTaau, sharpness(dlss, 64, 200, 448, 330), sharpness(taau, 64, 200, 448, 330),
                sharpness(reference, 64, 200, 448, 330));
    EXPECT_GT(pDlss, pTaau - 1.0) << "DLSS smears under camera motion more than TAAU";
}

TEST_F(PostProcessTest, DlssWithAutoExposureAndPreExposure) {
    const UpscalerAvailability a = upscalerAvailability(UpscalerType::DLSS, device.get());
    if (!a.available) GTEST_SKIP() << "DLSS unavailable: " << a.reason;
    // r.Exposure.Auto: SceneColorHDR is pre-exposed and DLSS gets the engine's Exposure texture + InPreExposure
    // instead of its own auto exposure. A wrong exposure scale shows as ghosting / lost detail.
    thinGeometryScene();
    const CameraParams cam = thinCamera();
    CVarScope ae("r.Exposure.Auto", "true");
    const f32 dt = 1.0f / 30.0f;
    const Image reference = downsample(renderFrames(cam, {.width = 2048, .height = 2048, .frames = 60}, {}, dt), 4);
    Image taau, dlss;
    {
        CVarScope up("r.Upscaler", "TAAU");
        CVarScope q("r.Upscaler.Quality", "Quality");
        taau = renderFrames(cam, {.width = 512, .height = 512, .frames = 60}, {}, dt);
    }
    {
        CVarScope up("r.Upscaler", "DLSS");
        CVarScope q("r.Upscaler.Quality", "Quality");
        dlss = renderFrames(cam, {.width = 512, .height = 512, .frames = 60}, {}, dt);
        EXPECT_TRUE(ranPass("DLSS"));
    }
    writeOut("postprocess_dlss_auto_exposure", dlss);
    writeOut("postprocess_dlss_auto_exposure_reference_ssaa", reference);
    writeOut("postprocess_dlss_auto_exposure_reference_taau", taau);
    const f64 pDlss = psnr(dlss, reference), pTaau = psnr(taau, reference);
    std::printf("auto exposure, 67%%: PSNR vs 16x SSAA DLSS %.2f dB, TAAU %.2f dB; mean luminance DLSS %.4f, "
                "TAAU %.4f, SSAA %.4f\n", pDlss, pTaau, meanLuminance(dlss), meanLuminance(taau), meanLuminance(reference));
    EXPECT_NEAR(meanLuminance(dlss), meanLuminance(reference), 0.02);
    EXPECT_GT(pDlss, pTaau - 2.0);
}

TEST_F(PostProcessTest, DlssSurvivesRendererAndDeviceRecreation) {
    if (!upscalerAvailability(UpscalerType::DLSS, device.get()).available) GTEST_SKIP() << "DLSS unavailable";
    // NGX lives with the device (shutdown callback), features with the renderer: neither order may leak or crash.
    CVarScope up("r.Upscaler", "DLSS");
    CVarScope q("r.Upscaler.Quality", "Balanced");
    auto renderOnce = [&] {
        colorfulScene();
        const Image img = renderFrames(colorfulCamera(), {.frames = 4});
        EXPECT_GT(meanLuminance(img), 0.1);
        EXPECT_TRUE(ranPass("DLSS"));
    };
    renderOnce();
    // A second renderer on the same device reuses the initialised NGX.
    device->waitIdle();
    renderer.reset();
    world = std::make_unique<World>();
    renderer = Renderer::create(*device);
    view = 0;
    renderOnce();
    // A new device: NGX is shut down with the old one and initialised again.
    world = std::make_unique<World>();
    recreateDevice(true);
    ASSERT_TRUE(upscalerAvailability(UpscalerType::DLSS, device.get()).available);
    renderOnce();
    // Probing only (no renderer ever uses DLSS on this device) must not leak NGX objects either: TearDown checks.
    world = std::make_unique<World>();
    recreateDevice(true);
    EXPECT_TRUE(upscalerAvailability(UpscalerType::DLSS, device.get()).available);
}

// --- post effects --------------------------------------------------------------------------------------------

TEST_F(PostProcessTest, BloomOnEmissives) {
    const Uuid ground = material({0.05f, 0.05f, 0.05f, 1.0f}, 0.0f, 0.9f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(30.0f));
    mesh(Primitive::Sphere, material({0.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 0.5f, glm::vec3(30.0f, 12.0f, 3.0f)), {-1.2f, 0.6f, 0}, glm::vec3(0.6f));
    mesh(Primitive::Sphere, material({0.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 0.5f, glm::vec3(3.0f, 10.0f, 40.0f)), {1.2f, 0.6f, 0}, glm::vec3(0.6f));
    environment(0.02f, 0.02f);
    const CameraParams cam = camera({0, 1.2f, 5.0f}, {0, 0.5f, 0}, 10.0f, 45.0f);
    const Image off = renderFrames(cam, {.frames = 2});
    PostProcessSettings s;
    s.overrideBloom = true;
    s.bloomIntensity = 0.15f;
    volume(s);
    CVarScope bloom("r.Bloom", "true");
    const Image on = renderFrames(cam, {.frames = 2});
    // Pixel between the spheres (dark without bloom) glows with bloom.
    const u32 cx = 128, cy = 150;
    std::printf("bloom: between spheres %.3f → %.3f\n", off.luminance(cx, cy), on.luminance(cx, cy));
    EXPECT_GT(on.luminance(cx, cy), off.luminance(cx, cy) + 0.03f);
    EXPECT_GT(meanLuminance(on), meanLuminance(off));
    EXPECT_GOLDEN("postprocess_bloom", on);
}

TEST_F(PostProcessTest, DepthOfFieldNearAndFar) {
    const Uuid checker = texturedMaterial(checkerTexture());
    const Uuid ground = material({0.3f, 0.3f, 0.3f, 1.0f}, 0.0f, 0.9f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(80.0f));
    mesh(Primitive::Cube, checker, {-1.0f, 0.5f, 3.0f}, glm::vec3(0.8f));  // near (≈2.5 m)
    mesh(Primitive::Cube, checker, {0.4f, 0.8f, 0.0f}, glm::vec3(1.4f));   // focus (≈5.5 m)
    mesh(Primitive::Cube, checker, {2.5f, 2.0f, -14.0f}, glm::vec3(4.0f)); // far (≈20 m)
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f, glm::vec3(1.0f), false);
    environment(1.0f, 1.0f);
    const CameraParams cam = camera({0, 1.2f, 5.5f}, {0.3f, 1.0f, 0}, 12.5f, 50.0f);
    const Image sharp = renderFrames(cam, {.frames = 2});
    PostProcessSettings s;
    s.overrideDepthOfField = true;
    s.focusDistance = 5.5f;
    s.aperture = 1.4f;
    s.focalLength = 85.0f;
    s.maxBokehSize = 2.0f;
    volume(s);
    CVarScope dof("r.DepthOfField", "true");
    const Image img = renderFrames(cam, {.frames = 2});
    // Regions (found from the image): near cube left-bottom, focus cube centre, far cube upper right.
    const f64 nearBefore = sharpness(sharp, 20, 175, 75, 235), nearAfter = sharpness(img, 20, 175, 75, 235);
    const f64 focusBefore = sharpness(sharp, 120, 110, 160, 150), focusAfter = sharpness(img, 120, 110, 160, 150);
    const f64 farBefore = sharpness(sharp, 165, 75, 200, 100), farAfter = sharpness(img, 165, 75, 200, 100);
    std::printf("DOF sharpness near %.4f→%.4f focus %.4f→%.4f far %.4f→%.4f\n", nearBefore, nearAfter, focusBefore,
                focusAfter, farBefore, farAfter);
    EXPECT_LT(nearAfter, nearBefore * 0.6);
    EXPECT_LT(farAfter, farBefore * 0.7);
    EXPECT_GT(focusAfter, focusBefore * 0.8);
    EXPECT_GOLDEN("postprocess_dof", img);
    EXPECT_GOLDEN("postprocess_dof_reference", sharp);
}

TEST_F(PostProcessTest, MotionBlurOnMovingObject) {
    const Uuid ground = material({0.2f, 0.2f, 0.2f, 1.0f}, 0.0f, 0.9f);
    const Uuid red = material({0.9f, 0.15f, 0.1f, 1.0f}, 0.0f, 0.5f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(40.0f));
    Entity mover = mesh(Primitive::Cube, red, {0, 0.8f, 0}, glm::vec3(1.0f));
    mesh(Primitive::Cube, red, {-2.5f, 0.8f, 0}, glm::vec3(1.0f)); // static reference cube
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f, glm::vec3(1.0f), false);
    environment(1.0f, 1.0f);
    const CameraParams cam = camera({-1.0f, 1.5f, 6.0f}, {-1.0f, 0.8f, 0}, 12.5f, 50.0f);
    const u32 moverId = encodeEntityId(u32(entt::to_integral(mover.handle())));
    auto move = [&](RenderSnapshot& s, u32) {
        for (SnapshotMesh& m : s.meshes) {
            if (m.entityId == moverId) m.prevWorld = glm::translate(glm::mat4(1.0f), {-0.6f, 0.0f, 0.0f}) * m.world;
        }
    };
    const Image still = renderFrames(cam, {.frames = 3}, move);
    CVarScope mb("r.MotionBlur", "true");
    const Image img = renderFrames(cam, {.frames = 3}, move);
    // Horizontal transition width at the cubes' row: pixels whose redness lies between background and cube.
    auto transition = [](const Image& im, u32 row, u32 x0, u32 x1) {
        auto redness = [&](u32 x) {
            const glm::u8vec4 c = im.at(x, row);
            return (f32(c.r) - f32(c.g)) / 255.0f;
        };
        f32 inside = 0.0f;
        for (u32 x = x0; x < x1; ++x) inside = std::max(inside, redness(x));
        u32 n = 0;
        for (u32 x = x0; x < x1; ++x) {
            const f32 r = redness(x) / std::max(inside, 1e-3f);
            if (r > 0.1f && r < 0.85f) ++n;
        }
        return n;
    };
    const u32 row = 130;
    const u32 movingBefore = transition(still, row, 120, 256), movingAfter = transition(img, row, 120, 256);
    const u32 staticBefore = transition(still, row, 0, 110), staticAfter = transition(img, row, 0, 110);
    std::printf("motion blur transition px: moving %u→%u, static %u→%u\n", movingBefore, movingAfter, staticBefore,
                staticAfter);
    EXPECT_GE(movingAfter, movingBefore + 5);
    EXPECT_LE(staticAfter, staticBefore + 2);
    EXPECT_GOLDEN("postprocess_motion_blur", img);
}

TEST_F(PostProcessTest, ColorGradingLut) {
    colorfulScene();
    const CameraParams cam = colorfulCamera();
    const Image base = renderFrames(cam, {.frames = 2});
    {
        PostProcessSettings s;
        s.overrideGrading = true;
        s.saturation = 0.0f;
        Entity v = volume(s);
        const Image grey = renderFrames(cam, {.frames = 2});
        u32 maxSpread = 0;
        for (u32 y = 0; y < grey.height; ++y) {
            for (u32 x = 0; x < grey.width; ++x) {
                const glm::u8vec4 c = grey.at(x, y);
                maxSpread = std::max<u32>(maxSpread, std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}));
            }
        }
        EXPECT_LE(maxSpread, 6u) << "saturation 0 must produce a grey image";
        world->destroyImmediate(v);
    }
    PostProcessSettings s;
    s.overrideWhiteBalance = true;
    s.temperature = 5200.0f; // warm illuminant corrected → cooler image
    s.overrideGrading = true;
    s.saturation = 1.3f;
    s.contrast = 1.2f;
    s.lift = glm::vec3(0.0f, 0.0f, 0.004f);
    s.gain = glm::vec3(1.04f, 1.0f, 0.97f);
    volume(s);
    const Image graded = renderFrames(cam, {.frames = 2});
    auto channelMean = [](const Image& im, u32 c) {
        f64 s = 0;
        for (usize i = 0; i < usize(im.width) * im.height; ++i) s += im.rgba[i * 4 + c];
        return s / (f64(im.width) * im.height);
    };
    const f64 baseBR = channelMean(base, 2) - channelMean(base, 0);
    const f64 gradedBR = channelMean(graded, 2) - channelMean(graded, 0);
    std::printf("grading: mean(B-R) %.2f → %.2f\n", baseBR, gradedBR);
    EXPECT_GT(gradedBR, baseBR + 3.0);
    EXPECT_GOLDEN("postprocess_grading", graded);
}

TEST_F(PostProcessTest, UserLutTextureIsApplied) {
    // 16³ strip LUT that inverts the display colour.
    constexpr u32 N = 16;
    assets::TextureData t;
    t.format = assets::TextureFormat::RGBA8Unorm;
    t.width = N * N;
    t.height = N;
    t.mipCount = 1;
    t.filter = assets::TextureFilter::Linear;
    assets::TextureMip mip;
    mip.width = N * N;
    mip.height = N;
    mip.data.resize(usize(N) * N * N * 4);
    for (u32 b = 0; b < N; ++b) {
        for (u32 g = 0; g < N; ++g) {
            for (u32 r = 0; r < N; ++r) {
                const usize i = (usize(g) * N * N + b * N + r) * 4;
                mip.data[i + 0] = std::byte(255 - r * 255 / (N - 1));
                mip.data[i + 1] = std::byte(255 - g * 255 / (N - 1));
                mip.data[i + 2] = std::byte(255 - b * 255 / (N - 1));
                mip.data[i + 3] = std::byte(255);
            }
        }
    }
    t.mips.push_back(std::move(mip));
    const Uuid lut = Uuid::fromName("test.pp.lut.invert");
    renderer->resources().addTexture(lut, t);
    colorfulScene();
    const CameraParams cam = colorfulCamera();
    const Image base = renderFrames(cam, {.frames = 2});
    PostProcessSettings s;
    s.overrideLut = true;
    s.lutTexture = lut;
    s.lutIntensity = 1.0f;
    volume(s);
    const Image inverted = renderFrames(cam, {.frames = 2});
    f64 err = 0.0;
    for (usize i = 0; i < usize(base.width) * base.height; ++i) {
        for (u32 c = 0; c < 3; ++c) err += std::abs(f64(255 - base.rgba[i * 4 + c]) - f64(inverted.rgba[i * 4 + c]));
    }
    err /= f64(base.width) * base.height * 3.0;
    EXPECT_LT(err, 3.0) << "inverting LUT: mean error " << err;
}

TEST_F(PostProcessTest, LensEffects) {
    colorfulScene();
    PostProcessSettings s;
    s.overrideLens = true;
    s.vignetteIntensity = 0.6f;
    s.chromaticAberration = 1.0f;
    s.filmGrainIntensity = 0.25f;
    volume(s);
    const Image img = renderFrames(colorfulCamera(), {.frames = 2});
    EXPECT_LT(img.luminance(2, 2), img.luminance(128, 40) + 0.05f); // corners darkened
    EXPECT_GOLDEN("postprocess_lens", img);
}

TEST_F(PostProcessTest, AutoExposureConvergesBetweenDarkAndBrightScenes) {
    const Uuid ground = material({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f);
    const Uuid obj = material({0.7f, 0.6f, 0.5f, 1.0f}, 0.0f, 0.5f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(40.0f));
    mesh(Primitive::Sphere, obj, {0, 0.8f, 0}, glm::vec3(1.6f));
    Entity sunE = sun({-0.3f, -0.8f, -0.5f}, 300.0f, glm::vec3(1.0f), false);
    Entity env = environment(0.0f, 0.0f);
    const CameraParams cam = camera({0, 1.5f, 5.0f}, {0, 0.6f, 0}, 12.0f, 50.0f);
    CVarScope ae("r.Exposure.Auto", "true");
    auto setLux = [&](f32 lux) { sunE.get<LightComponent>().intensity = lux; };

    setLux(300.0f); // dusk: ~8 EV darker than daylight
    const Image dark = renderFrames(cam, {.frames = 3}, {}, 1.0f / 30.0f);
    const f64 lDark = meanLuminance(dark);
    // Switch to 100 klx: the first frame is overexposed, then adaptation brings it back.
    setLux(100000.0f);
    std::vector<f64> curve;
    Image bright;
    for (int i = 0; i < 6; ++i) {
        bright = renderFrames(cam, {.frames = i == 0 ? 1u : 15u}, {}, 1.0f / 30.0f, true);
        curve.push_back(meanLuminance(bright));
    }
    const f64 lBright = curve.back();
    std::printf("auto exposure: dark %.3f; bright adaptation:", lDark);
    for (f64 l : curve) std::printf(" %.3f", l);
    std::printf("\n");
    EXPECT_GT(curve.front(), lBright + 0.15) << "the scene change must first overexpose";
    for (usize i = 1; i < curve.size(); ++i) EXPECT_LE(curve[i], curve[i - 1] + 0.01);
    EXPECT_NEAR(lBright, lDark, 0.12) << "both scenes converge to the same metered brightness";
    EXPECT_GT(lDark, 0.12);
    EXPECT_LT(lDark, 0.6);

    // And back to dusk (slower adaptation towards darker).
    setLux(300.0f);
    Image back = renderFrames(cam, {.frames = 1}, {}, 1.0f / 30.0f, true);
    const f64 first = meanLuminance(back);
    back = renderFrames(cam, {.frames = 120}, {}, 1.0f / 30.0f, true);
    std::printf("auto exposure back to dark: %.3f → %.3f\n", first, meanLuminance(back));
    EXPECT_LT(first, lDark - 0.1);
    EXPECT_NEAR(meanLuminance(back), lDark, 0.12);
    EXPECT_GOLDEN("postprocess_auto_exposure", bright);
    (void)env;
}

// --- performance ---------------------------------------------------------------------------------------------

TEST_F(PostProcessTest, PerfReport1080p) {
    Random rng(7);
    const Uuid ground = material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.8f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(120.0f));
    for (int i = 0; i < 200; ++i) {
        const Uuid m = material({rng.nextFloat(), rng.nextFloat(), rng.nextFloat(), 1.0f}, 0.0f, rng.range(0.1f, 0.9f),
                                rng.chance(0.1f) ? glm::vec3(8.0f) : glm::vec3(0.0f));
        mesh(Primitive::Sphere, m, {rng.range(-30.0f, 30.0f), 0.6f, rng.range(-30.0f, 10.0f)}, glm::vec3(rng.range(0.6f, 2.0f)));
    }
    sun(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 30000.0f);
    environment();
    PostProcessSettings s;
    s.overrideDepthOfField = true;
    s.focusDistance = 12.0f;
    s.aperture = 2.0f;
    s.overrideLens = true;
    s.vignetteIntensity = 0.4f;
    s.chromaticAberration = 0.3f;
    s.filmGrainIntensity = 0.1f;
    s.overrideGrading = true;
    s.saturation = 1.1f;
    s.overrideExposure = true;
    s.autoExposure = true;
    volume(s);
    const CameraParams cam = camera({0, 6, 25}, {0, 0, 0}, 13.0f, 60.0f, 300.0f);
    auto moving = [](RenderSnapshot& snap, u32) {
        for (usize i = 1; i < snap.meshes.size(); i += 7) {
            snap.meshes[i].prevWorld = glm::translate(glm::mat4(1.0f), {-0.2f, 0, 0}) * snap.meshes[i].world;
        }
    };
    auto report = [&](const char* label) {
        const Image img = renderFrames(cam, {.width = 1920, .height = 1080, .frames = 8}, moving, 1.0f / 60.0f);
        (void)img;
        const RenderStats& st = renderer->stats();
        std::map<std::string, f64> groups;
        for (const PassTiming& p : st.passes) {
            std::string g = p.name;
            if (auto slash = g.find('/'); slash != std::string::npos) g = g.substr(slash + 1); // "<view>/<pass>"
            if (auto dot = g.find('.'); dot != std::string::npos) g = g.substr(0, dot);
            groups[g] += p.gpuMs;
        }
        std::printf("[%s] GPU frame %.2f ms:", label, st.gpuFrameMs);
        for (const char* k : {"AutoExposure", "TAA", "TAAU", "FSR1", "DLSS", "DOF", "MotionBlur", "Bloom", "Grading",
                              "PostComposite", "Sharpen", "LdrPost", "FXAA", "Tonemap", "Resample"}) {
            if (auto it = groups.find(k); it != groups.end()) std::printf(" %s %.3f", k, it->second);
        }
        std::printf("\n   passes:");
        for (const PassTiming& p : st.passes) {
            for (const char* k : {"AutoExposure", "TAA", "FSR1", "DLSS", "DOF", "MotionBlur", "Bloom", "Grading",
                                  "PostComposite", "Sharpen", "LdrPost", "FXAA"}) {
                if (p.name.rfind(k, 0) == 0) {
                    std::printf(" %s %.3f", p.name.c_str(), p.gpuMs);
                    break;
                }
            }
        }
        std::printf("\n");
        EXPECT_GT(st.gpuFrameMs, 0.0);
    };
    CVarScope bloom("r.Bloom", "true");
    CVarScope dof("r.DepthOfField", "true");
    CVarScope mb("r.MotionBlur", "true");
    {
        CVarScope aa("r.AntiAliasing", "2");
        report("native 1080p, TAA + full post");
    }
    {
        CVarScope aa("r.AntiAliasing", "1");
        report("native 1080p, FXAA + full post");
    }
    {
        CVarScope aa("r.AntiAliasing", "2");
        CVarScope up("r.Upscaler", "FSR1");
        CVarScope q("r.Upscaler.Quality", "Performance");
        report("FSR1 540p→1080p + TAA");
    }
    {
        CVarScope up("r.Upscaler", "TAAU");
        CVarScope q("r.Upscaler.Quality", "Performance");
        report("TAAU 540p→1080p");
    }
    if (upscalerAvailability(UpscalerType::DLSS, device.get()).available) {
        CVarScope up("r.Upscaler", "DLSS");
        for (const char* quality : {"Performance", "Quality", "Native"}) {
            CVarScope q("r.Upscaler.Quality", quality);
            report(std::format("DLSS {} → 1080p", quality).c_str());
        }
    }
}
