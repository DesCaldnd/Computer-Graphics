// Ray tracing area, GPU tests.
//
// Run everywhere (no ray tracing hardware needed):
//   * SVGF denoiser on a synthetic noisy signal (stochastic AO with random samples per frame): noise reduction vs a
//     many-sample reference, convergence, no ghosting after camera motion, half-resolution + upsample path
//   * DDGI probe blending / octahedral layout / apply with synthetic probe rays (analytic radiance fields)
//   * gating: r.RayTracing on a device without ray queries changes nothing (same passes, same image, no errors)
// Need an RT GPU (GTEST_SKIP on MoltenVK): every ray traced effect against its raster counterpart with loose
// tolerances or self-consistency checks, runtime toggling, path tracer convergence and accumulation resets.
#include "render_fixture.hpp"

#include <oxwald/render/features/raytracing/ddgi.hpp>
#include <oxwald/render/features/raytracing/denoiser.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/render/features/raytracing/rt_api.hpp>

#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

class RayTracingTest : public RenderTest {
protected:
    // Grey floor with a few occluders, ambient light only (AO-friendly). `sky` dims the sky for scenes lit by
    // local lights (the IBL follows the sky: ambientIntensity stays 1, so raster, ray traced and path traced
    // images see the same environment).
    void aoScene(f32 sky = 1.0f) {
        const Uuid grey = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.6f);
        mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(30.0f));
        mesh(Primitive::Cube, grey, {-0.8f, 0.5f, 0.0f});
        mesh(Primitive::Cube, grey, {0.9f, 0.35f, -0.6f}, glm::vec3(0.7f));
        mesh(Primitive::Sphere, grey, {0.2f, 0.4f, 1.0f}, glm::vec3(0.8f));
        mesh(Primitive::Cylinder, grey, {1.4f, 0.75f, 0.8f}, glm::vec3(0.4f, 1.5f, 0.4f));
        environment(sky, 1.0f);
    }
    Uuid materialEx(const assets::MaterialAsset& m) {
        const Uuid id = Uuid::fromName(std::format("test.material.{}", materialCounter++));
        renderer->resources().addMaterial(id, m);
        return id;
    }
    // Alpha-tested checker: 8x8 cells, every other one cut out (alpha 0).
    Uuid cutoutMaterial(glm::vec3 color) {
        constexpr u32 kSize = 64;
        assets::TextureData t;
        t.format = assets::TextureFormat::RGBA8Srgb;
        t.width = t.height = kSize;
        t.mipCount = 1;
        t.filter = assets::TextureFilter::Nearest;
        assets::TextureMip mip;
        mip.width = mip.height = kSize;
        mip.data.resize(usize(kSize) * kSize * 4);
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) {
                const bool solid = (((x / 8) + (y / 8)) & 1) != 0;
                const u8 px[4] = {255, 255, 255, u8(solid ? 255 : 0)};
                std::memcpy(&mip.data[(usize(y) * kSize + x) * 4], px, 4);
            }
        }
        t.mips.push_back(std::move(mip));
        const Uuid tex = Uuid::fromName(std::format("test.rt.texture.{}", materialCounter++));
        renderer->resources().addTexture(tex, t);
        assets::MaterialAsset m;
        m.baseColor = glm::vec4(color, 1.0f);
        m.roughness = 0.7f;
        m.blendMode = assets::BlendMode::AlphaTest;
        m.alphaCutoff = 0.5f;
        m.doubleSided = true;
        m.albedoTexture = tex;
        return materialEx(m);
    }
    // Like render(), but the world is changed and extracted again before every frame (moving objects).
    Image renderAnimated(const CameraParams& cam, u32 frames, const std::function<void(u32)>& beforeFrame) {
        ensureTarget(256, 256, VK_FORMAT_R8G8B8A8_UNORM);
        ensureView(false);
        for (u32 f = 0; f < frames; ++f) {
            beforeFrame(f);
            extractSnapshot();
            if (f == 0) renderer->resources().flush();
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
        img.width = img.height = 256;
        img.rgba = device->readTexture(target);
        for (usize i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
        return img;
    }
    [[nodiscard]] bool ranPass(std::string_view part) const {
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.find(part) != std::string::npos) return true;
        }
        return false;
    }
    [[nodiscard]] f64 passMs(std::string_view part) const {
        f64 ms = 0.0;
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.find(part) != std::string::npos) ms += p.gpuMs;
        }
        return ms;
    }
    // Every ray traced effect image goes to <temp>/oxwald_render_out/<name>.png for inspection.
    static void dump(const char* name, const Image& img) {
        writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / (std::string(name) + ".png"), img);
    }
};

namespace {

// RGBA16F texture readback → red channel as floats.
std::vector<f32> readRed16f(rhi::Device& device, rhi::TextureHandle tex) {
    const std::vector<u8> bytes = device.readTexture(tex);
    std::vector<f32> out(bytes.size() / 8);
    for (usize i = 0; i < out.size(); ++i) {
        u16 h;
        std::memcpy(&h, bytes.data() + i * 8, 2);
        out[i] = glm::unpackHalf1x16(h);
    }
    return out;
}

f64 rmse(const std::vector<f32>& a, const std::vector<f32>& b) {
    f64 s = 0.0;
    for (usize i = 0; i < a.size(); ++i) s += f64(a[i] - b[i]) * f64(a[i] - b[i]);
    return std::sqrt(s / f64(std::max<usize>(a.size(), 1)));
}

rhi::TextureHandle makeTarget(rhi::Device& dev, Extent2D e, const char* name) {
    rhi::TextureDesc td;
    td.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    td.width = e.width;
    td.height = e.height;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::TransferDst |
               rhi::TextureUsage::TransferSrc;
    td.name = name;
    return dev.createTexture(td);
}

void copyInto(FeatureContext& ctx, rhi::RGTexture src, rhi::TextureHandle dst, const char* name) {
    const rhi::RGTexture d = ctx.graph().importTexture(ctx.device(), dst);
    ctx.graph()
        .addPass(name, rhi::PassType::Transfer)
        .read(src, rhi::Access::TransferRead)
        .overwrite(d, rhi::Access::TransferWrite)
        .execute([src, d](rhi::PassContext& p) { p.cmd.copyTexture(p.texture(src), p.texture(d)); });
}

// Noisy stochastic AO (1 sample per pixel and frame) → SvgfDenoiser, plus a 256-sample reference; copies noisy,
// denoised and reference images into persistent textures for readback.
struct DenoiseProbeFeature final : IRenderFeature {
    rt::SvgfDenoiser denoiser;
    rhi::PipelineHandle noise;
    rhi::TextureHandle outNoisy, outDenoised, outRef;
    Extent2D extent;
    u32 scale = 1;
    u32 seed = 1;
    bool denoise = true;
    rt::DenoiseOutput format = rt::DenoiseOutput::RGBA16F;

