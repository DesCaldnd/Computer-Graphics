// Neural texture compression in the renderer: r.NeuralTextures Off / DecodeOnLoad / InferenceOnSample.
#include "render_fixture.hpp"

#include <oxwald/assets/mesh_processing.hpp>
#include <oxwald/render/mesh_primitives.hpp>

#include <oxwald/assets/neural/ntc.hpp>
#include <oxwald/assets/texture_import.hpp>
#include <oxwald/core/time.hpp>
#include <oxwald/render/neural/neural.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <filesystem>
#include <fstream>
#include <numbers>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;
using namespace ox::render::neural;
namespace fs = std::filesystem;

namespace {

assets::NtcSourceImages proceduralMaterial(u32 size) {
    assets::NtcSourceImages s;
    s.albedo = assets::Image(size, size);
    s.normal = assets::Image(size, size);
    s.orm = assets::Image(size, size);
    const f32 tau = 2.0f * std::numbers::pi_v<f32>;
    auto height = [&](f32 u, f32 v) {
        const f32 tu = u * 4.0f, tv = v * 4.0f;
        const f32 gu = std::abs(tu - std::round(tu)), gv = std::abs(tv - std::round(tv));
        const f32 grout = std::min(gu, gv) < 0.05f ? 0.0f : 1.0f;
        return grout * (0.8f + 0.1f * std::sin(tau * u * 9.0f) * std::sin(tau * v * 7.0f));
    };
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 u = (f32(x) + 0.5f) / f32(size), v = (f32(y) + 0.5f) / f32(size);
            const f32 h = height(u, v);
            const u32 tile = u32(u * 4.0f) + 4u * u32(v * 4.0f);
            const f32 hue = f32((tile * 37u) % 11u) / 11.0f;
            const glm::vec3 c = h > 0.1f ? glm::vec3(0.6f + 0.3f * hue, 0.35f + 0.25f * (1.0f - hue), 0.2f + 0.1f * hue)
                                         : glm::vec3(0.25f, 0.25f, 0.24f);
            s.albedo.at(x, y) = glm::vec4(glm::clamp(c, 0.0f, 1.0f), 1.0f);
            const f32 e = 1.0f / f32(size);
            const f32 dx = (height(u + e, v) - height(u - e, v)) * 3.0f, dy = (height(u, v + e) - height(u, v - e)) * 3.0f;
            const glm::vec3 n = glm::normalize(glm::vec3(-dx, -dy, 1.0f));
            s.normal.at(x, y) = glm::vec4(n * 0.5f + 0.5f, 1.0f);
            s.orm.at(x, y) = glm::vec4(h > 0.1f ? 1.0f : 0.5f, h > 0.1f ? 0.35f + 0.3f * hue : 0.95f, 0.0f, 1.0f);
        }
    }
    return s;
}

f64 imagePsnr(const Image& a, const Image& b) {
    f64 mse = 0.0;
    usize n = 0;
    for (usize i = 0; i < a.rgba.size(); i += 4) {
        for (u32 c = 0; c < 3; ++c) {
            const f64 d = (f64(a.rgba[i + c]) - f64(b.rgba[i + c])) / 255.0;
            mse += d * d;
            ++n;
        }
    }
    mse /= f64(std::max<usize>(n, 1));
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}

fs::path outDir() {
    const fs::path d = fs::temp_directory_path() / "oxwald_render_out";
    fs::create_directories(d);
    return d;
}

