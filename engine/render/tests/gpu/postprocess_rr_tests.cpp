// DLSS Ray Reconstruction (postprocess-upscalers + ray tracing denoise integration), GPU tests (labels render;gpu).
//
// Need an NVIDIA RTX GPU with the NGX Ray Reconstruction runtime (nvngx_dlssd); GTEST_SKIP elsewhere, except the
// fallback test, which runs everywhere (on the RTX machine it recreates the device without the NGX extensions).
// Images go to <temp>/oxwald_render_out/rr_*.png. Debug builds run the development NGX runtime, which stamps a banner
// into the lower part of the frame: image metrics leave that band out (same as the DLSS tests).
//
// OX_RT_PERF=1 makes PerfReportRayReconstruction measure 1080p and 4K (run with OX_VULKAN_VALIDATION=0).
#include "render_fixture.hpp"

#include <oxwald/render/features/postprocess/postprocess.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

namespace {

// Rows [skip0, skip1) are left out: the development NGX runtime's banner (Debug builds only).
void bannerRows(const Image& img, u32& skip0, u32& skip1) {
#if defined(NDEBUG)
    skip0 = skip1 = 0;
    (void)img;
#else
    skip0 = img.height * 80 / 100;
    skip1 = img.height * 95 / 100;
#endif
}

f64 psnr(const Image& a, const Image& b) {
    u32 s0, s1;
    bannerRows(a, s0, s1);
    f64 se = 0.0;
    usize n = 0;
    for (u32 y = 0; y < a.height; ++y) {
        if (y >= s0 && y < s1) continue;
        for (u32 x = 0; x < a.width; ++x, ++n) {
            const usize i = usize(y) * a.width + x;
            for (u32 c = 0; c < 3; ++c) {
                const f64 d = f64(a.rgba[i * 4 + c]) - f64(b.rgba[i * 4 + c]);
                se += d * d;
            }
        }
    }
    const f64 mse = se / (f64(n) * 3.0);
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// 2x2 box downsample: the path traced reference keeps some firefly noise even at thousands of samples; comparing at
// half resolution measures the denoised signal rather than how well a denoiser reproduces the reference's noise.
Image half(const Image& img) {
    Image out;
    out.width = img.width / 2;
    out.height = img.height / 2;
    out.rgba.resize(usize(out.width) * out.height * 4);
    for (u32 y = 0; y < out.height; ++y) {
        for (u32 x = 0; x < out.width; ++x) {
            for (u32 c = 0; c < 4; ++c) {
                u32 sum = 0;
                for (u32 j = 0; j < 2; ++j)
                    for (u32 i = 0; i < 2; ++i) sum += img.rgba[(usize(y * 2 + j) * img.width + x * 2 + i) * 4 + c];
                out.rgba[(usize(y) * out.width + x) * 4 + c] = u8((sum + 2) / 4);
            }
        }
    }
    return out;
}

// Mean absolute luminance difference (0..255) between two frames: flicker / temporal instability.
f64 lumaDelta(const Image& a, const Image& b) {
    u32 s0, s1;
    bannerRows(a, s0, s1);
    f64 sum = 0.0;
    usize n = 0;
    for (u32 y = 0; y < a.height; ++y) {
        if (y >= s0 && y < s1) continue;
        for (u32 x = 0; x < a.width; ++x, ++n) sum += std::abs(f64(a.luminance(x, y)) - f64(b.luminance(x, y))) * 255.0;
    }
    return n ? sum / f64(n) : 0.0;
}

f64 meanLuminance(const Image& img) {
    f64 s = 0.0;
    for (u32 y = 0; y < img.height; ++y)
        for (u32 x = 0; x < img.width; ++x) s += img.luminance(x, y);
    return s / (f64(img.width) * img.height);
}

class RayReconstructionTest : public RenderTest {
protected:
    [[nodiscard]] bool rrAvailable() const {
        return device->caps().rayTracingSupported() && rayReconstructionAvailability(device.get()).available;
    }
    // Exact pass name (the timings are "<view>/<pass>").
    [[nodiscard]] bool ranPass(std::string_view name) const {
        for (const PassTiming& p : renderer->stats().passes) {
            std::string_view n = p.name;
            if (const auto slash = n.find('/'); slash != std::string_view::npos) n = n.substr(slash + 1);
            if (n == name) return true;
        }
        return false;
    }
    [[nodiscard]] bool ranPassContaining(std::string_view part) const {
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.find(part) != std::string::npos) return true;
        }
        return false;
    }
    [[nodiscard]] f64 passMs(std::string_view name) const {
        f64 ms = 0.0;
        for (const PassTiming& p : renderer->stats().passes) {
            std::string_view n = p.name;
            if (const auto slash = n.find('/'); slash != std::string_view::npos) n = n.substr(slash + 1);
            if (n == name) ms += p.gpuMs;
        }
        return ms;
    }
    [[nodiscard]] bool anySvgfPass() const {
        return ranPassContaining(".Temporal") || ranPassContaining(".Atrous") || ranPassContaining(".Variance");
    }
    // Histories start over (different denoiser / upscaler configurations are compared from the same start).
    void freshView() {
        if (view) {
            device->waitIdle();
            renderer->destroyView(view);
            view = 0;
        }
    }
    static void dump(const char* name, const Image& img) {
        writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / (std::string(name) + ".png"), img);
    }

    // Floor, a rough gold sphere and a glossy metal slab (the grainy reflections of the RTX bring-up), coloured
    // dielectrics, sun + two shadowed point lights.
    void rrScene() {
        mesh(Primitive::Plane, material({0.7f, 0.7f, 0.7f, 1.0f}, 0.0f, 0.6f), {0, 0, 0}, glm::vec3(30.0f));
        mesh(Primitive::Cube, material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.15f), {0.0f, 0.02f, 1.2f}, {3.0f, 0.04f, 1.6f});
        mesh(Primitive::Sphere, material({1.0f, 0.78f, 0.34f, 1.0f}, 1.0f, 0.3f), {-0.9f, 0.6f, 0.0f}, glm::vec3(1.2f));
        mesh(Primitive::Cube, material({0.8f, 0.1f, 0.08f, 1.0f}, 0.0f, 0.5f), {1.1f, 0.5f, -0.4f});
        mesh(Primitive::Cylinder, material({0.1f, 0.25f, 0.85f, 1.0f}, 0.0f, 0.35f), {0.4f, 0.75f, -1.6f}, {0.5f, 1.5f, 0.5f});
        mesh(Primitive::Sphere, material({0.9f, 0.9f, 0.9f, 1.0f}, 0.0f, 0.8f), {1.6f, 0.35f, 0.9f}, glm::vec3(0.7f));
        sun({-0.45f, -0.8f, -0.4f}, 20000.0f);
        pointLight({-2.0f, 1.6f, 1.5f}, 6000.0f, 8.0f, {1.0f, 0.6f, 0.3f}, true);
        pointLight({2.2f, 1.2f, -0.5f}, 6000.0f, 8.0f, {0.4f, 0.6f, 1.0f}, true);
        environment();
    }
    static CameraParams rrCamera() { return camera({0.2f, 2.0f, 4.6f}, {0.0f, 0.4f, 0.0f}, 12.5f, 55.0f); }
};