    std::string_view name() const override { return "TestDenoiseProbe"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    bool initialize(FeatureInitContext& ctx) override {
        noise = createComputePipeline(ctx.device, "test.rt.noise", "render/raytracing/denoise_test_noise.comp");
        return denoiser.initialize(ctx.device) && ctx.device.vkPipeline(noise) != VK_NULL_HANDLE;
    }
    void shutdown(rhi::Device& d) override {
        denoiser.shutdown(d);
        d.destroy(noise);
        for (rhi::TextureHandle t : {outNoisy, outDenoised, outRef}) {
            if (t) d.destroy(t);
        }
    }
    rhi::RGTexture noisePass(FeatureContext& ctx, Extent2D e, u32 s, u32 samples, u32 passSeed, const char* name) {
        rhi::TextureDesc td;
        td.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        td.width = e.width;
        td.height = e.height;
        td.usage = rhi::TextureUsage::None;
        td.name = name;
        const rhi::RGTexture out = ctx.graph().createTexture(td);
        const rhi::RGTexture depth = ctx.resources().texture(res::kDepth), normals = ctx.resources().texture(res::kNormals);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        ctx.graph()
            .addPass(name, rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .overwrite(out, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                struct {
                    u64 view, scene;
                    u32 depth, normals, out, width, height, scale, samples, seed;
                    f32 radius;
                } pc{view, scene, p.sampledIndex(depth), p.sampledIndex(normals), p.storageIndex(out), e.width, e.height,
                     s, samples, passSeed, 1.0f};
                p.cmd.bindPipeline(noise);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((e.width + 7) / 8, (e.height + 7) / 8);
            });
        return out;
    }
    void setup(FeatureContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const Extent2D re = ctx.renderExtent();
        if (!(extent == re)) {
            for (rhi::TextureHandle* t : {&outNoisy, &outDenoised, &outRef}) {
                if (*t) dev.destroy(*t);
            }
            outNoisy = makeTarget(dev, re, "test.noisy");
            outDenoised = makeTarget(dev, re, "test.denoised");
            outRef = makeTarget(dev, re, "test.reference");
            extent = re;
        }
        const Extent2D se = scale == 1 ? re : Extent2D{(re.width + 1) / 2, (re.height + 1) / 2};
        const rhi::RGTexture signal = noisePass(ctx, se, scale, 1, seed++, "Test.NoisyAO");
        const rhi::RGTexture ref = noisePass(ctx, re, 1, 256, 777, "Test.ReferenceAO");
        rt::DenoiserInputs in;
        in.signal = signal;
        in.depth = ctx.resources().texture(res::kDepth);
        in.normals = ctx.resources().texture(res::kNormals);
        in.velocity = ctx.resources().texture(res::kVelocity);
        in.signalExtent = se;
        in.renderExtent = re;
        in.scale = scale;
        rt::DenoiserSettings st;
        st.enabled = denoise;
        st.lumaWeights = {1, 0, 0, 0};
        st.output = format;
        const rt::DenoiserOutputs out =
            denoiser.denoise(ctx, "TestAO", in, st, scale == 1 ? std::nullopt : std::optional<Extent2D>(re));
        if (scale == 1) copyInto(ctx, signal, outNoisy, "Test.CopyNoisy");
        if (format == rt::DenoiseOutput::RGBA16F) copyInto(ctx, out.result, outDenoised, "Test.CopyDenoised");
        copyInto(ctx, ref, outRef, "Test.CopyReference");
    }
};

bool hasRayTracing(rhi::Device& d) { return d.caps().rayTracingSupported(); }

#define OX_REQUIRE_RT()                                                                                                \
    if (!hasRayTracing(*device)) GTEST_SKIP() << "needs ray tracing: " << device->caps().whyRayTracingUnavailable()

f64 meanAbs(const Image& a, const Image& b) {
    f64 s = 0.0;
    for (usize i = 0; i < a.rgba.size(); ++i) s += std::abs(int(a.rgba[i]) - int(b.rgba[i]));
    return s / f64(a.rgba.size());
}

f64 meanLuminance(const Image& img, u32 x0, u32 y0, u32 x1, u32 y1) {
    f64 s = 0.0;
    u32 n = 0;
    for (u32 y = y0; y < y1; ++y)
        for (u32 x = x0; x < x1; ++x, ++n) s += img.luminance(x, y);
    return n ? s / n : 0.0;
}

glm::vec3 meanColor(const Image& img, u32 x0, u32 y0, u32 x1, u32 y1) {
    glm::dvec3 s(0.0);
    u32 n = 0;
    for (u32 y = y0; y < y1; ++y)
        for (u32 x = x0; x < x1; ++x, ++n) s += glm::dvec3(glm::vec3(img.at(x, y))) / 255.0;
    return glm::vec3(n ? s / f64(n) : s);
}

} // namespace

// --- denoiser (runs on every device) ---

TEST_F(RayTracingTest, DenoiserReducesNoiseOfAStochasticSignal) {
    aoScene();
    auto& probe = renderer->features().emplace<DenoiseProbeFeature>();
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 11.0f, 55.0f);
    render(cam, {.width = 192, .height = 192, .frames = 1});
    const std::vector<f32> ref = readRed16f(*device, probe.outRef);
    const f64 noisy1 = rmse(readRed16f(*device, probe.outNoisy), ref);
    const f64 first = rmse(readRed16f(*device, probe.outDenoised), ref);
    render(cam, {.width = 192, .height = 192, .frames = 24});
    const f64 noisy = rmse(readRed16f(*device, probe.outNoisy), ref);
    const f64 converged = rmse(readRed16f(*device, probe.outDenoised), ref);
    std::printf("[denoiser] RMSE vs 256-spp reference: noisy %.4f (frame 1 %.4f), denoised frame 1 %.4f, frame 25 %.4f\n",
                noisy, noisy1, first, converged);
    EXPECT_GT(noisy, 0.08) << "the synthetic signal must actually be noisy";
    EXPECT_LT(first, noisy1 * 0.6) << "spatial filtering alone already removes most noise";
    EXPECT_LT(converged, first) << "temporal accumulation keeps improving";
    EXPECT_LT(converged, noisy * 0.3) << "denoised result must be much closer to the reference";
    EXPECT_LT(converged, 0.06);

    // Pass-through mode keeps the noise (sanity check of the measurement).
    probe.denoise = false;
    render(cam, {.width = 192, .height = 192, .frames = 1});
    EXPECT_GT(rmse(readRed16f(*device, probe.outDenoised), ref), noisy * 0.8);
}

TEST_F(RayTracingTest, DenoiserDoesNotGhostUnderCameraMotion) {
    aoScene();
    auto& probe = renderer->features().emplace<DenoiseProbeFeature>();
    const CameraParams a = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 11.0f, 55.0f);
    const CameraParams b = camera({-0.9f, 2.4f, 4.0f}, {-0.4f, 0.3f, 0.0f}, 11.0f, 55.0f);
    render(a, {.width = 192, .height = 192, .frames = 24});
    const std::vector<f32> refA = readRed16f(*device, probe.outRef);
    const std::vector<f32> denA = readRed16f(*device, probe.outDenoised);
    render(b, {.width = 192, .height = 192, .frames = 1});
    const std::vector<f32> refB = readRed16f(*device, probe.outRef);
    const std::vector<f32> denB = readRed16f(*device, probe.outDenoised);
    const f64 change = rmse(refA, refB);
    const f64 errB = rmse(denB, refB);
    const f64 staleB = rmse(denA, refB);
    // Pixels whose reference changed a lot: the denoised frame must follow the new image, not the old one.
    f64 toNew = 0.0, toOld = 0.0;
    u32 changed = 0;
    for (usize i = 0; i < refA.size(); ++i) {
        if (std::abs(refA[i] - refB[i]) < 0.25f) continue;
        toNew += std::abs(denB[i] - refB[i]);
        toOld += std::abs(denB[i] - refA[i]);
        ++changed;
    }
    std::printf("[denoiser motion] reference change %.4f, denoised-after-move error %.4f, stale history error %.4f, "
                "changed pixels %u: |den-new| %.4f |den-old| %.4f\n",
                change, errB, staleB, changed, changed ? toNew / changed : 0.0, changed ? toOld / changed : 0.0);
    ASSERT_GT(change, 0.05) << "the camera move must change the image";
    ASSERT_GT(changed, 200u);
    EXPECT_LT(errB, staleB * 0.5) << "reprojection follows the motion";
    EXPECT_LT(toNew, toOld * 0.4) << "disoccluded / moved pixels must not show the old image (ghosting)";
    EXPECT_LT(errB, 0.09);
    render(b, {.width = 192, .height = 192, .frames = 16});
    EXPECT_LT(rmse(readRed16f(*device, probe.outDenoised), refB), 0.06) << "re-converges after the move";
}

TEST_F(RayTracingTest, DenoiserHalfResolutionUpsampleAndNarrowFormats) {
    aoScene();
    auto& probe = renderer->features().emplace<DenoiseProbeFeature>();
    probe.scale = 2;
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 11.0f, 55.0f);
    render(cam, {.width = 192, .height = 192, .frames = 24});
    const std::vector<f32> ref = readRed16f(*device, probe.outRef);
    const f64 err = rmse(readRed16f(*device, probe.outDenoised), ref);
    std::printf("[denoiser half-res] RMSE after joint-bilateral upsample: %.4f\n", err);
    EXPECT_LT(err, 0.08);
    // R8 / RGBA8 outputs (AO, shadow masks) build and run without validation errors.
    probe.scale = 1;
    probe.format = rt::DenoiseOutput::R8;
    render(cam, {.width = 192, .height = 192, .frames = 2});
    probe.format = rt::DenoiseOutput::RGBA8;
    render(cam, {.width = 192, .height = 192, .frames = 2});
    probe.denoise = false;
    render(cam, {.width = 192, .height = 192, .frames = 1});
}

TEST_F(RayTracingTest, DenoiserCost1080p) {
    aoScene();
    renderer->features().emplace<DenoiseProbeFeature>();
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 11.0f, 55.0f);
    render(cam, {.width = 1920, .height = 1080, .frames = 6});
    render(cam, {.width = 1920, .height = 1080, .frames = 2});
    f64 total = 0.0;
    for (const PassTiming& t : renderer->stats().passes) {
        if (t.name.find("TestAO.") == std::string::npos) continue;
        std::printf("[denoiser 1080p] %-40s %.3f ms\n", t.name.c_str(), t.gpuMs);
        total += t.gpuMs;
    }
    std::printf("[denoiser 1080p] total %.3f ms (RGBA16F signal, 4 a-trous iterations)\n", total);
    if (device->caps().timestampQueries) EXPECT_GT(total, 0.0);
}