class NeuralTexturesTest : public RenderTest {
protected:
    // Regular textures of `src` (BC7 / BC5 like the importer) under fresh UUIDs; returns a material using them.
    Uuid regularMaterial(const assets::NtcSourceImages& src, glm::vec2 tiling = glm::vec2(1.0f)) {
        assets::MaterialAsset m;
        m.uvTiling = tiling;
        auto add = [&](const assets::Image& img, assets::TextureType type, bool srgb) {
            assets::TextureImportSettings ts;
            ts.type = type;
            ts.srgb = srgb;
            auto tex = assets::importTexture(img, ts);
            EXPECT_TRUE(tex);
            const Uuid id = Uuid::generate();
            renderer->resources().addTexture(id, *tex);
            return id;
        };
        m.albedoTexture = add(src.albedo, assets::TextureType::Color, true);
        m.normalTexture = add(src.normal, assets::TextureType::Normal, false);
        m.ormTexture = add(src.orm, assets::TextureType::Linear, false);
        const Uuid id = Uuid::generate();
        renderer->resources().addMaterial(id, m);
        return id;
    }
    template <class M>
    Uuid regularMaterialFull(const M& mat) {
        assets::MaterialAsset m;
        m.uvTiling = mat.tiling;
        auto add = [&](const assets::Image& img, assets::TextureType type, bool srgb) {
            if (img.empty()) return Uuid{};
            assets::TextureImportSettings ts;
            ts.type = type;
            ts.srgb = srgb;
            auto tex = assets::importTexture(img, ts);
            EXPECT_TRUE(tex);
            const Uuid id = Uuid::generate();
            renderer->resources().addTexture(id, *tex);
            return id;
        };
        m.albedoTexture = add(mat.src.albedo, assets::TextureType::Color, true);
        m.normalTexture = add(mat.src.normal, assets::TextureType::Normal, false);
        m.ormTexture = add(mat.src.orm, assets::TextureType::Linear, false);
        m.emissiveTexture = add(mat.src.emissive, assets::TextureType::Color, true);
        if (m.emissiveTexture.isValid()) m.emissive = glm::vec3(1.0f);
        const Uuid id = Uuid::generate();
        renderer->resources().addMaterial(id, m);
        return id;
    }
    // `emissive`: the set carries an emissive channel (white emissive factor, like regularMaterialFull).
    Uuid neuralMaterial(std::shared_ptr<const assets::NeuralTextureSet> set, glm::vec2 tiling = glm::vec2(1.0f),
                        bool emissive = false) {
        const Uuid setId = Uuid::generate();
        addNeuralTextureSet(*renderer, setId, std::move(set));
        assets::MaterialAsset m;
        m.uvTiling = tiling;
        if (emissive) m.emissive = glm::vec3(1.0f);
        m.neuralTextures = setId;
        m.neuralTexturesOnly = true;
        const Uuid id = Uuid::generate();
        renderer->resources().addMaterial(id, m);
        return id;
    }
    std::shared_ptr<const assets::NeuralTextureSet> encode(const assets::NtcSourceImages& src, u32 steps = 900) {
        assets::NeuralTextureSettings s;
        s.preset = assets::NtcPreset::Custom;
        s.latentStride = 4;
        s.features0 = 8;
        s.features1 = 8;
        s.hidden = 32;
        s.steps = steps;
        s.batch = 2048;
        s.fallbackSize = 32;
        auto set = assets::encodeNeuralTextures(src, s);
        EXPECT_TRUE(set) << (set ? "" : set.error().message);
        return std::make_shared<const assets::NeuralTextureSet>(std::move(*set));
    }
    Image renderFlushed(const CameraParams& cam, Options o) {
        extractSnapshot();
        renderer->resources().flush();
        render(cam, {.width = o.width, .height = o.height, .frames = 2});
        flushNeuralTextures(*renderer);
        // Textures registered by the flush (decoded / fallback) start with their mip tail resident: give the
        // texture streamer frames to bring the finest mips in.
        render(cam, {.width = o.width, .height = o.height, .frames = 6});
        return render(cam, o);
    }
};

} // namespace