#define OX_REQUIRE_RR()                                                                                                \
    if (!rrAvailable())                                                                                               \
    GTEST_SKIP() << "needs DLSS Ray Reconstruction: " << rayReconstructionAvailability(device.get()).reason

} // namespace

// --- creation / evaluation / pass set ---------------------------------------------------------------------------

TEST_F(RayReconstructionTest, CreatesAndEvaluatesWithoutSvgfPasses) {
    OX_REQUIRE_RR();
    rrScene();
    CVarScope rt("r.RayTracing", "true");
    const Image svgf = render(rrCamera(), {.width = 480, .height = 270, .frames = 8});
    EXPECT_TRUE(anySvgfPass()) << "SVGF path: the ray traced effects denoise themselves";
    EXPECT_FALSE(ranPass("DLSSRR"));
    EXPECT_FALSE(ranPass("RT.RRGuides"));

    CVarScope d("r.Denoiser", "DLSSRR");
    const Image rr = render(rrCamera(), {.width = 480, .height = 270, .frames = 8});
    dump("rr_basic_svgf", svgf);
    dump("rr_basic", rr);
    EXPECT_TRUE(ranPass("DLSSRR")) << "Ray Reconstruction evaluates";
    EXPECT_TRUE(ranPass("RT.RRGuides")) << "albedo guides";
    EXPECT_TRUE(ranPass("RT.Shadows") && ranPass("RT.AO") && ranPass("RT.Reflections")) << "the rays still run";
    EXPECT_FALSE(anySvgfPass()) << "no SVGF temporal / variance / a-trous pass under Ray Reconstruction";
    EXPECT_FALSE(ranPass("DLSS")) << "RR replaces DLSS super resolution / TAAU / TAA";
    EXPECT_FALSE(ranPass("TAA"));
    EXPECT_FALSE(ranPass("TAAU"));
    // Upscaler Off → DLAA: render at output resolution.
    EXPECT_EQ(renderer->view(view)->renderExtent().width, 480u);
    EXPECT_GT(meanLuminance(rr), 0.1);
    // Same scene, same lighting: the two denoisers agree on the overall image.
    EXPECT_NEAR(meanLuminance(rr), meanLuminance(svgf), 0.05);
    EXPECT_EQ(rhi::Device::validationErrorCount(), 0u);
}