// --- DDGI probe blending with synthetic rays (runs on every device) ---

namespace {

struct DdgiProbeFeature final : IRenderFeature {
    rt::DdgiVolumeRenderer volume;
    rhi::PipelineHandle fill;
    rhi::TextureHandle out;
    Extent2D extent;
    u32 mode = 0; // 0: L = max(dir.y, 0), 1: L = 0.5 constant
    u32 frame = 0;
    glm::vec3 center{0.1f, 0.5f, 0.1f};
    rt::DdgiVolumeGpu last;

    std::string_view name() const override { return "TestDdgiProbe"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    bool initialize(FeatureInitContext& ctx) override {
        rhi::ComputePipelineDesc d;
        d.name = "test.ddgi.fill";
        d.shader = rhi::ShaderStageDesc::glsl(R"(#version 460
#include <render/raytracing/ddgi.glsl>
layout(local_size_x = 64) in;
OX_PUSH_CONSTANTS({ DdgiVolumeRef volume; uint rays; uint probes; uint mode; });
void main() {
    DdgiVolume v = pc.volume.v;
    uint ray = gl_GlobalInvocationID.x, probe = gl_GlobalInvocationID.y;
    if (ray >= v.raysPerProbe || probe >= pc.probes) return;
    vec3 dir = oxDdgiRayDirection(v, ray);
    vec3 L = pc.mode == 0u ? vec3(max(dir.y, 0.0)) : vec3(0.5);
    OX_IMAGE_STORE_2D(rgba16f, pc.rays, ivec2(ray, probe), vec4(L, 100.0));
}
)", rhi::ShaderStage::Compute, "test_ddgi_fill.comp");
        fill = ctx.device.createComputePipeline(d);
        rt::DdgiVolumeDesc desc;
        desc.counts = {4, 2, 4};
        desc.spacing = 2.0f;
        desc.raysPerProbe = 128;
        volume.initialize(ctx.device);
        volume.configure(ctx.device, desc);
        return ctx.device.vkPipeline(fill) != VK_NULL_HANDLE;
    }
    void shutdown(rhi::Device& d) override {
        volume.shutdown(d);
        d.destroy(fill);
        if (out) d.destroy(out);
    }
    void setup(FeatureContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const Extent2D re = ctx.renderExtent();
        if (!(extent == re)) {
            if (out) dev.destroy(out);
            out = makeTarget(dev, re, "test.ddgi.out");
            extent = re;
        }
        volume.import(ctx);
        rt::DdgiVolumeGpu v = volume.frameVolume(dev, center, frame++);
        v.hysteresis = 0.5f;
        last = v;
        const VkDeviceAddress addr = ctx.upload(std::span<const rt::DdgiVolumeGpu>(&v, 1));
        const rhi::RGTexture rays = volume.createRayTexture(ctx);
        const u32 probes = volume.probeCount(), raysPerProbe = v.raysPerProbe, m = mode;
        ctx.graph()
            .addPass("Test.DdgiFill", rhi::PassType::Compute)
            .overwrite(rays, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                struct {
                    u64 volume;
                    u32 rays, probes, mode;
                } pc{addr, p.storageIndex(rays), probes, m};
                p.cmd.bindPipeline(fill);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((raysPerProbe + 63) / 64, probes);
            });
        volume.update(ctx, rays, addr);
        const rhi::RGTexture diffuse = volume.apply(ctx, addr, ctx.resources().texture(res::kDepth),
                                                    ctx.resources().texture(res::kNormals), re, 1);
        copyInto(ctx, diffuse, out, "Test.CopyDdgi");
    }
};

} // namespace

TEST_F(RayTracingTest, DdgiProbeBlendingMatchesAnalyticIrradiance) {
    const Uuid grey = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.6f);
    mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(30.0f));
    environment(1.0f, 1.0f);
    auto& probe = renderer->features().emplace<DdgiProbeFeature>();
    const CameraParams cam = camera({0.0f, 2.5f, 0.01f}, {0.0f, 0.0f, 0.0f}, 11.0f, 50.0f);
    // L(ω) = max(ω.y, 0) → for an upward normal E/π = (1/π) ∫ cos²θ dω over the upper hemisphere = 2/3.
    probe.mode = 0;
    render(cam, {.width = 96, .height = 96, .frames = 1});
    std::vector<f32> v = readRed16f(*device, probe.out);
    f64 mean = 0.0, worst = 0.0;
    for (f32 x : v) {
        mean += x;
        worst = std::max(worst, std::abs(f64(x) - 2.0 / 3.0));
    }
    mean /= f64(v.size());
    std::printf("[ddgi] L = max(y,0): mean irradiance/π %.4f (expected 0.6667), worst deviation %.4f\n", mean, worst);
    // The first update must restart the never-written probes (no hysteresis against the cleared atlas) and record
    // which world probe each slot holds.
    const std::vector<u8> state = device->readBuffer(probe.volume.probeStateBuffer());
    const i32* si = reinterpret_cast<const i32*>(state.data());
    for (u32 i = 0; i < probe.volume.probeCount(); ++i) EXPECT_EQ(si[i * 4 + 3], 1) << "probe " << i;
    EXPECT_NEAR(mean, 2.0 / 3.0, 0.03);
    EXPECT_LT(worst, 0.08);
    // Constant radiance: irradiance / π equals the radiance for every normal; hysteresis converges to it.
    probe.mode = 1;
    render(cam, {.width = 96, .height = 96, .frames = 12});
    v = readRed16f(*device, probe.out);
    mean = 0.0;
    for (f32 x : v) mean += x;
    mean /= f64(v.size());
    std::printf("[ddgi] constant L = 0.5: mean %.4f\n", mean);
    EXPECT_NEAR(mean, 0.5, 0.02);
    // Scrolling: the window moves one probe along +x; the slots of the slice that left are re-assigned to the new
    // slice (state = new world coordinates), the others keep their probes.
    probe.center.x += 2.0f;
    render(camera({2.0f, 2.5f, 0.01f}, {2.0f, 0.0f, 0.0f}, 11.0f, 50.0f), {.width = 96, .height = 96, .frames = 1});
    const std::vector<u8> moved = device->readBuffer(probe.volume.probeStateBuffer());
    const i32* ms = reinterpret_cast<const i32*>(moved.data());
    for (u32 i = 0; i < probe.volume.probeCount(); ++i) {
        const glm::ivec3 c{ms[i * 4], ms[i * 4 + 1], ms[i * 4 + 2]};
        EXPECT_EQ(c, rt::ddgiWorldCoord(i, probe.last.minCoord, probe.last.counts)) << "slot " << i;
        EXPECT_TRUE(glm::all(glm::greaterThanEqual(c, probe.last.minCoord)) &&
                    glm::all(glm::lessThan(c, probe.last.minCoord + probe.last.counts)));
    }
    v = readRed16f(*device, probe.out);
    mean = 0.0;
    for (f32 x : v) mean += x;
    EXPECT_NEAR(mean / f64(v.size()), 0.5, 0.03) << "probes that stayed keep their (constant) history";
}

// --- ReSTIR DI resampling (the reservoir passes need no ray tracing) ---

namespace {

struct RestirProbeFeature final : IRenderFeature {
    rhi::PipelineHandle initial, spatial;
    rhi::BufferHandle res[3];
    u64 bytes = 0;
    u32 frame = 0;
    u32 lightCount = 2;
    bool temporal = false;
    Extent2D extent;