TEST_F(NeuralTexturesTest, InferenceOnSampleMatchesRegularTextures) {
    const assets::NtcSourceImages src = proceduralMaterial(256);
    const auto set = encode(src);
    const Uuid regular = regularMaterial(src);
    const Uuid neural = neuralMaterial(set);
    sun(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 20000.0f, glm::vec3(1.0f), false);
    environment(0.5f, 0.5f);
    // Plane facing the camera (Plane primitive is XZ, rotate it up).
    const glm::quat up = glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));
    Entity plane = mesh(Primitive::Plane, regular, {0, 0, 0}, glm::vec3(2.0f), up);
    world->updateTransforms();
    const CameraParams cam = camera({0, 0, 2.2f}, {0, 0, 0}, 12.0f, 50.0f);
    const Options opt{.width = 256, .height = 256, .frames = 3};

    // Content-adaptive VRS coarsens the shading of this flat, mid-contrast plane in 2x2 / 4x4 blocks differently per
    // mode (with r.VRS on, decode-on-load fell to 25.7 dB and inference to 30.3 dB): compare texture decoding only.
    CVarScope noVrs("r.VRS", "false");
    CVarScope off("r.NeuralTextures", "Off");
    const Image ref = renderFlushed(cam, opt);
    writePng(outDir() / "ntc_regular.png", ref);

    plane.get<MeshRendererComponent>().materials[0] = neural;
    CVarScope inference("r.NeuralTextures", "InferenceOnSample");
    CVarScope bilinear("r.NeuralTextures.Filter", "Bilinear");
    const Image inf = renderFlushed(cam, opt);
    writePng(outDir() / "ntc_inference_bilinear.png", inf);
    const NeuralTextureStats st = neuralTextureStats(*renderer);
    EXPECT_EQ(st.inferenceMaterials, 1u);
    EXPECT_GT(st.latentBytes, 0u);

    // Only the BC fallback tail (the network is not evaluated): clearly worse than inference.
    Image fallbackOnly;
    {
        CVarScope minMip("r.NeuralTextures.MinMip", "8");
        fallbackOnly = renderFlushed(cam, opt);
        writePng(outDir() / "ntc_fallback_only.png", fallbackOnly);
    }
    // Stochastic filtering, single frame (noisy before TAA).
    Image stochastic;
    {
        CVarScope stf("r.NeuralTextures.Filter", "Stochastic");
        stochastic = renderFlushed(cam, opt);
        writePng(outDir() / "ntc_inference_stochastic.png", stochastic);
    }
    CVarScope decode("r.NeuralTextures", "DecodeOnLoad");
    const Image dec = renderFlushed(cam, opt);
    writePng(outDir() / "ntc_decode_on_load.png", dec);
    {
        const TextureStreamingStats& ss = renderer->stats().streaming;
        std::printf("  streaming: %u textures, %u pending, resident %llu B, wanted %llu B\n", ss.streamedTextures,
                    ss.pendingRequests, (unsigned long long)ss.residentBytes, (unsigned long long)ss.wantedBytes);
    }
    EXPECT_GE(neuralTextureStats(*renderer).decodedMaterials, 1u);

    const f64 pInf = imagePsnr(ref, inf), pFallback = imagePsnr(ref, fallbackOnly), pStochastic = imagePsnr(ref, stochastic),
              pDecode = imagePsnr(ref, dec);
    std::printf("  PSNR vs regular BC textures: inference (bilinear) %.2f dB, stochastic 1 frame %.2f dB, decode-on-load "
                "%.2f dB, fallback tail only %.2f dB\n",
                pInf, pStochastic, pDecode, pFallback);
    EXPECT_GT(pInf, 28.0);
    EXPECT_GT(pDecode, 28.0);
    EXPECT_GT(pStochastic, 22.0);
    EXPECT_GT(pInf, pFallback + 1.0) << "the network must add detail over the fallback tail";
}