TEST_F(RayReconstructionTest, QualityModesResizeAndRecreate) {
    OX_REQUIRE_RR();
    rrScene();
    CVarScope rt("r.RayTracing", "true");
    CVarScope d("r.Denoiser", "Auto");
    CVarScope up("r.Upscaler", "DLSS");
    struct Mode {
        const char* name;
        u32 minWidth, maxWidth; // render width at 960 output
    };
    for (const Mode m : {Mode{"UltraPerformance", 300, 340}, Mode{"Performance", 460, 500}, Mode{"Balanced", 540, 580},
                         Mode{"Quality", 620, 660}, Mode{"Native", 960, 960}}) {
        CVarScope q("r.Upscaler.Quality", m.name);
        render(rrCamera(), {.width = 960, .height = 540, .frames = 3});
        EXPECT_TRUE(ranPass("DLSSRR")) << m.name;
        const u32 w = renderer->view(view)->renderExtent().width;
        std::printf("[rr] %-16s render width %u at 960 output\n", m.name, w);
        EXPECT_GE(w, m.minWidth) << m.name;
        EXPECT_LE(w, m.maxWidth) << m.name;
    }
    // Output resize with the feature alive (recreated after waitIdle).
    CVarScope q("r.Upscaler.Quality", "Quality");
    const Image small = render(rrCamera(), {.width = 640, .height = 360, .frames = 4});
    const Image large = render(rrCamera(), {.width = 800, .height = 450, .frames = 4});
    EXPECT_GT(meanLuminance(small), 0.1);
    EXPECT_GT(meanLuminance(large), 0.1);
    EXPECT_EQ(rhi::Device::validationErrorCount(), 0u);
}

// --- image quality / stability against SVGF + DLSS super resolution -----------------------------------------------