    std::string_view name() const override { return "TestRestirProbe"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    bool initialize(FeatureInitContext& ctx) override {
        initial = createComputePipeline(ctx.device, "test.restir.initial", "render/raytracing/restir_initial.comp");
        spatial = createComputePipeline(ctx.device, "test.restir.spatial", "render/raytracing/restir_spatial.comp");
        return ctx.device.vkPipeline(initial) != VK_NULL_HANDLE && ctx.device.vkPipeline(spatial) != VK_NULL_HANDLE;
    }
    void shutdown(rhi::Device& d) override {
        d.destroy(initial);
        d.destroy(spatial);
        for (auto& b : res) {
            if (b) d.destroy(b);
        }
    }
    void setup(FeatureContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const Extent2D e = ctx.renderExtent();
        const u64 need = u64(e.width) * e.height * 32;
        if (bytes != need) {
            for (auto& b : res) {
                if (b) dev.destroy(b);
                b = dev.createBuffer({need, rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc,
                                      rhi::MemoryUsage::GpuOnly, "test.reservoirs"});
            }
            bytes = need;
        }
        extent = e;
        std::vector<u32> lights(lightCount);
        for (u32 i = 0; i < lightCount; ++i) lights[i] = i; // no directional light: local lights start at 0
        const VkDeviceAddress list = ctx.upload(std::span<const u32>(lights));
        const rhi::BufferDesc bd{need, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly, "Reservoirs"};
        const rhi::RGBuffer prev = ctx.graph().importBuffer(res[2], bd, {rhi::Access::General, rhi::Access::Undefined});
        const rhi::RGBuffer cur = ctx.graph().importBuffer(res[0], bd, {rhi::Access::General, rhi::Access::Undefined});
        const rhi::RGBuffer out = ctx.graph().importBuffer(res[1], bd, {rhi::Access::General, rhi::Access::Undefined});
        const rhi::RGTexture depth = ctx.resources().texture(res::kDepth), normals = ctx.resources().texture(res::kNormals),
                             velocity = ctx.resources().texture(res::kVelocity);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        const u32 f = frame++, n = lightCount, t = temporal ? 1u : 0u;
        ctx.graph()
            .addPass("Test.ReSTIR.Initial", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .read(velocity, rhi::Access::SampledCompute)
            .read(prev, rhi::Access::StorageReadCompute)
            .overwrite(cur, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                struct {
                    u64 view, scene, prevRes, curRes, lightList;
                    u32 depth, normals, velocity, lightCount, width, height, scale, frame, candidates, flags;
                } pc{view, scene, p.address(prev), p.address(cur), list, p.sampledIndex(depth), p.sampledIndex(normals),
                     p.sampledIndex(velocity), n, e.width, e.height, 1, f, 32, t};
                p.cmd.bindPipeline(initial);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((e.width + 7) / 8, (e.height + 7) / 8);
            });
        ctx.graph()
            .addPass("Test.ReSTIR.Spatial", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .read(cur, rhi::Access::StorageReadCompute)
            .overwrite(out, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                struct {
                    u64 view, scene, src, dst;
                    u32 depth, normals, width, height, scale, frame, samples;
                    f32 radius;
                } pc{view, scene, p.address(cur), p.address(out), p.sampledIndex(depth), p.sampledIndex(normals),
                     e.width, e.height, 1, f, 4, 16.0f};
                p.cmd.bindPipeline(spatial);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch((e.width + 7) / 8, (e.height + 7) / 8);
            });
        // Spatial output becomes next frame's temporal input.
        ctx.graph()
            .addPass("Test.ReSTIR.History", rhi::PassType::Transfer)
            .read(out, rhi::Access::TransferRead)
            .overwrite(prev, rhi::Access::TransferWrite)
            .execute([=](rhi::PassContext& p) { p.cmd.copyBuffer(p.buffer(out), p.buffer(prev), need); });
    }
};

struct ReservoirCpu {
    u32 light, entity;
    f32 wSum, M, W, z;
    glm::vec2 normal;
};
static_assert(sizeof(ReservoirCpu) == 32);

} // namespace

TEST_F(RayTracingTest, RestirReservoirsSampleLightsProportionallyToTheirContribution) {
    mesh(Primitive::Plane, material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.9f), {0, 0, 0}, glm::vec3(30.0f));
    // Two lights symmetric around the view centre, 4:1 in power → the target function ratio at the centre is ~4:1.
    pointLight({-1.0f, 1.0f, 0.0f}, 4000.0f, 12.0f, glm::vec3(1.0f), true);
    pointLight({1.0f, 1.0f, 0.0f}, 1000.0f, 12.0f, glm::vec3(1.0f), true);
    auto& probe = renderer->features().emplace<RestirProbeFeature>();
    const CameraParams cam = camera({0.0f, 3.0f, 0.01f}, {0.0f, 0.0f, 0.0f}, 9.0f, 40.0f);
    const u32 size = 64;
    auto measure = [&](f64& brightFraction, f64& meanM) {
        const std::vector<u8> bytes = device->readBuffer(probe.res[1]);
        const auto* r = reinterpret_cast<const ReservoirCpu*>(bytes.data());
        u32 bright = 0, total = 0;
        meanM = 0.0;
        // Central 8×8 pixels: geometry is (almost) symmetric, so the selection frequency is the power ratio.
        for (u32 y = size / 2 - 4; y < size / 2 + 4; ++y) {
            for (u32 x = size / 2 - 4; x < size / 2 + 4; ++x) {
                const ReservoirCpu& v = r[y * size + x];
                EXPECT_LT(v.light, 2u);
                EXPECT_GT(v.W, 0.0f);
                EXPECT_GT(v.z, 0.0f);
                bright += v.light == 0 ? 1u : 0u;
                meanM += v.M;
                ++total;
            }
        }
        // Light buffer order follows the snapshot (not creation) order: report the more frequently chosen light.
        brightFraction = std::max(f64(bright) / total, 1.0 - f64(bright) / total);
        meanM /= total;
    };
    f64 fraction = 0.0, m = 0.0, sum = 0.0;
    const int runs = 24;
    for (int i = 0; i < runs; ++i) {
        render(cam, {.width = size, .height = size, .frames = 1});
        measure(fraction, m);
        sum += fraction;
    }
    std::printf("[restir] spatial only: bright light chosen %.3f (expected ~0.8), mean M %.1f\n", sum / runs, m);
    EXPECT_NEAR(sum / runs, 0.8, 0.06);
    EXPECT_GE(m, 32.0) << "initial candidates + spatial neighbours";
    // Temporal reuse keeps the distribution and grows M (clamped to 20× the initial count).
    probe.temporal = true;
    sum = 0.0;
    for (int i = 0; i < runs; ++i) {
        render(cam, {.width = size, .height = size, .frames = 1});
        measure(fraction, m);
        sum += fraction;
    }
    std::printf("[restir] temporal + spatial: bright light chosen %.3f, mean M %.1f\n", sum / runs, m);
    EXPECT_NEAR(sum / runs, 0.8, 0.08);
    EXPECT_GT(m, 100.0);
    EXPECT_LT(m, 32.0 * 21.0 * 5.0 + 1.0) << "temporal M is clamped to 20× the initial candidates";
}

// --- gating on devices without ray tracing ---

TEST_F(RayTracingTest, RayTracingCheckboxIsInertWithoutSupport) {
    if (hasRayTracing(*device)) GTEST_SKIP() << "checks the fallback path of devices without ray tracing";
    aoScene();
    sun({-0.4f, -0.8f, -0.3f}, 20000.0f);
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 13.0f, 55.0f);
    const Image raster = render(cam);
    const u32 passes = renderer->stats().renderGraphPasses;
    rt::RayTracingSceneApi* api = rt::RayTracingSceneApi::find(renderer->features());
    ASSERT_NE(api, nullptr) << "the raytracing area is registered";
    {
        CVarScope on("r.RayTracing", "true");
        CVarScope pt("r.PathTracing", "true");
        const Image rt = render(cam);
        EXPECT_EQ(renderer->stats().renderGraphPasses, passes) << "no ray tracing pass may be added";
        EXPECT_FALSE(renderer->settings().rayTracing) << "forced off by the renderer";
        EXPECT_FALSE(api->activeThisFrame());
        EXPECT_EQ(api->sceneHeaderAddress(), 0u);
        EXPECT_LT(meanAbs(raster, rt), 0.01) << "raster features stay active and unchanged";
    }
    const rt::RtStatus st = rt::rayTracingStatus(device->caps());
    EXPECT_FALSE(st.available);
    EXPECT_FALSE(st.reason.empty());
    std::printf("[rt] unavailable: %s\n", st.reason.c_str());
}

// --- ray traced effects (need an RT capable GPU) ---

TEST_F(RayTracingTest, RtShadowsMatchShadowMapsLoosely) {
    OX_REQUIRE_RT();
    aoScene();
    sun({-0.4f, -0.8f, -0.3f}, 20000.0f);
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 13.0f, 55.0f);
    const Image raster = render(cam, {.frames = 4});
    CVarScope on("r.RayTracing", "true");
    CVarScope off("r.RayTracing.AO", "false"), r("r.RayTracing.Reflections", "false"), g("r.RayTracing.GI", "false");
    const Image rt = render(cam, {.frames = 24});
    dump("rt_shadows_raster", raster);
    dump("rt_shadows", rt);
    EXPECT_TRUE(rt::RayTracingSceneApi::find(renderer->features())->activeThisFrame());
    // Same lighting model; shadow edges differ (PCSS vs cone-sampled rays): loose image tolerance.
    EXPECT_LT(meanAbs(raster, rt), 10.0);
    // The cube's shadow on the floor exists in both (the sun comes from +x+z... behind-right).
    EXPECT_LT(meanLuminance(rt, 30, 150, 60, 180), meanLuminance(rt, 200, 220, 240, 250) * 1.5);
}