// The GPU-driven meshlet path (r.GpuDriven.Meshlets, compute expansion or mesh shaders) keeps two material pipeline
// sets like the renderer: without the decoder (OX_NTC_DISABLED) normally, with it under InferenceOnSample. A neural
// material drawn through meshlets must decode like the instanced path.
TEST_F(NeuralTexturesTest, MeshletPathDecodesNeuralMaterials) {
    const assets::NtcSourceImages src = proceduralMaterial(256);
    const auto set = encode(src);
    const Uuid neural = neuralMaterial(set);
    sun(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 20000.0f, glm::vec3(1.0f), false);
    environment(0.5f, 0.5f);
    PrimitiveParams pp;
    pp.segments = 96;
    pp.rings = 48;
    assets::MeshData sphere = makePrimitive(Primitive::Sphere, pp);
    assets::processMesh(sphere, {}); // meshlets
    ASSERT_FALSE(sphere.meshlets.empty());
    const Uuid sphereId = Uuid::fromName("test.ntc.meshletSphere");
    renderer->resources().addMesh(sphereId, sphere);
    Entity e = world->create("NeuralSphere");
    e.setScale(glm::vec3(2.0f));
    auto& mr = e.add<MeshRendererComponent>();
    mr.mesh = sphereId;
    mr.materials = {neural};
    world->updateTransforms();
    const CameraParams cam = camera({0, 0, 2.6f}, {0, 0, 0}, 12.0f, 50.0f);
    const Options opt{.width = 256, .height = 256, .frames = 3};
    CVarScope noVrs("r.VRS", "false");
    CVarScope inference("r.NeuralTextures", "InferenceOnSample");
    CVarScope bilinear("r.NeuralTextures.Filter", "Bilinear");
    CVarScope gpuDriven("r.GpuDriven", "true");
    Image instanced, meshlets;
    {
        CVarScope m("r.GpuDriven.Meshlets", "false");
        instanced = renderFlushed(cam, opt);
    }
    {
        CVarScope m("r.GpuDriven.Meshlets", "true");
        meshlets = renderFlushed(cam, opt);
        const GpuCullingStats& g = renderer->stats().gpuCulling;
        EXPECT_GT(g.meshletsVisible, 0u) << "the sphere must go through the meshlet path";
    }
    writePng(outDir() / "ntc_meshlets_instanced.png", instanced);
    writePng(outDir() / "ntc_meshlets.png", meshlets);
    const f64 p = imagePsnr(instanced, meshlets);
    std::printf("  neural material, meshlets vs instanced: %.2f dB\n", p);
    EXPECT_GT(p, 35.0) << "the meshlet pipelines of InferenceOnSample carry the neural texture decoder";
    EXPECT_GT(meshlets.luminance(128, 128), 0.02f);
}

TEST_F(NeuralTexturesTest, ModeSwitchingAtRuntimeWithoutLeaks) {
    const assets::NtcSourceImages src = proceduralMaterial(128);
    const auto set = encode(src, 200);
    const Uuid a = neuralMaterial(set);
    const Uuid b = neuralMaterial(set, glm::vec2(2.0f));
    sun(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 20000.0f, glm::vec3(1.0f), false);
    environment();
    mesh(Primitive::Cube, a, {-0.7f, 0, 0});
    mesh(Primitive::Sphere, b, {0.7f, 0, 0});
    world->updateTransforms();
    const CameraParams cam = camera({0, 0.5f, 3.5f}, {0, 0, 0}, 12.0f);
    rhi::GpuMemoryStats first{};
    const char* modes[] = {"InferenceOnSample", "DecodeOnLoad", "Off"};
    for (int round = 0; round < 3; ++round) {
        for (const char* mode : modes) {
            CVarScope m("r.NeuralTextures", mode);
            const Image img = renderFlushed(cam, {.width = 96, .height = 64, .frames = 2});
            EXPECT_GT(img.luminance(30, 32) + img.luminance(66, 32), 0.05f) << mode;
            const NeuralTextureStats st = neuralTextureStats(*renderer);
            if (std::string(mode) == "InferenceOnSample") {
                EXPECT_EQ(st.inferenceMaterials, 2u);
                EXPECT_EQ(st.decodedMaterials, 0u);
            } else {
                EXPECT_EQ(st.inferenceMaterials, 0u) << mode;
                EXPECT_EQ(st.latentBytes, 0u) << mode;
                EXPECT_EQ(st.decodedMaterials, 2u) << mode; // neuralTexturesOnly: Off decodes too
            }
        }
        device->waitIdle();
        render(cam, {.width = 96, .height = 64, .frames = 3}); // retire deferred destructions
        if (round == 0) first = device->memoryStats();
    }
    const rhi::GpuMemoryStats last = device->memoryStats();
    EXPECT_LE(last.textureCount, first.textureCount + 2);
    EXPECT_LE(last.bufferCount, first.bufferCount + 2);
}