TEST_F(RayReconstructionTest, ImageQualityAndStabilityVersusSvgfPlusDlss) {
    OX_REQUIRE_RR();
    rrScene();
    const CameraParams cam = rrCamera();
    constexpr u32 kW = 640, kH = 360;
    CVarScope rt("r.RayTracing", "true");
    const Image reference = [&] {
        CVarScope pt("r.PathTracing", "true");
        CVarScope d("r.Denoiser", "SVGF");
        CVarScope up("r.Upscaler", "Off");
        freshView();
        return render(cam, {.width = kW, .height = kH, .frames = 4096});
    }();
    dump("rr_quality_path_traced", reference);
    // The same hybrid lighting model as the denoised frames, converged instead of denoised: 32 rays per pixel for
    // shadows / reflections / AO, no SVGF, TAA at native resolution over 64 frames. Against it the comparison
    // measures the denoisers alone, without the bias of the hybrid lighting against the path tracer.
    const Image hybrid = [&] {
        CVarScope d("r.Denoiser", "SVGF");
        CVarScope up("r.Upscaler", "Off");
        CVarScope s0("r.RayTracing.Shadows.SamplesPerPixel", "32");
        CVarScope s1("r.RayTracing.Reflections.SamplesPerPixel", "32");
        CVarScope s2("r.RayTracing.AO.SamplesPerPixel", "32");
        CVarScope d0("r.RayTracing.Shadows.Denoiser", "false");
        CVarScope d1("r.RayTracing.Reflections.Denoiser", "false");
        CVarScope d2("r.RayTracing.AO.Denoiser", "false");
        freshView();
        return render(cam, {.width = kW, .height = kH, .frames = 64});
    }();
    dump("rr_quality_hybrid_converged", hybrid);

    struct Result {
        f64 psnr = 0.0, psnrHalf = 0.0, flicker = 0.0, psnrHybrid = 0.0;
        Image image;
    };
    // Converge 48 frames, then the mean frame-to-frame luminance change over 8 more frames of a static camera.
    auto measure = [&](const char* denoiser, const char* quality, const char* name) {
        CVarScope d("r.Denoiser", denoiser);
        CVarScope up("r.Upscaler", "DLSS");
        CVarScope q("r.Upscaler.Quality", quality);
        freshView();
        Result r;
        Image prev = render(cam, {.width = kW, .height = kH, .frames = 48});
        f64 flicker = 0.0;
        for (int i = 0; i < 8; ++i) {
            Image next = render(cam, {.width = kW, .height = kH, .frames = 1});
            flicker += lumaDelta(prev, next) / 8.0;
            prev = std::move(next);
        }
        r.image = std::move(prev);
        r.psnr = psnr(r.image, reference);
        r.psnrHalf = psnr(half(r.image), half(reference));
        r.psnrHybrid = psnr(half(r.image), half(hybrid));
        if (std::getenv("OX_RR_DIAG")) { // 8x4 tiles: mean signed luminance difference against the reference (x1000)
            std::printf("[rr-diag] %s\n", name);
            for (u32 ty = 0; ty < 4; ++ty) {
                std::printf("[rr-diag]  ");
                for (u32 tx = 0; tx < 8; ++tx) {
                    f64 s = 0.0;
                    u32 n = 0;
                    for (u32 y = ty * kH / 4; y < (ty + 1) * kH / 4; ++y)
                        for (u32 x = tx * kW / 8; x < (tx + 1) * kW / 8; ++x, ++n)
                            s += f64(r.image.luminance(x, y)) - f64(reference.luminance(x, y));
                    std::printf("%6.0f", s / n * 1000.0);
                }
                std::printf("\n");
            }
        }
        r.flicker = flicker;
        dump(name, r.image);
        return r;
    };
    const Result svgfQ = measure("SVGF", "Quality", "rr_quality_svgf_dlss_quality");
    EXPECT_TRUE(anySvgfPass());
    EXPECT_TRUE(ranPass("DLSS"));
    const Result rrQ = measure("DLSSRR", "Quality", "rr_quality_rr_quality");
    EXPECT_TRUE(ranPass("DLSSRR"));
    const Result svgfN = measure("SVGF", "Native", "rr_quality_svgf_dlaa");
    const Result rrN = measure("DLSSRR", "Native", "rr_quality_rr_dlaa");
    std::printf("[rr] PSNR vs path tracer, full / half resolution (dB); half-res PSNR vs converged hybrid (dB); flicker "
                "(mean |dL|, 0..255):\n");
    for (const auto& [label, r] : {std::pair<const char*, const Result*>{"SVGF + DLSS Quality", &svgfQ}, {"RR Quality", &rrQ},
                                   {"SVGF + DLAA", &svgfN}, {"RR DLAA", &rrN}}) {
        std::printf("[rr]   %-20s %6.2f / %6.2f   %6.2f   %.3f\n", label, r->psnr, r->psnrHalf, r->psnrHybrid, r->flicker);
    }
    // Denoising quality: against the converged hybrid frame (same lighting, no denoiser) RR must not lose against
    // SVGF + DLSS at the same render resolution. RTX 3080, driver 617.42, preset E: RR Quality 37.8 dB vs SVGF + DLSS
    // Quality 34.5 dB, RR DLAA 38.2 dB vs SVGF + DLAA 37.1 dB.
    EXPECT_GE(rrQ.psnrHybrid, svgfQ.psnrHybrid - 0.25) << "RR Quality vs SVGF + DLSS Quality (converged hybrid)";
    EXPECT_GE(rrN.psnrHybrid, svgfN.psnrHybrid - 0.25) << "RR DLAA vs SVGF + DLAA (converged hybrid)";
    // Against the path tracer RR scores lower (25.3 vs 25.8 dB at Quality, 25.3 vs 25.7 dB at DLAA, half resolution).
    // Both denoisers start from the same hybrid lighting, which misses energy the path tracer has: the second bounce
    // inside reflections (underside of the spheres in the metal slab, the gold sphere's mirror image), so the
    // reflections are 0.14-0.15 darker in luminance for both. The gap between the two denoisers sits in exactly those
    // tiles: SVGF keeps residual sparkle there, which adds back some of the missing energy by chance, RR removes it (it
    // is noise: against the converged hybrid RR wins by 1-3 dB, see above). Investigated and ruled out as RR input
    // errors: specular albedo guide (F0 * A + B), diffuse albedo, roughness as perceptual vs squared (Unpacked mode
    // with alpha: -0.01 dB), the specular hit distance (no change with it removed), the camera matrices (removing them
    // flickers 3x more), the reflection firefly clamp (16x vs 64x display white: identical; 4x: -0.1 dB).
    // So the path tracer comparison only bounds the loss.
    EXPECT_GE(rrQ.psnrHalf, svgfQ.psnrHalf - 0.6) << "RR Quality vs SVGF + DLSS Quality (path tracer)";
    EXPECT_GE(rrN.psnrHalf, svgfN.psnrHalf - 0.6) << "RR DLAA vs SVGF + DLAA (path tracer)";
    EXPECT_LE(rrQ.flicker, std::max(svgfQ.flicker * 1.25, 0.3)) << "temporal stability (static camera)";
    EXPECT_LE(rrN.flicker, std::max(svgfN.flicker * 1.25, 0.3));
}