TEST_F(RayTracingTest, RtAmbientOcclusionDarkensContacts) {
    OX_REQUIRE_RT();
    aoScene();
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 11.0f, 55.0f);
    CVarScope on("r.RayTracing", "true");
    CVarScope s("r.RayTracing.Shadows", "false"), r("r.RayTracing.Reflections", "false"), g("r.RayTracing.GI", "false");
    const Image noAo = [&] {
        CVarScope a("r.RayTracing.AO", "false");
        CVarScope f("r.Feature.AmbientOcclusion", "false");
        return render(cam, {.frames = 4});
    }();
    const Image ao = render(cam, {.frames = 24});
    dump("rt_ao_off", noAo);
    dump("rt_ao", ao);
    EXPECT_LT(meanLuminance(ao, 0, 0, ao.width, ao.height), meanLuminance(noAo, 0, 0, ao.width, ao.height));
    EXPECT_GT(meanAbs(ao, noAo), 0.5) << "AO must change the image";
}

TEST_F(RayTracingTest, RtReflectionsSeeOffscreenObjects) {
    OX_REQUIRE_RT();
    mesh(Primitive::Plane, material({0.9f, 0.9f, 0.9f, 1.0f}, 1.0f, 0.05f), {0, 0, 0}, glm::vec3(30.0f));
    // A bright red sphere in front of the camera but above the frame: invisible on screen, visible only in a ray
    // traced reflection (a flat floor mirrors what is above it, never what is behind the camera).
    mesh(Primitive::Sphere, material({1, 0, 0, 1}, 0.0f, 0.5f, glm::vec3(4.0f, 0.0f, 0.0f)), {0, 5.0f, -6}, glm::vec3(2.0f));
    environment(0.2f, 0.2f);
    const CameraParams cam = camera({0, 1.0f, 4}, {0, 0.0f, 0}, 9.0f, 50.0f);
    const Image raster = render(cam, {.frames = 4});
    CVarScope on("r.RayTracing", "true");
    const Image rt = render(cam, {.frames = 24});
    dump("rt_reflections_raster", raster);
    dump("rt_reflections", rt);
    const glm::vec3 floorRt = meanColor(rt, 96, 200, 160, 250), floorRaster = meanColor(raster, 96, 200, 160, 250);
    EXPECT_GT(floorRt.r - floorRt.g, floorRaster.r - floorRaster.g + 0.05f) << "red reflection of the off-screen sphere";
}

TEST_F(RayTracingTest, RtGlobalIlluminationBleedsColour) {
    OX_REQUIRE_RT();
    // A sunlit red wall next to a white floor. The wall is large against the DDGI probe spacing (2 m) and no probe
    // lies inside it (probes sit at odd coordinates): a 2 m wall is below what the probe grid resolves.
    mesh(Primitive::Plane, material({0.9f, 0.9f, 0.9f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(30.0f));
    mesh(Primitive::Cube, material({0.9f, 0.05f, 0.05f, 1.0f}, 0.0f, 0.8f), {-2.0f, 3.0f, 0.0f}, {0.4f, 6.0f, 12.0f});
    sun({-0.6f, -0.7f, -0.2f}, 30000.0f); // lights the wall face the camera sees
    environment(0.1f, 1.0f);
    const CameraParams cam = camera({4.5f, 3.0f, 4.0f}, {-2.0f, 0.8f, 0.0f}, 13.0f, 55.0f);
    const Image raster = render(cam, {.frames = 4});
    CVarScope on("r.RayTracing", "true");
    const Image rt = render(cam, {.frames = 48});
    const Image reference = [&] {
        CVarScope pt("r.PathTracing", "true");
        return render(cam, {.frames = 768});
    }();
    dump("rt_gi_raster", raster);
    dump("rt_gi", rt);
    dump("rt_gi_path_traced", reference);
    // Floor in front of the red wall picks up red light: red excess over green, against the path traced reference.
    auto excess = [](const Image& img) {
        const glm::vec3 c = meanColor(img, 40, 190, 140, 250);
        return c.r / std::max(c.g, 1e-3f) - 1.0f;
    };
    const f32 eRaster = excess(raster), eRt = excess(rt), eRef = excess(reference);
    std::printf("[rt] gi red excess on the floor: raster %.4f, ddgi %.4f, path traced %.4f\n", eRaster, eRt, eRef);
    EXPECT_GT(eRef, 0.03f) << "the scene bleeds colour at all";
    EXPECT_LT(std::abs(eRaster), 0.01f) << "raster has no bounce light";
    EXPECT_GT(eRt, eRef * 0.4f) << "DDGI carries the wall's colour onto the floor";
    EXPECT_LT(eRt, eRef * 1.6f) << "and does not overshoot the reference";
}

TEST_F(RayTracingTest, RtRefractionThroughGlass) {
    OX_REQUIRE_RT();
    mesh(Primitive::Plane, material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(30.0f));
    mesh(Primitive::Cube, material({0.1f, 0.2f, 0.9f, 1.0f}, 0.0f, 0.5f), {0, 0.5f, -2.0f});
    const Uuid glass = material({0.9f, 1.0f, 0.9f, 1.0f}, 0.0f, 0.0f, glm::vec3(0.0f), assets::BlendMode::Refractive);
    mesh(Primitive::Sphere, glass, {0, 0.6f, 0}, glm::vec3(1.2f));
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    environment();
    const CameraParams cam = camera({0, 1.0f, 3.0f}, {0, 0.6f, 0}, 13.0f, 50.0f);
    const Image raster = render(cam, {.frames = 2});
    CVarScope on("r.RayTracing", "true");
    const Image rt = render(cam, {.frames = 4});
    dump("rt_refraction_raster", raster);
    dump("rt_refraction", rt);
    EXPECT_GT(meanAbs(raster, rt), 0.3) << "ray traced refraction replaces the screen-space one";
    EXPECT_LT(meanAbs(raster, rt), 25.0) << "but stays in the same ballpark";
}

TEST_F(RayTracingTest, PathTracerConvergesAndResetsOnCameraChange) {
    OX_REQUIRE_RT();
    aoScene();
    sun({-0.4f, -0.8f, -0.3f}, 20000.0f);
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 13.0f, 55.0f);
    const Image raster = render(cam, {.frames = 4});
    CVarScope on("r.RayTracing", "true");
    CVarScope pt("r.PathTracing", "true");
    const Image few = render(cam, {.frames = 4});
    const Image many = render(cam, {.frames = 252});
    const Image more = render(cam, {.frames = 256});
    dump("rt_path_tracer_raster", raster);
    dump("rt_path_tracer_4spp", few);
    dump("rt_path_tracer", more);
    EXPECT_LT(meanAbs(many, more), meanAbs(few, many)) << "progressive accumulation converges";
    EXPECT_LT(meanAbs(many, more), 1.5);
    EXPECT_LT(meanAbs(raster, more), 25.0) << "reference and raster agree loosely (GI, soft shadows differ)";
    // Moving the camera restarts the accumulation: one frame later the image is noisy again.
    const CameraParams moved = camera({0.35f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 13.0f, 55.0f);
    const Image after = render(moved, {.frames = 1});
    const Image afterMany = render(moved, {.frames = 128});
    EXPECT_GT(meanAbs(after, afterMany), meanAbs(many, more) * 2.0) << "accumulation reset (no smeared history)";
}

// The path tracer replaces SceneColorHDR: every hybrid effect (and its SVGF stages) skips itself under it — no
// pass, no target — and comes back when the path tracer is switched off.
TEST_F(RayTracingTest, HybridEffectsSkipUnderPathTracer) {
    OX_REQUIRE_RT();
    const Uuid grey = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.6f);
    mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(30.0f));
    mesh(Primitive::Cube, material({0.9f, 0.9f, 0.9f, 1.0f}, 1.0f, 0.1f), {-0.8f, 0.5f, 0.0f}); // mirror-like metal
    mesh(Primitive::Sphere, grey, {0.2f, 0.4f, 1.0f}, glm::vec3(0.8f));
    const Uuid glass = material({0.9f, 1.0f, 0.9f, 1.0f}, 0.0f, 0.0f, glm::vec3(0.0f), assets::BlendMode::Refractive);
    mesh(Primitive::Sphere, glass, {1.2f, 0.6f, 0.0f}, glm::vec3(1.0f));
    pointLight({-2.0f, 2.0f, 1.0f}, 20000.0f, 8.0f, glm::vec3(1.0f), true); // local light shadows
    sun({-0.4f, -0.8f, -0.3f}, 20000.0f);
    Entity env = environment();
    auto& e = env.get<EnvironmentComponent>();
    e.fogEnabled = true; // volumetric fog: RT visibility
    e.fogDensity = 0.02f;
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 13.0f, 55.0f);
    // Hybrid effect passes and the SVGF stages of their signals.
    const std::vector<std::string_view> hybrid = {"RT.Shadows",     "RtShadowSun",    "RtShadowLocal", "RT.AO", "RtAO",
                                                  "RT.DDGI",        "RT.Reflections", "RtReflections",
                                                  "RT.VolumetricVisibility", "RT.Translucency"};
    CVarScope on("r.RayTracing", "true");
    rhi::GpuMemoryStats underPathTracer{};
    {
        CVarScope pt("r.PathTracing", "true");
        render(cam, {.frames = 3});
        EXPECT_TRUE(ranPass("RT.PathTrace")) << "the path tracer runs";
        for (std::string_view h : hybrid) EXPECT_FALSE(ranPass(h)) << h << " runs under the path tracer";
        device->waitIdle();
        underPathTracer = device->memoryStats();
    }
    render(cam, {.frames = 3});
    EXPECT_FALSE(ranPass("RT.PathTrace"));
    for (std::string_view h : hybrid) EXPECT_TRUE(ranPass(h)) << h << " is missing from the hybrid frame";
    device->waitIdle();
    const rhi::GpuMemoryStats hybridMem = device->memoryStats();
    std::printf("[rt] textures under the path tracer %u (%.1f MB), hybrid %u (%.1f MB)\n", underPathTracer.textureCount,
                f64(underPathTracer.totalUsageBytes) / 1048576.0, hybridMem.textureCount,
                f64(hybridMem.totalUsageBytes) / 1048576.0);
    EXPECT_GT(hybridMem.textureCount, underPathTracer.textureCount)
        << "the hybrid targets (denoiser histories, local shadow mask) were not allocated under the path tracer";
    {
        CVarScope pt("r.PathTracing", "true");
        render(cam, {.frames = 3});
        EXPECT_TRUE(ranPass("RT.PathTrace"));
        for (std::string_view h : hybrid) EXPECT_FALSE(ranPass(h)) << h << " runs after switching back";
        device->waitIdle();
        EXPECT_LE(device->memoryStats().textureCount, hybridMem.textureCount);
    }
}