TEST_F(NeuralTexturesTest, LatentResidencyFollowsDistance) {
    const assets::NtcSourceImages src = proceduralMaterial(256);
    const auto set = encode(src, 200);
    ASSERT_EQ(set->pairs.size(), 2u);
    const Uuid m = neuralMaterial(set);
    sun(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 20000.0f, glm::vec3(1.0f), false);
    environment();
    Entity e = mesh(Primitive::Sphere, m, {0, 0, 0});
    world->updateTransforms();
    CVarScope inference("r.NeuralTextures", "InferenceOnSample");
    renderFlushed(camera({0, 0, 2.0f}, {0, 0, 0}, 12.0f), {.width = 256, .height = 256, .frames = 2});
    EXPECT_EQ(neuralTextureStats(*renderer).latentPairsResident, 2u);
    e.setPosition({0, 0, -150.0f}); // a few pixels: the finest pair is dropped after the drop delay
    world->updateTransforms();
    render(camera({0, 0, 2.0f}, {0, 0, -150}, 12.0f, 50.0f, 400.0f), {.width = 256, .height = 256, .frames = 40});
    EXPECT_EQ(neuralTextureStats(*renderer).latentPairsResident, 1u);
    e.setPosition({0, 0, 0});
    world->updateTransforms();
    render(camera({0, 0, 2.0f}, {0, 0, 0}, 12.0f), {.width = 256, .height = 256, .frames = 2});
    EXPECT_EQ(neuralTextureStats(*renderer).latentPairsResident, 2u);
}

TEST_F(NeuralTexturesTest, GpuDecodeMatchesCpuOnEveryPath) {
    const auto set = encode(proceduralMaterial(128), 200);
    const std::array<assets::Image, 4> cpu = assets::decodeNeuralMip(*set, 0);
    NtcGpuDecoder decoder(*device);
    for (u32 p = 0; p < kNeuralPathCount; ++p) {
        const NeuralPath path = NeuralPath(p);
        if (!neuralPathSupported(device->caps(), path)) continue;
        const NtcGpuDecoder::Result r = decoder.decode(*set, path);
        ASSERT_TRUE(r.ok) << neuralPathName(path);
        ASSERT_EQ(r.mips.size(), set->neuralMips);
        const f64 albedo = assets::computePsnr(r.mips[0][0], cpu[0], 7);
        const f64 normal = assets::computePsnr(r.mips[0][1], cpu[1], 3);
        const f64 orm = assets::computePsnr(r.mips[0][2], cpu[2], 7);
        std::printf("  %-10s GPU vs CPU decode: albedo %.1f dB, normal %.1f dB, orm %.1f dB\n", neuralPathName(path), albedo,
                    normal, orm);
        // 8-bit output quantisation (+ FP16 activations on the FP16 / tensor paths).
        EXPECT_GT(albedo, 38.0) << neuralPathName(path);
        EXPECT_GT(normal, 38.0) << neuralPathName(path);
        EXPECT_GT(orm, 38.0) << neuralPathName(path);
    }
}