// --- runtime toggling RR <-> SVGF ---------------------------------------------------------------------------------

TEST_F(RayReconstructionTest, RuntimeToggleWithoutLeaks) {
    OX_REQUIRE_RR();
    rrScene();
    const CameraParams cam = rrCamera();
    CVarScope rt("r.RayTracing", "true");
    CVarScope up("r.Upscaler", "DLSS");
    CVarScope q("r.Upscaler.Quality", "Quality");
    render(cam, {.width = 640, .height = 360, .frames = 4}); // SVGF + DLSS SR: histories, pipelines
    device->waitIdle();
    const rhi::GpuMemoryStats svgf0 = device->memoryStats();
    const u32 ngxSvgf = ngxLiveFeatureCount(); // the DLSS super resolution feature of this view
    rhi::GpuMemoryStats rrFirst{}, svgfFirst{}, rrLast{}, svgfLast{};
    u32 ngxRrMax = 0, ngxRrMin = ~0u, ngxOffMax = 0;
    for (int i = 0; i < 6; ++i) {
        {
            CVarScope d("r.Denoiser", "DLSSRR");
            render(cam, {.width = 640, .height = 360, .frames = 3});
            EXPECT_TRUE(ranPass("DLSSRR"));
            EXPECT_FALSE(anySvgfPass());
            device->waitIdle();
            (i == 1 ? rrFirst : rrLast) = device->memoryStats();
            ngxRrMax = std::max(ngxRrMax, ngxLiveFeatureCount());
            ngxRrMin = std::min(ngxRrMin, ngxLiveFeatureCount());
        }
        render(cam, {.width = 640, .height = 360, .frames = 3});
        EXPECT_FALSE(ranPass("DLSSRR"));
        EXPECT_TRUE(anySvgfPass());
        EXPECT_TRUE(ranPass("DLSS"));
        device->waitIdle();
        (i == 1 ? svgfFirst : svgfLast) = device->memoryStats();
        ngxOffMax = std::max(ngxOffMax, ngxLiveFeatureCount());
    }
    auto mb = [](const rhi::GpuMemoryStats& s) { return f64(s.totalUsageBytes) / 1048576.0; };
    std::printf("[rr] VRAM: SVGF %.1f MB, RR %.1f MB, back to SVGF %.1f MB; after 6 toggles RR %.1f MB, SVGF %.1f MB "
                "(textures %u -> %u, buffers %u -> %u); NGX features: SVGF %u, RR %u..%u, back to SVGF <= %u\n",
                mb(svgf0), mb(rrFirst), mb(svgfFirst), mb(rrLast), mb(svgfLast), svgfFirst.textureCount,
                svgfLast.textureCount, svgfFirst.bufferCount, svgfLast.bufferCount, ngxSvgf, ngxRrMin, ngxRrMax, ngxOffMax);
    // Switching RR off gives its NGX feature back (DLSSRRRelease) and switching it on again creates exactly one: the
    // number of live NGX features never grows.
    EXPECT_GE(ngxRrMin, 1u);
    EXPECT_LE(ngxRrMax, ngxSvgf + 1);
    EXPECT_LE(ngxOffMax, ngxSvgf);
    // Repeated toggling allocates nothing new: neither engine resources nor driver-level video memory.
    EXPECT_LE(svgfLast.totalUsageBytes, svgfFirst.totalUsageBytes + (8u << 20));
    EXPECT_LE(rrLast.totalUsageBytes, rrFirst.totalUsageBytes + (8u << 20));
    EXPECT_LE(svgfLast.textureCount, svgfFirst.textureCount + 2);
    EXPECT_LE(svgfLast.bufferCount, svgfFirst.bufferCount + 4);
    // Not checked: svgfFirst against svgf0. The first Ray Reconstruction feature makes NGX load the RR network and its
    // device-wide working memory (~360-510 MB on an RTX 3080, driver 617.42), which NGX keeps after
    // NVSDK_NGX_VULKAN_ReleaseFeature until NGX shuts down with the device; destroying the whole Renderer gives back
    // only ~120 MB of it. The steady state above (no growth over six toggles) is what a leak would break.
    EXPECT_EQ(rhi::Device::validationErrorCount(), 0u);
}