TEST_F(RayTracingTest, RestirShadowsManyLights) {
    OX_REQUIRE_RT();
    aoScene(0.003f); // dim sky: the local lights dominate, their shadows are what the test looks at
    // 32 shadow casting point lights as a panel on the left (-x): together they act as an area light, the
    // occluders throw soft shadows to the right.
    for (int i = 0; i < 32; ++i) {
        pointLight({-3.5f, 1.0f + f32(i / 8) * 0.6f, -1.75f + f32(i % 8) * 0.5f}, 16000.0f, 10.0f, glm::vec3(1.0f), true);
    }
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 9.0f, 55.0f);
    CVarScope on("r.RayTracing", "true");
    const Image unshadowed = [&] {
        CVarScope s("r.Shadows", "false");
        return render(cam, {.frames = 16});
    }();
    const Image fourLights = render(cam, {.frames = 32}); // without ReSTIR only MaxLocalLights (4) are traced
    const Image reference = [&] {
        CVarScope pt("r.PathTracing", "true");
        return render(cam, {.frames = 768});
    }();
    CVarScope restir("r.RayTracing.Shadows.ReSTIR", "true");
    const Image a = render(cam, {.frames = 32});
    const Image b = render(cam, {.frames = 8});
    dump("rt_restir_unshadowed", unshadowed);
    dump("rt_restir_off_4_lights", fourLights);
    dump("rt_restir", b);
    dump("rt_restir_path_traced", reference);
    // Floor right of the sphere (shadowed from the whole panel) against the open floor in front (lit by all).
    auto shadowRatio = [](const Image& img) {
        return meanLuminance(img, 150, 150, 185, 170) / std::max(meanLuminance(img, 20, 190, 80, 250), 1e-3);
    };
    const f64 rUnshadowed = shadowRatio(unshadowed), rFour = shadowRatio(fourLights), rRestir = shadowRatio(a),
              rRef = shadowRatio(reference);
    std::printf("[rt] restir shadow/lit luminance: unshadowed %.3f, 4 lights %.3f, restir %.3f, path traced %.3f\n",
                rUnshadowed, rFour, rRestir, rRef);
    EXPECT_LT(rRestir, rUnshadowed * 0.6) << "occluders cast shadows from many lights";
    EXPECT_LT(rRestir, rFour * 0.7) << "ReSTIR shadows every light, not only the 4 explicit ones";
    EXPECT_NEAR(rRestir, rRef, 0.12) << "shadow depth matches the path traced reference";
    EXPECT_NEAR(meanLuminance(a, 20, 190, 80, 250), meanLuminance(unshadowed, 20, 190, 80, 250), 0.03)
        << "unoccluded surfaces keep their lighting";
    EXPECT_LT(meanAbs(a, b), 3.0) << "temporally stable after denoising";
}

TEST_F(RayTracingTest, RuntimeToggleRebuildsGraphWithoutLeaks) {
    OX_REQUIRE_RT();
    aoScene();
    sun({-0.4f, -0.8f, -0.3f}, 20000.0f);
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 13.0f, 55.0f);
    render(cam, {.frames = 2});
    const u32 rasterPasses = renderer->stats().renderGraphPasses;
    rhi::GpuMemoryStats first{};
    for (int i = 0; i < 4; ++i) {
        {
            CVarScope on("r.RayTracing", "true");
            render(cam, {.frames = 2});
            EXPECT_NE(renderer->stats().renderGraphPasses, rasterPasses);
            rt::RayTracingSceneApi* api = rt::RayTracingSceneApi::find(renderer->features());
            ASSERT_NE(api, nullptr);
            EXPECT_EQ(api->tlasInstanceCount(), renderer->scene().liveInstanceCount());
            EXPECT_GE(api->blasStats().resident, 4u);
        }
        render(cam, {.frames = 2});
        EXPECT_EQ(renderer->stats().renderGraphPasses, rasterPasses);
        device->waitIdle();
        if (i == 1) first = device->memoryStats();
    }
    render(cam, {.frames = 4});
    device->waitIdle();
    const rhi::GpuMemoryStats last = device->memoryStats();
    EXPECT_LE(last.textureCount, first.textureCount + 2);
    EXPECT_LE(last.bufferCount, first.bufferCount + 4);
}

// --- path tracer: RT pipeline (shader binding table) against the ray query megakernel ---

TEST_F(RayTracingTest, PathTracerPipelineMatchesRayQuery) {
    OX_REQUIRE_RT();
    if (!device->caps().rayTracingPipeline) GTEST_SKIP() << "needs VK_KHR_ray_tracing_pipeline";
    // Every hit group is in the picture: opaque, alpha-tested (any-hit on radiance and shadow rays) and translucent.
    aoScene();
    mesh(Primitive::Plane, cutoutMaterial({0.9f, 0.5f, 0.1f}), {-0.4f, 1.3f, 1.6f}, glm::vec3(1.4f));
    // Hovering clear of the others: mode 1 traces opaque shadows only, the sphere's tinted shadow falls outside the view.
    mesh(Primitive::Sphere, material({0.9f, 0.3f, 0.3f, 0.5f}, 0.0f, 0.3f, glm::vec3(0.0f), assets::BlendMode::Transparent),
         {2.2f, 1.6f, -1.5f}, glm::vec3(0.8f));
    sun({-0.4f, -0.8f, -0.3f}, 20000.0f);
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 13.0f, 55.0f);
    CVarScope on("r.RayTracing", "true");
    CVarScope pt("r.PathTracing", "true");
    CVarScope spf("r.PathTracing.SamplesPerFrame", "4");
    const Image query = render(cam, {.frames = 128});
    EXPECT_TRUE(ranPass("RT.PathTrace")) << "ray query kernel";
    EXPECT_FALSE(ranPass("RT.PathTrace.Pipeline"));
    CVarScope mode("r.PathTracing.Mode", "1");
    const Image pipeline = render(cam, {.frames = 128});
    EXPECT_TRUE(ranPass("RT.PathTrace.Pipeline")) << "RT pipeline with the shader binding table";
    dump("rt_path_tracer_mode0", query);
    dump("rt_path_tracer_mode1", pipeline);
    const f64 diff = meanAbs(query, pipeline);
    std::printf("[rt] path tracer mode 0 vs mode 1: mean abs %.3f\n", diff);
    EXPECT_GT(meanLuminance(pipeline, 0, 0, 256, 256), 0.05) << "the pipeline renders an image";
    EXPECT_LT(diff, 1.5) << "both implementations trace the same paths (512 spp each, independent noise)";
    // The cutout's shadow on the floor has the same holes in both (any-hit on shadow rays).
    EXPECT_NEAR(meanLuminance(query, 10, 150, 90, 210), meanLuminance(pipeline, 10, 150, 90, 210), 0.02);
}

// --- shadows: local lights without ReSTIR, alpha-tested casters, coloured transmission ---