// ---- benchmark: a material station like showcase station 02 ---------------------------------------------------
// OX_NTC_PERF=1 (or =Low/Medium/High): trains the showcase materials once (cached in <temp>/oxwald_ntc_cache), then
// renders the station at 1080p and 4K with r.NeuralTextures Off / DecodeOnLoad / InferenceOnSample (TAA on) and
// prints GPU time of the depth prepass + forward pass, texture memory and load-time costs (docs/dev/perf.md).
namespace {

struct StationMaterial {
    std::string name;
    assets::NtcSourceImages src;
    glm::vec2 tiling{1.0f};
};

assets::Image loadOr(const fs::path& p, u32 maxSize) {
    auto img = assets::loadImage(p);
    if (!img) return {};
    assets::Image i = std::move(*img);
    while (maxSize && std::max(i.width, i.height) > maxSize) i = assets::resizeImage(i, i.width / 2, i.height / 2);
    return i;
}

std::vector<StationMaterial> stationMaterials() {
    const fs::path root = fs::path(OX_RENDER_SHADER_DIR).parent_path().parent_path() / "samples/OxwaldShowcase/Assets";
    const fs::path gen = root / "Textures/Generated", legacy = root / "Legacy";
    std::vector<StationMaterial> m;
    m.push_back({"tiles", {loadOr(gen / "tiles_albedo.png", 0), loadOr(gen / "tiles_normal.png", 0), loadOr(gen / "tiles_orm.png", 0), {}}, {4, 4}});
    m.push_back({"concrete", {loadOr(gen / "concrete_albedo.png", 0), loadOr(gen / "concrete_normal.png", 0), {}, {}}, {2, 1}});
    m.push_back({"wood", {loadOr(gen / "wood_albedo.png", 0), {}, loadOr(gen / "leather_orm.png", 512), {}}, {1, 1}});
    // 2048^2 leather from the legacy 4K set: glossiness -> ORM (occlusion 1, roughness 1 - gloss, metallic 0).
    assets::Image orm = loadOr(legacy / "beige_leather_5_glossiness.png", 2048);
    for (glm::vec4& p : orm.pixels) p = glm::vec4(1.0f, 1.0f - p.r, 0.0f, 1.0f);
    m.push_back({"leather", {loadOr(legacy / "beige_leather_5_diffuse.png", 2048), loadOr(legacy / "beige_leather_5_normal.png", 2048), orm, {}}, {1, 1}});
    m.push_back({"rock", {loadOr(gen / "terrain_rock.png", 0), loadOr(gen / "concrete_normal.png", 0), {}, {}}, {1, 1}});
    m.push_back({"screen", {loadOr(gen / "grid_albedo.png", 0), {}, {}, loadOr(gen / "screen_emissive.png", 0)}, {1, 1}});
    return m;
}

} // namespace