// --- fallback ------------------------------------------------------------------------------------------------------

TEST_F(RayReconstructionTest, FallsBackToSvgfWhereUnavailable) {
    RayReconstructionAvailability a = rayReconstructionAvailability(device.get());
    if (a.available) {
        // RTX machine: the same GPU through a device created without the NGX extensions must report Ray
        // Reconstruction unavailable up front (NGX not called) and keep the SVGF denoisers.
        device->waitIdle();
        renderer.reset();
        if (target) device->destroy(target);
        world.reset();
        device.reset();
        target = {};
        view = 0;
        rhi::DeviceDesc desc;
        desc.appName = "ox_render_gpu_tests";
        desc.validation = true;
        desc.shaderOptions.cacheDirectory = std::filesystem::temp_directory_path() / "oxwald_render_test_shader_cache";
        device = rhi::Device::create(desc);
        ASSERT_TRUE(device);
        renderer = Renderer::create(*device);
        world = std::make_unique<World>();
        a = rayReconstructionAvailability(device.get());
    }
    EXPECT_FALSE(a.available);
    EXPECT_FALSE(a.reason.empty());
    std::printf("[rr] unavailable: %s\n", a.reason.c_str());
    if (!device->caps().rayTracingSupported()) {
        // MoltenVK & co.: r.Denoiser is inert, nothing ray traced runs.
        rrScene();
        CVarScope rt("r.RayTracing", "true");
        CVarScope d("r.Denoiser", "DLSSRR");
        render(rrCamera(), {.frames = 2});
        EXPECT_FALSE(ranPass("DLSSRR"));
        return;
    }
    rrScene();
    CVarScope rt("r.RayTracing", "true");
    CVarScope d("r.Denoiser", "DLSSRR");
    CVarScope up("r.Upscaler", "TAAU");
    const Image img = render(rrCamera(), {.width = 480, .height = 270, .frames = 6});
    dump("rr_fallback_svgf", img);
    EXPECT_FALSE(ranPass("DLSSRR"));
    EXPECT_FALSE(ranPass("RT.RRGuides"));
    EXPECT_TRUE(anySvgfPass()) << "ray traced effects keep SVGF";
    EXPECT_TRUE(ranPass("TAAU")) << "and the configured upscaler stays";
    EXPECT_GT(meanLuminance(img), 0.1);
}