TEST_F(RayTracingTest, RtLocalLightShadowsWithoutRestir) {
    OX_REQUIRE_RT();
    aoScene(0.003f);
    pointLight({-2.5f, 3.0f, 2.0f}, 60000.0f, 14.0f, glm::vec3(1.0f), true);
    spotLight({3.0f, 4.0f, 1.0f}, {-3.0f, -4.0f, -1.0f}, 60000.0f, 14.0f, 25.0f, 40.0f, true, {1.0f, 0.8f, 0.6f});
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 9.0f, 55.0f);
    const Image raster = render(cam, {.frames = 4});
    CVarScope on("r.RayTracing", "true");
    CVarScope a("r.RayTracing.AO", "false"), r("r.RayTracing.Reflections", "false"), g("r.RayTracing.GI", "false");
    const Image unshadowed = [&] {
        CVarScope s("r.Shadows", "false");
        return render(cam, {.frames = 8});
    }();
    const Image rt = render(cam, {.frames = 24});
    EXPECT_TRUE(ranPass("RT.Shadows"));
    EXPECT_TRUE(ranPass("RT.Shadows.PublishLocal")) << "explicit local light channels";
    EXPECT_FALSE(ranPass("RT.ReSTIR")) << "ReSTIR is off by default";
    dump("rt_local_shadows_raster", raster);
    dump("rt_local_shadows_unshadowed", unshadowed);
    dump("rt_local_shadows", rt);
    std::printf("[rt] local shadows: raster vs rt %.3f, unshadowed vs rt %.3f\n", meanAbs(raster, rt), meanAbs(unshadowed, rt));
    // Same lights, same occluders: the ray traced shadows land where the shadow maps put them.
    EXPECT_LT(meanAbs(raster, rt), 6.0);
    EXPECT_GT(meanAbs(unshadowed, rt), meanAbs(raster, rt) * 2.0) << "shadows are present";
}

TEST_F(RayTracingTest, RtShadowsOfAlphaTestedAndTintedCasters) {
    OX_REQUIRE_RT();
    mesh(Primitive::Plane, material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(30.0f));
    // Three plates hovering over the floor, sun from above: opaque, alpha-tested checker, green glass.
    const glm::vec3 plate{1.2f};
    mesh(Primitive::Plane, material({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f), {-1.6f, 1.0f, 0.0f}, plate);
    mesh(Primitive::Plane, cutoutMaterial({0.5f, 0.5f, 0.5f}), {0.0f, 1.0f, 0.0f}, plate);
    assets::MaterialAsset glass;
    glass.baseColor = {0.15f, 0.9f, 0.2f, 1.0f};
    glass.roughness = 0.05f;
    glass.blendMode = assets::BlendMode::Refractive;
    mesh(Primitive::Plane, materialEx(glass), {1.6f, 1.0f, 0.0f}, plate);
    sun({0.0f, -1.0f, -0.35f}, 20000.0f);
    environment(0.2f, 1.0f);
    // Looking down from the front: the shadows lie behind the plates (-z), in view above them.
    const CameraParams cam = camera({0.0f, 4.5f, 3.2f}, {0.0f, 0.0f, -0.6f}, 13.0f, 50.0f);
    const Image raster = render(cam, {.frames = 4});
    CVarScope on("r.RayTracing", "true");
    CVarScope a("r.RayTracing.AO", "false"), r("r.RayTracing.Reflections", "false"), g("r.RayTracing.GI", "false");
    const Image rt = render(cam, {.frames = 24});
    const Image plain = [&] {
        CVarScope c("r.RayTracing.Shadows.Colored", "false");
        return render(cam, {.frames = 200}); // long enough for the denoiser history to forget the tinted shadow
    }();
    dump("rt_shadow_casters_raster", raster);
    dump("rt_shadow_casters", rt);
    dump("rt_shadow_casters_uncoloured", plain);
    // Floor regions: open floor, then the shadows of the opaque, the alpha-tested and the glass plate.
    auto lit = [](const Image& i) { return meanColor(i, 100, 200, 160, 250); };
    auto opaque = [](const Image& i) { return meanColor(i, 30, 149, 64, 157); };
    auto cutout = [](const Image& i) { return meanColor(i, 104, 146, 150, 160); };
    auto tinted = [](const Image& i) { return meanColor(i, 184, 149, 236, 157); };
    auto lum = [](glm::vec3 c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); };
    std::printf("[rt] caster shadows (luminance): lit %.3f, opaque %.3f, alpha-tested %.3f (raster %.3f), glass rgb "
                "%.3f %.3f %.3f, glass uncoloured %.3f\n",
                lum(lit(rt)), lum(opaque(rt)), lum(cutout(rt)), lum(cutout(raster)), tinted(rt).r, tinted(rt).g, tinted(rt).b,
                lum(tinted(plain)));
    EXPECT_LT(lum(opaque(rt)), lum(lit(rt)) * 0.3f) << "opaque plate: full shadow";
    // Half of the checker is cut out: its shadow lets about half of the sun through.
    EXPECT_GT(lum(cutout(rt)), lum(opaque(rt)) + 0.15f) << "alpha-tested shadow has holes";
    EXPECT_LT(lum(cutout(rt)), lum(lit(rt)) - 0.15f) << "and solid cells";
    EXPECT_NEAR(lum(cutout(rt)), lum(cutout(raster)), 0.08f) << "like the alpha-tested shadow map";
    // Green glass: the shadow is green (red and blue absorbed), not grey.
    EXPECT_GT(tinted(rt).g, tinted(rt).r * 1.8f);
    EXPECT_GT(tinted(rt).g, tinted(rt).b * 1.5f);
    EXPECT_LT(lum(tinted(rt)), lum(lit(rt)) - 0.03f);
    // Without coloured transmission translucent casters throw no shadow at all.
    EXPECT_NEAR(lum(tinted(plain)), lum(lit(plain)), 0.02f);
    EXPECT_NEAR(tinted(plain).g / tinted(plain).r, lit(plain).g / lit(plain).r, 0.02f);
}

// --- volumetric fog: ray traced froxel visibility ---

TEST_F(RayTracingTest, RtVolumetricFogVisibility) {
    OX_REQUIRE_RT();
    // Fog under a roof: the sun comes in at an angle, the volume below the slab is in shadow.
    const Uuid stone = material({0.55f, 0.52f, 0.48f, 1.0f}, 0.0f, 0.8f);
    mesh(Primitive::Plane, stone, {0, 0, 0}, glm::vec3(80.0f));
    mesh(Primitive::Cube, stone, {0.0f, 5.0f, -8.0f}, {14.0f, 0.4f, 10.0f});
    sun(glm::normalize(glm::vec3(0.5f, -1.0f, 0.1f)), 60000.0f);
    Entity e = environment(0.25f, 1.0f);
    auto& env = e.get<EnvironmentComponent>();
    env.fogEnabled = true;
    env.fogDensity = 0.06f;
    env.fogHeightFalloff = 0.02f;
    env.fogColor = glm::vec3(0.9f);
    const CameraParams cam = camera({0.0f, 2.0f, 12.0f}, {0.0f, 2.5f, -8.0f}, 13.5f, 55.0f);
    const Image raster = render(cam, {.frames = 24});
    CVarScope on("r.RayTracing", "true");
    const Image without = [&] {
        CVarScope v("r.RayTracing.Volumetrics", "false");
        const Image img = render(cam, {.frames = 48});
        EXPECT_FALSE(ranPass("RT.VolumetricVisibility"));
        return img;
    }();
    const Image with = render(cam, {.frames = 48});
    EXPECT_TRUE(ranPass("RT.VolumetricVisibility"));
    dump("rt_fog_raster", raster);
    dump("rt_fog_without_visibility", without);
    dump("rt_fog", with);
    // Fog below the roof. Ray traced shadows replace the shadow maps the fog would sample: without the ray traced
    // visibility the fog there is fully lit, with it the roof shadows the fog like the shadow maps do in raster.
    const f64 lRaster = meanLuminance(raster, 60, 100, 200, 140), lWithout = meanLuminance(without, 60, 100, 200, 140),
              lWith = meanLuminance(with, 60, 100, 200, 140);
    std::printf("[rt] fog under the roof: raster %.3f, rt without visibility %.3f, with %.3f; images raster vs rt %.3f\n",
                lRaster, lWithout, lWith, meanAbs(raster, with));
    EXPECT_GT(meanAbs(without, with), 0.5) << "the visibility pass changes the image";
    EXPECT_LT(lWith, lWithout - 0.03) << "the roof shadows the fog";
    EXPECT_NEAR(lWith, lRaster, 0.02) << "like the shadow mapped fog";
    EXPECT_NEAR(meanLuminance(with, 60, 20, 200, 60), meanLuminance(raster, 60, 20, 200, 60), 0.02) << "sunlit fog unchanged";
}

// --- TLAS over many frames with moving instances ---