TEST_F(NeuralTexturesTest, PerfReportMaterialStation) {
    const char* env = std::getenv("OX_NTC_PERF");
    if (!env) GTEST_SKIP() << "benchmark: set OX_NTC_PERF=1 (or Low/Medium/High)";
    const std::string presetName = std::string(env) == "1" ? "Medium" : env;
    const assets::NtcPreset preset = presetName == "Low"    ? assets::NtcPreset::Low
                                     : presetName == "High" ? assets::NtcPreset::High
                                                            : assets::NtcPreset::Medium;
    const fs::path cache = fs::temp_directory_path() / "oxwald_ntc_cache";
    fs::create_directories(cache);
    std::vector<StationMaterial> mats = stationMaterials();
    std::vector<std::shared_ptr<const assets::NeuralTextureSet>> sets;
    std::printf("\n== NTC material station: preset %s ==\n", presetName.c_str());
    std::printf("%-9s %9s | %10s %10s %10s %6s | %8s %8s | %s\n", "material", "size", "BC bytes", "NTC bytes", "NTC VRAM",
                "bpt", "train s", "load ms", "PSNR NTC/BC (dB): albedo normal rough  | SSIM albedo NTC/BC");
    u64 totalBc = 0, totalNtc = 0, totalVram = 0;
    for (StationMaterial& m : mats) {
        const fs::path file = cache / (m.name + "_" + presetName + ".oxntc");
        std::shared_ptr<const assets::NeuralTextureSet> set;
        if (fs::exists(file)) {
            std::ifstream in(file, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto s = assets::deserializeNeuralTextureSet(std::as_bytes(std::span<const char>(bytes)));
            if (s) set = std::make_shared<const assets::NeuralTextureSet>(std::move(*s));
        }
        if (!set) {
            assets::NeuralTextureSettings s;
            s.preset = preset;
            auto enc = assets::encodeNeuralTextures(m.src, s);
            ASSERT_TRUE(enc) << enc.error().message;
            enc->stats = assets::evaluateNeuralTextures(*enc, m.src, true);
            const auto bytes = assets::serializeNeuralTextureSet(*enc);
            std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            set = std::make_shared<const assets::NeuralTextureSet>(std::move(*enc));
        }
        Stopwatch sw(true);
        const assets::NtcDecodedTextures dec = assets::decodeToTextures(*set, 0);
        const f64 loadMs = sw.elapsedMs();
        auto psnr = [&](const char* n) {
            for (const auto& c : set->stats.channels)
                if (c.name == n) return std::format("{:.1f}/{:.1f}", c.psnr, c.bcPsnr);
            return std::string("-");
        };
        auto ssim = [&](const char* n) {
            for (const auto& c : set->stats.channels)
                if (c.name == n) return std::format("{:.3f}/{:.3f}", c.ssim, c.bcSsim);
            return std::string("-");
        };
        std::printf("%-9s %4ux%-4u | %10llu %10llu %10llu %6.2f | %8.1f %8.0f | %s %s %s | %s\n", m.name.c_str(),
                    set->width, set->height, (unsigned long long)set->stats.sourceBcBytes,
                    (unsigned long long)set->payloadBytes(), (unsigned long long)set->inferenceVramBytes(),
                    set->bitsPerTexel(), set->stats.trainMs / 1000.0, loadMs, psnr("albedo").c_str(),
                    psnr("normal").c_str(), psnr("roughness").c_str(), ssim("albedo").c_str());
        std::printf("          decode-on-load (CPU): network %.0f ms + BC encode %.0f ms\n", dec.decodeMs, dec.encodeMs);
        totalBc += set->stats.sourceBcBytes;
        totalNtc += set->payloadBytes();
        totalVram += set->inferenceVramBytes();
        sets.push_back(set);
    }
    std::printf("%-9s %9s | %10llu %10llu %10llu\n", "total", "", (unsigned long long)totalBc, (unsigned long long)totalNtc,
                (unsigned long long)totalVram);

    // GPU decode (DecodeOnLoad on the GPU) per path, largest material.
    {
        NtcGpuDecoder decoder(*device);
        for (u32 p = 0; p < kNeuralPathCount; ++p) {
            if (!neuralPathSupported(device->caps(), NeuralPath(p))) continue;
            const auto r = decoder.decode(*sets[3], NeuralPath(p), true, 8, false);
            std::printf("  GPU decode of all neural mips of 'leather' (%ux%u): %-10s %.3f ms\n", sets[3]->width,
                        sets[3]->height, neuralPathName(NeuralPath(p)), r.gpuMs);
        }
    }

    auto run = [&](const char* shaderPath) {
        CVarScope sp("r.NeuralTextures.ShaderPath", shaderPath);
        // Fresh renderer: the material shaders' MLP path is chosen when the pipelines are created.
        device->waitIdle();
        renderer.reset();
        world = std::make_unique<World>();
        renderer = Renderer::create(*device);
        view = 0;
        std::vector<Uuid> regular, neural;
        for (usize i = 0; i < mats.size(); ++i) {
            regular.push_back(regularMaterialFull(mats[i]));
            neural.push_back(neuralMaterial(sets[i], mats[i].tiling, !mats[i].src.emissive.empty()));
        }
        sun(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f)), 30000.0f);
        environment(0.8f, 0.8f);
        std::vector<Entity> entities;
        const glm::quat up = glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));
        entities.push_back(mesh(Primitive::Plane, regular[0], {0, 0, 0}, glm::vec3(12.0f)));             // floor: tiles
        entities.push_back(mesh(Primitive::Plane, regular[1], {0, 3, -4}, glm::vec3(12.0f, 1, 6), up)); // wall: concrete
        const Primitive shapes[3] = {Primitive::Sphere, Primitive::Cube, Primitive::Cylinder};
        for (int z = 0; z < 3; ++z)
            for (int x = 0; x < 6; ++x)
                entities.push_back(mesh(shapes[(x + z) % 3], regular[usize(x + z) % mats.size()],
                                        {-3.75f + f32(x) * 1.5f, 0.6f, -2.0f + f32(z) * 1.5f}, glm::vec3(1.0f)));
        world->updateTransforms();
        auto setMaterials = [&](bool useNeural) {
            for (Entity& e : entities) {
                Uuid& mid = e.get<MeshRendererComponent>().materials[0];
                for (usize i = 0; i < mats.size(); ++i)
                    if (mid == regular[i] || mid == neural[i]) mid = useNeural ? neural[i] : regular[i];
            }
        };
        const CameraParams cam = camera({0, 2.2f, 5.0f}, {0, 0.6f, -1.0f}, 12.5f, 60.0f);
        CVarScope taa("r.AntiAliasing", "2");
        struct Mode {
            const char* label;
            const char* cvar;
            bool neural;
            const char* filter;
        };
        const Mode modes[] = {{"Off (BC7/BC5)", "Off", false, "Stochastic"},
                              {"DecodeOnLoad", "DecodeOnLoad", true, "Stochastic"},
                              {"InferenceOnSample", "InferenceOnSample", true, "Stochastic"},
                              {"Inference+Bilinear", "InferenceOnSample", true, "Bilinear"}};
        for (const Mode& md : modes) {
            CVarScope m("r.NeuralTextures", md.cvar);
            CVarScope f("r.NeuralTextures.Filter", md.filter);
            setMaterials(md.neural);
            for (auto [w, h] : {std::pair{1920u, 1080u}, std::pair{3840u, 2160u}}) {
                const Image img = renderFlushed(cam, {.width = w, .height = h, .frames = 24});
                if (w == 1920) writePng(outDir() / std::format("ntc_station_{}_{}_{}.png", shaderPath, md.cvar, md.filter), img);
                f64 prepass = 0, forward = 0;
                const RenderStats& s = renderer->stats();
                for (const PassTiming& p : s.passes) {
                    if (p.name.find("DepthPrepass") != std::string::npos) prepass += p.gpuMs;
                    if (p.name.find("ForwardOpaque") != std::string::npos) forward += p.gpuMs;
                }
                const NeuralTextureStats ns = neuralTextureStats(*renderer);
                const u64 vram = renderer->resources().textureBytes() + ns.latentBytes + ns.weightBytes;
                std::printf("  [%-10s] %-19s %4ux%-4u frame %6.2f ms  prepass %5.2f  forward %6.2f  textures+latents %7.1f MiB\n",
                            shaderPath, md.label, w, h, s.gpuFrameMs, prepass, forward, f64(vram) / (1024.0 * 1024.0));
            }
        }
    };
    // OX_NTC_PERF_PATHS=FP16,Auto,... measures only these material shader paths, in this order (default Auto, Scalar,
    // FP16; the order matters for the Off rows' warm-up).
    std::vector<std::string> paths = {"Auto", "Scalar", "FP16"};
    if (const char* p = std::getenv("OX_NTC_PERF_PATHS")) {
        paths.clear();
        for (std::string list = p; !list.empty();) {
            const usize comma = list.find(',');
            paths.push_back(list.substr(0, comma));
            list = comma == std::string::npos ? std::string() : list.substr(comma + 1);
        }
    }
    for (const std::string& p : paths) run(p.c_str());
}