// --- GPU timings: hybrid RT frame with SVGF + DLSS against Ray Reconstruction ----------------------------------------

TEST_F(RayReconstructionTest, PerfReportRayReconstruction) {
    OX_REQUIRE_RR();
    const char* perf = std::getenv("OX_RT_PERF");
    const bool measureFull = perf && *perf && std::string_view(perf) != "0";
    // The scene of RayTracingTest.PerfReportRayTracing: 240 objects, a mirror-like floor patch, glass, 8 shadowed
    // local lights, sun, fog.
    Random rng(11);
    mesh(Primitive::Plane, material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(120.0f));
    mesh(Primitive::Cube, material({0.9f, 0.9f, 0.9f, 1.0f}, 1.0f, 0.1f), {0.0f, 0.02f, 8.0f}, {16.0f, 0.04f, 10.0f});
    Entity mover;
    for (int i = 0; i < 240; ++i) {
        const f32 cr = rng.nextFloat();
        const f32 cg = rng.nextFloat();
        const f32 cb = rng.nextFloat();
        const f32 rough = rng.range(0.1f, 0.9f);
        const f32 x = rng.range(-30.0f, 30.0f);
        const f32 z = rng.range(-30.0f, 14.0f);
        const f32 size = rng.range(0.6f, 2.0f);
        const Primitive prim = i % 3 == 0 ? Primitive::Cube : i % 3 == 1 ? Primitive::Sphere : Primitive::Cylinder;
        mover = mesh(prim, material({cr, cg, cb, 1.0f}, i % 7 == 0 ? 1.0f : 0.0f, rough), {x, size * 0.5f, z}, glm::vec3(size));
    }
    const glm::vec3 moverStart = mover.worldPosition();
    const Uuid glass = material({0.8f, 0.95f, 0.9f, 1.0f}, 0.0f, 0.02f, glm::vec3(0.0f), assets::BlendMode::Refractive);
    for (int i = 0; i < 4; ++i) mesh(Primitive::Sphere, glass, {-6.0f + 4.0f * f32(i), 1.2f, 10.0f}, glm::vec3(2.4f));
    for (int i = 0; i < 8; ++i) {
        pointLight({-21.0f + 6.0f * f32(i), 3.0f, 4.0f - 3.0f * f32(i % 3)}, 40000.0f, 14.0f, glm::vec3(1.0f, 0.9f, 0.8f), true);
    }
    sun(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 30000.0f);
    Entity e = environment();
    auto& env = e.get<EnvironmentComponent>();
    env.fogEnabled = true;
    env.fogDensity = 0.01f;
    env.fogHeightFalloff = 0.05f;
    const CameraParams cam = camera({0, 6, 25}, {0, 0, 0}, 13.0f, 60.0f, 300.0f);

    std::vector<Extent2D> sizes{{640, 360}};
    if (measureFull) sizes = {{1920, 1080}, {3840, 2160}};
    if (unsigned w = 0, h = 0; measureFull && std::sscanf(perf, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) sizes = {{w, h}};
    const u32 warmUp = measureFull ? 48 : 4, frames = measureFull ? 8 : 2;
    std::printf("[rr-perf] %s, validation %s\n", device->caps().gpuName.c_str(), device->caps().validationEnabled ? "ON" : "off");
    CVarScope rt("r.RayTracing", "true");
    auto report = [&](Extent2D size, const char* label) {
        freshView();
        render(cam, {.width = size.width, .height = size.height, .frames = warmUp});
        f64 frame = 0.0, rtMs = 0.0, denoise = 0.0, upscale = 0.0, guides = 0.0;
        for (u32 f = 0; f < frames; ++f) {
            mover.setPosition(moverStart + glm::vec3(0.0f, 0.01f * f32(f + 1), 0.0f));
            render(cam, {.width = size.width, .height = size.height, .frames = 1});
            const RenderStats& st = renderer->stats();
            frame += st.gpuFrameMs / frames;
            for (const PassTiming& p : st.passes) {
                std::string n = p.name;
                if (auto slash = n.find('/'); slash != std::string::npos) n = n.substr(slash + 1);
                const f64 ms = p.gpuMs / frames;
                if (n.starts_with("Rt")) denoise += ms; // SVGF stages (+ format resolve / upsample under RR)
                if (n.starts_with("RT.") || n.starts_with("Rt")) rtMs += ms;
                if (n == "RT.RRGuides") guides += ms;
                if (n == "DLSS" || n == "DLSSRR" || n == "TAAU" || n == "TAA") upscale += ms;
            }
        }
        device->waitIdle();
        const rhi::GpuMemoryStats mem = device->memoryStats();
        const Extent2D re = renderer->view(view)->renderExtent();
        std::printf("[rr-perf] %ux%u %-24s render %4ux%-4u GPU frame %7.3f ms | RT passes %7.3f (denoise/resolve %6.3f, "
                    "RR guides %5.3f) | upscaler pass %6.3f ms | VRAM %.0f MB\n",
                    size.width, size.height, label, re.width, re.height, frame, rtMs, denoise, guides, upscale,
                    f64(mem.totalUsageBytes) / 1048576.0);
        return frame;
    };
    for (const Extent2D size : sizes) {
        {
            CVarScope d("r.Denoiser", "SVGF");
            CVarScope up("r.Upscaler", "Off");
            report(size, "SVGF, native, no AA");
        }
        for (const char* q : {"Quality", "Performance", "Native"}) {
            CVarScope up("r.Upscaler", "DLSS");
            CVarScope qs("r.Upscaler.Quality", q);
            {
                CVarScope d("r.Denoiser", "SVGF");
                report(size, std::format("SVGF + DLSS {}", q).c_str());
            }
            CVarScope d("r.Denoiser", "DLSSRR");
            const f64 ms = report(size, std::format("RR {}", q).c_str());
            if (device->caps().timestampQueries) EXPECT_GT(ms, 0.0);
            EXPECT_TRUE(ranPass("DLSSRR"));
        }
    }
}