TEST_F(RayTracingTest, TlasFollowsMovingInstancesWithoutLeaks) {
    OX_REQUIRE_RT();
    aoScene();
    sun({-0.4f, -0.8f, -0.3f}, 20000.0f);
    const Uuid red = material({0.8f, 0.2f, 0.2f, 1.0f}, 0.0f, 0.6f);
    std::vector<Entity> movers;
    for (int i = 0; i < 6; ++i) movers.push_back(mesh(Primitive::Cube, red, {0, 0, 0}, glm::vec3(0.35f)));
    // The cubes circle above the floor: their shadows sweep over it.
    auto place = [&](f32 t) {
        for (usize i = 0; i < movers.size(); ++i) {
            const f32 a = t + f32(i) * 1.047f;
            movers[i].setPosition({std::cos(a) * 1.8f, 1.6f + 0.3f * std::sin(a * 3.0f), std::sin(a) * 1.8f});
        }
    };
    const CameraParams cam = camera({0.3f, 2.6f, 4.2f}, {0.2f, 0.3f, 0.0f}, 13.0f, 55.0f);
    constexpr f32 kEnd = 300.0f * 0.02f;
    place(kEnd);
    const Image rasterEnd = render(cam, {.frames = 4});
    place(0.0f);
    const Image rasterStart = render(cam, {.frames = 4});

    CVarScope on("r.RayTracing", "true");
    CVarScope a("r.RayTracing.AO", "false"), r("r.RayTracing.Reflections", "false"), g("r.RayTracing.GI", "false");
    rt::RayTracingSceneApi* api = rt::RayTracingSceneApi::find(renderer->features());
    ASSERT_NE(api, nullptr);
    rhi::GpuMemoryStats early{};
    u32 residentEarly = 0;
    renderAnimated(cam, 300, [&](u32 f) {
        place(f32(f + 1) * 0.02f);
        if (f == 40) {
            early = device->memoryStats();
            residentEarly = api->blasStats().resident;
        }
    });
    EXPECT_EQ(api->tlasInstanceCount(), renderer->scene().liveInstanceCount());
    EXPECT_EQ(api->blasStats().resident, residentEarly) << "moving instances share their BLAS: nothing is rebuilt";
    device->waitIdle();
    const rhi::GpuMemoryStats late = device->memoryStats();
    std::printf("[rt] tlas over 300 frames: buffers %u -> %u, textures %u -> %u, VRAM %.1f -> %.1f MB\n", early.bufferCount,
                late.bufferCount, early.textureCount, late.textureCount, f64(early.totalUsageBytes) / 1048576.0,
                f64(late.totalUsageBytes) / 1048576.0);
    EXPECT_LE(late.bufferCount, early.bufferCount + 2);
    EXPECT_LE(late.textureCount, early.textureCount + 2);
    EXPECT_LE(late.totalUsageBytes, early.totalUsageBytes + (8u << 20)) << "VRAM stays flat";
    // The cubes stopped at the end position 8 frames ago: shadows are where the shadow maps put them, not where they
    // started and not smeared along the way (the denoiser drops the history of a shadow that moved on).
    const Image rtEnd = renderAnimated(cam, 8, [&](u32) {});
    dump("rt_tlas_moving_raster", rasterEnd);
    dump("rt_tlas_moving", rtEnd);
    std::printf("[rt] tlas moving: rt vs raster at the end position %.3f, vs the start position %.3f\n",
                meanAbs(rtEnd, rasterEnd), meanAbs(rtEnd, rasterStart));
    EXPECT_LT(meanAbs(rtEnd, rasterEnd), 3.0);
    EXPECT_GT(meanAbs(rtEnd, rasterStart), meanAbs(rtEnd, rasterEnd) * 2.0);
}

// --- GPU timings of every ray traced pass ---
// OX_RT_PERF=1 measures 1080p and 4K (OX_RT_PERF=<width>x<height>: one custom size); run it with
// OX_VULKAN_VALIDATION=0 for real numbers. Without the variable the same configurations run as a quick smoke test at
// 640x360. Configurations: raster baseline, the hybrid effects, + ReSTIR, path tracer in both modes. Prints the
// average over the measured frames of every pass of the ray tracing area.

TEST_F(RayTracingTest, PerfReportRayTracing) {
    OX_REQUIRE_RT();
    const char* perf = std::getenv("OX_RT_PERF");
    const bool measure = perf && *perf && std::string_view(perf) != "0";
    // 240 objects around the camera, a mirror-like floor patch, glass, 8 shadowed local lights, sun, fog.
    Random rng(11);
    mesh(Primitive::Plane, material({0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.8f), {0, 0, 0}, glm::vec3(120.0f));
    mesh(Primitive::Cube, material({0.9f, 0.9f, 0.9f, 1.0f}, 1.0f, 0.1f), {0.0f, 0.02f, 8.0f}, {16.0f, 0.04f, 10.0f});
    Entity mover;
    for (int i = 0; i < 240; ++i) {
        // One draw per statement: argument evaluation order differs between compilers.
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
    assets::MaterialAsset glass;
    glass.baseColor = {0.8f, 0.95f, 0.9f, 1.0f};
    glass.roughness = 0.02f;
    glass.blendMode = assets::BlendMode::Refractive;
    const Uuid glassId = materialEx(glass);
    for (int i = 0; i < 4; ++i) mesh(Primitive::Sphere, glassId, {-6.0f + 4.0f * f32(i), 1.2f, 10.0f}, glm::vec3(2.4f));
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
    if (measure) sizes = {{1920, 1080}, {3840, 2160}};
    if (unsigned w = 0, h = 0; measure && std::sscanf(perf, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) sizes = {{w, h}};
    const u32 warmUp = measure ? 48 : 4, frames = measure ? 8 : 2;
    std::printf("[rt-perf] %s, validation %s\n", device->caps().gpuName.c_str(), device->caps().validationEnabled ? "ON" : "off");
    auto report = [&](Extent2D size, const char* label) {
        const u32 kFrames = frames;
        render(cam, {.width = size.width, .height = size.height, .frames = warmUp}); // BLAS builds, histories, DDGI
        std::vector<std::pair<std::string, f64>> passes; // first-appearance order
        f64 frame = 0.0;
        for (u32 f = 0; f < kFrames; ++f) {
            mover.setPosition(moverStart + glm::vec3(0.0f, 0.01f * f32(f + 1), 0.0f)); // one moving instance: TLAS update
            render(cam, {.width = size.width, .height = size.height, .frames = 1});
            const RenderStats& st = renderer->stats();
            frame += st.gpuFrameMs / kFrames;
            for (const PassTiming& p : st.passes) {
                std::string n = p.name;
                if (auto slash = n.find('/'); slash != std::string::npos) n = n.substr(slash + 1); // "<view>/<pass>"
                if (!n.starts_with("RT.") && !n.starts_with("Rt")) continue;
                auto it = std::find_if(passes.begin(), passes.end(), [&](const auto& e) { return e.first == n; });
                if (it == passes.end()) it = passes.insert(passes.end(), {n, 0.0});
                it->second += p.gpuMs / kFrames;
            }
        }
        f64 rt = 0.0;
        for (const auto& [n, ms] : passes) rt += ms;
        const rhi::GpuMemoryStats mem = device->memoryStats();
        std::printf("[rt-perf] %ux%u %-22s GPU frame %7.3f ms, ray tracing passes %7.3f ms, VRAM %.0f MB\n", size.width,
                    size.height, label, frame, rt, f64(mem.totalUsageBytes) / 1048576.0);
        // Denoiser stages are summed per signal ("RtAO.Temporal" + ".Variance" + ".Atrous*" + ".Upsample").
        std::vector<std::pair<std::string, f64>> rows;
        for (const auto& [n, ms] : passes) {
            const std::string key = n.starts_with("Rt") ? n.substr(0, n.find('.')) + " (denoise)" : n;
            auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& e) { return e.first == key; });
            if (it == rows.end()) it = rows.insert(rows.end(), {key, 0.0});
            it->second += ms;
        }
        for (const auto& [n, ms] : rows) std::printf("[rt-perf]     %-34s %7.3f ms\n", n.c_str(), ms);
        return frame;
    };
    for (const Extent2D size : sizes) {
        const f64 raster = report(size, "raster");
        if (device->caps().timestampQueries) EXPECT_GT(raster, 0.0);
        CVarScope on("r.RayTracing", "true");
        report(size, "ray tracing (default)");
        {
            CVarScope restir("r.RayTracing.Shadows.ReSTIR", "true");
            report(size, "ray tracing + ReSTIR");
        }
        CVarScope pt("r.PathTracing", "true");
        report(size, "path tracer (mode 0)");
        CVarScope mode("r.PathTracing.Mode", "1");
        report(size, "path tracer (mode 1)");
    }
}
