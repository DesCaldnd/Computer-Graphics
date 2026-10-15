// Neural network CPU reference (ox::nn) and neural texture compression (NTC).
#include "test_helpers.hpp"

#include <oxwald/assets/neural/mlp.hpp>
#include <oxwald/assets/neural/ntc.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>

using namespace oxtest;

namespace {

// Procedural "material": tiles with grout, low-frequency colour variation, a height map turned into normals, ORM
// with rougher grout. Deterministic and tileable.
NtcSourceImages proceduralMaterial(u32 size, bool emissive = false, bool alpha = false) {
    NtcSourceImages s;
    s.albedo = Image(size, size);
    s.normal = Image(size, size);
    s.orm = Image(size, size);
    if (emissive) s.emissive = Image(size, size);
    const f32 tau = 2.0f * std::numbers::pi_v<f32>;
    auto height = [&](f32 u, f32 v) {
        const f32 tu = u * 4.0f, tv = v * 4.0f;
        const f32 gu = std::abs(tu - std::round(tu)), gv = std::abs(tv - std::round(tv));
        const f32 grout = std::min(gu, gv) < 0.04f ? 0.0f : 1.0f;
        return grout * (0.8f + 0.1f * std::sin(tau * u * 9.0f) * std::sin(tau * v * 7.0f));
    };
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 u = (f32(x) + 0.5f) / f32(size), v = (f32(y) + 0.5f) / f32(size);
            const f32 h = height(u, v);
            const u32 tile = u32(u * 4.0f) + 4u * u32(v * 4.0f);
            const f32 hue = f32((tile * 37u) % 11u) / 11.0f;
            const f32 noise = 0.05f * std::sin(tau * (u * 23.0f + v * 5.0f)) + 0.04f * std::cos(tau * v * 31.0f);
            glm::vec3 c = h > 0.1f ? glm::vec3(0.55f + 0.3f * hue, 0.35f + 0.2f * (1.0f - hue), 0.25f) + noise
                                   : glm::vec3(0.3f, 0.3f, 0.28f);
            const f32 a = alpha ? (h > 0.1f ? 1.0f : 0.2f) : 1.0f;
            s.albedo.at(x, y) = glm::vec4(glm::clamp(c, 0.0f, 1.0f), a);
            const f32 e = 1.0f / f32(size);
            const f32 dx = (height(u + e, v) - height(u - e, v)) * 4.0f, dy = (height(u, v + e) - height(u, v - e)) * 4.0f;
            const glm::vec3 n = glm::normalize(glm::vec3(-dx, -dy, 1.0f));
            s.normal.at(x, y) = glm::vec4(n * 0.5f + 0.5f, 1.0f);
            s.orm.at(x, y) = glm::vec4(h > 0.1f ? 1.0f : 0.6f, h > 0.1f ? 0.45f + 0.1f * hue : 0.9f, tile % 5 == 0 ? 1.0f : 0.0f, 1.0f);
            if (emissive) s.emissive.at(x, y) = tile == 6 && h > 0.1f ? glm::vec4(1.0f, 0.6f, 0.2f, 1.0f) : glm::vec4(0, 0, 0, 1);
        }
    }
    return s;
}

NeuralTextureSettings fastSettings() {
    NeuralTextureSettings s;
    s.preset = NtcPreset::Custom;
    s.latentStride = 4;
    s.features0 = 8;
    s.features1 = 8;
    s.hidden = 32;
    s.steps = 700;
    s.batch = 2048;
    s.fallbackSize = 32;
    return s;
}

f64 channelPsnr(const NtcStats& st, const std::string& name) {
    for (const auto& c : st.channels)
        if (c.name == name) return c.psnr;
    return -1.0;
}

} // namespace

TEST(NeuralNet, HalfConversionRoundTrips) {
    for (f32 v : {0.0f, 1.0f, -2.5f, 0.333251953125f, 65504.0f, 6.103515625e-05f, 5.9604645e-08f}) {
        EXPECT_EQ(nn::halfToFloat(nn::floatToHalf(v)), v) << v;
    }
    EXPECT_EQ(nn::floatToHalf(1e9f), 0x7BFFu);                      // saturates
    EXPECT_EQ(nn::roundToHalf(1.0f + 1.0f / 4096.0f), 1.0f);        // ties to even
    EXPECT_NEAR(nn::roundToHalf(0.1f), 0.1f, 1e-4f);
}

TEST(NeuralNet, ForwardMatchesManualComputation) {
    nn::Mlp net(std::vector<nn::LayerDesc>{{2, 2, nn::Activation::ReLU}, {2, 1, nn::Activation::None}});
    auto p = net.params();
    // layer 0: W = [[1, -1], [2, 0.5]], b = [0.5, -4]; layer 1: W = [[3, -2]], b = [1]
    const f32 values[] = {1, -1, 2, 0.5f, 0.5f, -4, 3, -2, 1};
    std::copy(std::begin(values), std::end(values), p.begin());
    net.syncTransposed();
    std::vector<f32> scratch(net.scratchSize());
    const f32 in[2] = {2.0f, 1.0f};
    f32 out = 0.0f;
    net.forward(in, &out, scratch.data());
    // h = relu([2 - 1 + 0.5, 4 + 0.5 - 4]) = [1.5, 0.5]; y = 4.5 - 1 + 1 = 4.5
    EXPECT_FLOAT_EQ(out, 4.5f);
}

TEST(NeuralNet, TrainingReducesLossAndIsDeterministic) {
    // f(x, y) = (sin(3x) * cos(2y), x*y) on [-1, 1]^2
    const u32 count = 4096;
    std::vector<f32> x(count * 2), y(count * 2);
    nn::Pcg32 rng(7);
    for (u32 i = 0; i < count; ++i) {
        const f32 a = rng.uniform(-1, 1), b = rng.uniform(-1, 1);
        x[i * 2] = a, x[i * 2 + 1] = b;
        y[i * 2] = std::sin(3 * a) * std::cos(2 * b);
        y[i * 2 + 1] = a * b;
    }
    auto train = [&](u32 threads) {
        nn::Mlp net = nn::Mlp::make(2, {32, 32}, 2);
        net.initialize(42);
        nn::TrainOptions o;
        o.steps = 400;
        o.batch = 256;
        o.learningRate = 0.01f;
        o.seed = 3;
        o.threads = threads;
        const nn::TrainResult r = nn::trainRegression(net, x, y, count, o);
        return std::make_pair(net, r);
    };
    const auto [a, ra] = train(1);
    const auto [b, rb] = train(4);
    EXPECT_LT(ra.finalLoss, ra.initialLoss * 0.1f) << ra.initialLoss << " -> " << ra.finalLoss;
    EXPECT_LT(ra.finalLoss, 0.01f);
    EXPECT_TRUE(a == b) << "parameters differ between 1 and 4 threads";
    EXPECT_EQ(ra.finalLoss, rb.finalLoss);
    // Serialisation round trip.
    nn::Mlp c;
    ASSERT_TRUE(nn::Mlp::deserialize(a.serialize(), c));
    EXPECT_TRUE(a == c);
}

TEST(NeuralNet, PackFp16PadsRowsAndAligns) {
    nn::Mlp net = nn::Mlp::make(5, {20}, 3);
    net.initialize(1);
    net.roundToHalf();
    const nn::PackedNetwork p = nn::packFp16(net);
    ASSERT_EQ(p.layers.size(), 2u);
    EXPECT_EQ(p.layers[0].inputsPadded, 16u);
    EXPECT_EQ(p.layers[0].outputsPadded, 32u);
    EXPECT_EQ(p.layers[1].inputsPadded, 32u);
    EXPECT_EQ(p.layers[1].outputsPadded, 16u);
    for (const auto& l : p.layers) {
        EXPECT_EQ(l.weightOffset % 32u, 0u);
        EXPECT_EQ(l.biasOffset % 32u, 0u);
    }
    // weight (o=2, i=4) of layer 0 and a padding entry
    EXPECT_EQ(nn::halfToFloat(p.halves[p.layers[0].weightOffset + 2 * 16 + 4]), net.params()[net.weightOffset(0) + 2 * 5 + 4]);
    EXPECT_EQ(p.halves[p.layers[0].weightOffset + 2 * 16 + 5], 0u);
}

TEST(NeuralTextures, EncodeDecodeRoundTripQuality) {
    const NtcSourceImages src = proceduralMaterial(256, true);
    auto set = encodeNeuralTextures(src, fastSettings());
    ASSERT_TRUE(set) << set.error().message;
    EXPECT_EQ(set->width, 256u);
    EXPECT_EQ(set->mipCount, 9u);
    EXPECT_EQ(set->neuralMips, 3u); // 256, 128, 64 (32 and below: fallback)
    EXPECT_EQ(set->pairs.size(), 2u);
    EXPECT_EQ(set->channels.count, 11u);
    EXPECT_EQ(set->fallback[0].format, TextureFormat::BC7Srgb);
    EXPECT_EQ(set->fallback[1].format, TextureFormat::BC5Unorm);
    EXPECT_EQ(set->fallback[0].mips.size(), 6u);
    const NtcStats st = evaluateNeuralTextures(*set, src, true);
    for (const auto& c : st.channels)
        std::printf("  %-10s NTC %.2f dB (SSIM %.3f)  BC %.2f dB\n", c.name.c_str(), c.psnr, c.ssim, c.bcPsnr);
    std::printf("  %.2f bits/texel, payload %llu B vs BC %llu B, %u steps, %.0f ms\n", set->bitsPerTexel(),
                (unsigned long long)set->payloadBytes(), (unsigned long long)st.sourceBcBytes, set->stats.steps,
                set->stats.trainMs);
    EXPECT_GT(channelPsnr(st, "albedo"), 26.0);
    EXPECT_GT(channelPsnr(st, "normal"), 26.0);
    EXPECT_GT(channelPsnr(st, "roughness"), 26.0);
    EXPECT_LT(set->payloadBytes(), st.sourceBcBytes);
}

TEST(NeuralTextures, EncodingIsDeterministicAndSerializes) {
    const NtcSourceImages src = proceduralMaterial(128, false, true);
    NeuralTextureSettings s = fastSettings();
    s.steps = 150;
    s.threads = 1;
    auto a = encodeNeuralTextures(src, s);
    s.threads = 6;
    auto b = encodeNeuralTextures(src, s);
    ASSERT_TRUE(a && b);
    EXPECT_GE(a->channels.alpha, 0);
    EXPECT_TRUE(a->network == b->network);
    ASSERT_EQ(a->pairs.size(), b->pairs.size());
    for (usize p = 0; p < a->pairs.size(); ++p)
        for (u32 g = 0; g < 2; ++g) EXPECT_EQ(a->pairs[p].grids[g].values, b->pairs[p].grids[g].values);

    const std::vector<std::byte> bytes = serializeNeuralTextureSet(*a);
    auto c = deserializeNeuralTextureSet(bytes);
    ASSERT_TRUE(c) << c.error().message;
    EXPECT_TRUE(a->network == c->network);
    EXPECT_EQ(c->width, a->width);
    EXPECT_EQ(c->neuralMips, a->neuralMips);
    for (usize p = 0; p < a->pairs.size(); ++p)
        for (u32 g = 0; g < 2; ++g) EXPECT_EQ(a->pairs[p].grids[g].values, c->pairs[p].grids[g].values);
    for (u32 i = 0; i < 4; ++i) EXPECT_EQ(c->fallback[i].mips.size(), a->fallback[i].mips.size());
    EXPECT_EQ(c->payloadBytes(), a->payloadBytes());
    // Decode is identical after the round trip, and Q4 latents take half a byte each on disk.
    const auto da = decodeNeuralMip(*a, 1), dc = decodeNeuralMip(*c, 1);
    EXPECT_EQ(da[0].pixels, dc[0].pixels);
    EXPECT_LT(bytes.size(), a->payloadBytes() + 4096);
    // Corruption is detected.
    std::vector<std::byte> bad = bytes;
    bad[bad.size() / 2] ^= std::byte{0x5A};
    EXPECT_FALSE(deserializeNeuralTextureSet(bad));
}

TEST(NeuralTextures, DecodeOnLoadProducesValidBcTextures) {
    const NtcSourceImages src = proceduralMaterial(128);
    NeuralTextureSettings s = fastSettings();
    s.steps = 300;
    auto set = encodeNeuralTextures(src, s);
    ASSERT_TRUE(set);
    const NtcDecodedTextures dec = decodeToTextures(*set);
    const TextureFormat formats[4] = {TextureFormat::BC7Srgb, TextureFormat::BC5Unorm, TextureFormat::BC7Unorm,
                                      TextureFormat::Unknown};
    for (u32 i = 0; i < 3; ++i) {
        const TextureData& t = dec.textures[i];
        EXPECT_EQ(t.format, formats[i]);
        EXPECT_EQ(t.width, 128u);
        ASSERT_EQ(t.mips.size(), set->mipCount);
        for (u32 m = 0; m < t.mips.size(); ++m) {
            EXPECT_EQ(t.mips[m].width, set->mipWidth(m));
            EXPECT_EQ(t.mips[m].data.size(), textureLevelSize(t.format, t.mips[m].width, t.mips[m].height));
        }
        // The cooked file loads back.
        auto rt = deserializeTexture(serializeTexture(t));
        ASSERT_TRUE(rt) << rt.error().message;
    }
    EXPECT_TRUE(dec.textures[3].mips.empty());
    // BC mip 0 agrees with the network output.
    const auto neural = decodeNeuralMip(*set, 0);
    const Image bc = decodeBC7(dec.textures[0].mips[0].data, 128, 128);
    EXPECT_GT(computePsnr(bc, neural[0], 7), 35.0);
    const Image bc5 = decodeBC5(dec.textures[1].mips[0].data, 128, 128);
    EXPECT_GT(computePsnr(bc5, neural[1], 3), 35.0);
}

TEST(NeuralTextures, PresetsAndBitrateTargets) {
    NeuralTextureSettings s;
    s.preset = NtcPreset::Low;
    const f64 low = estimateNtcBitsPerTexel(resolveNtcSettings(s));
    s.preset = NtcPreset::High;
    const f64 high = estimateNtcBitsPerTexel(resolveNtcSettings(s));
    EXPECT_LT(low, high);
    s.targetBitsPerTexel = 1.0f;
    EXPECT_EQ(resolveNtcSettings(s).preset, NtcPreset::Low);
    s.targetBitsPerTexel = 12.0f;
    EXPECT_EQ(resolveNtcSettings(s).preset, NtcPreset::High);
    s.preset = NtcPreset::Custom;
    s.targetBitsPerTexel = 0.0f;
    s.features0 = 16;
    s.features1 = 16;
    const NeuralTextureSettings r = resolveNtcSettings(s);
    EXPECT_EQ(r.features0, kNtcMaxFeatures0);
    EXPECT_EQ(r.features1, kNtcMaxFeatures1);
}

TEST(NeuralTextures, MaterialImporterTrainsSubAsset) {
    TempDir dir;
    const NtcSourceImages src = proceduralMaterial(64);
    writePng(dir / "Assets/T/a.png", src.albedo);
    writePng(dir / "Assets/T/a_normal.png", src.normal);
    writePng(dir / "Assets/T/a_orm.png", src.orm);
    Uuid albedo, normal, orm;
    {
        AssetRegistry reg(dir.path());
        reg.scan();
        albedo = *reg.uuidForPath("T/a.png");
        normal = *reg.uuidForPath("T/a_normal.png");
        orm = *reg.uuidForPath("T/a_orm.png");
    }
    MaterialAsset m;
    m.albedoTexture = albedo;
    m.normalTexture = normal;
    m.ormTexture = orm;
    ASSERT_TRUE(saveMaterial(m, dir / "Assets/M/a.oxmat"));
    AssetRegistry reg(dir.path());
    reg.scan();
    const Uuid mat = *reg.uuidForPath("M/a.oxmat");
    MaterialImportSettings ms;
    ms.neuralCompression = NeuralCompressionMode::Replace;
    ms.neuralPreset = NtcPreset::Custom;
    ms.neuralHidden = 32;
    ms.neuralSteps = 100;
    ms.neuralFallbackSize = 16;
    ASSERT_TRUE(reg.setSettings(mat, toSettingsJson(ms)));
    auto bytes = reg.readArtifact(mat);
    ASSERT_TRUE(bytes);
    auto loaded = loadMaterial(*bytes);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->neuralTextures.isValid());
    EXPECT_TRUE(loaded->neuralTexturesOnly);
    EXPECT_EQ(loaded->textureDependencies(), std::vector<Uuid>{loaded->neuralTextures});
    auto ntc = reg.readArtifact(loaded->neuralTextures);
    ASSERT_TRUE(ntc) << ntc.error().message;
    auto set = deserializeNeuralTextureSet(*ntc);
    ASSERT_TRUE(set) << set.error().message;
    EXPECT_EQ(set->width, 64u);
    EXPECT_EQ(set->channels.count, 8u);
    EXPECT_FALSE(set->stats.channels.empty());
    const auto deps = reg.dependencies(mat);
    EXPECT_EQ(deps.size(), 1u);
    // The AssetManager loads the set as a dependency of the material.
    AssetManager manager(reg);
    auto h = manager.loadSync<MaterialAsset>(mat);
    ASSERT_TRUE(h.isLoaded());
    auto hs = manager.loadSync<NeuralTextureSet>(loaded->neuralTextures);
    ASSERT_TRUE(hs.isLoaded());
    EXPECT_EQ(hs.get()->width, 64u);
}

// Report over real textures (not a regular test): OX_NTC_REPORT=<dir with albedo/normal/orm pngs>[;preset]
TEST(NeuralTextures, Report) {
    const char* env = std::getenv("OX_NTC_REPORT");
    if (!env) GTEST_SKIP() << "set OX_NTC_REPORT=<albedo.png>;<normal.png>;<orm.png>[;preset[;steps]]";
    std::vector<std::string> parts;
    std::string cur;
    for (const char* p = env; *p; ++p) {
        if (*p == ';') parts.push_back(cur), cur.clear();
        else cur += *p;
    }
    parts.push_back(cur);
    NtcSourceImages src;
    if (parts.size() > 0 && !parts[0].empty()) src.albedo = *loadImage(parts[0]);
    if (parts.size() > 1 && !parts[1].empty()) src.normal = *loadImage(parts[1]);
    if (parts.size() > 2 && !parts[2].empty()) src.orm = *loadImage(parts[2]);
    NeuralTextureSettings s;
    if (parts.size() > 3) s.preset = parts[3] == "Low" ? NtcPreset::Low : parts[3] == "High" ? NtcPreset::High : NtcPreset::Medium;
    if (parts.size() > 4) s.steps = u32(std::stoul(parts[4]));
    auto set = encodeNeuralTextures(src, s, [](f32 f, f32 loss) {
        static int last = -1;
        if (int(f * 10) != last) last = int(f * 10), std::printf("  %3.0f%% loss %.5f\n", f * 100, loss);
    });
    ASSERT_TRUE(set);
    const NtcStats st = evaluateNeuralTextures(*set, src, true);
    for (const auto& c : st.channels)
        std::printf("  %-10s NTC %.2f dB (SSIM %.4f)  BC %.2f dB (SSIM %.4f)\n", c.name.c_str(), c.psnr, c.ssim, c.bcPsnr, c.bcSsim);
    std::printf("  %ux%u preset %s: %.2f bits/texel, payload %llu B (latents %llu, net %llu, fallback %llu), BC %llu B, "
                "VRAM(inference) %llu B, %u steps, %.0f ms\n",
                set->width, set->height, ntcPresetName(set->preset).data(), set->bitsPerTexel(),
                (unsigned long long)set->payloadBytes(), (unsigned long long)set->latentBytes(),
                (unsigned long long)set->networkBytes(), (unsigned long long)set->fallbackBytes(),
                (unsigned long long)st.sourceBcBytes, (unsigned long long)set->inferenceVramBytes(), set->stats.steps,
                set->stats.trainMs);
    const NtcDecodedTextures dec = decodeToTextures(*set);
    std::printf("  DecodeOnLoad (CPU): network %.1f ms, BC encode %.1f ms\n", dec.decodeMs, dec.encodeMs);
    const auto img = decodeNeuralMip(*set, 0);
    if (!img[0].empty()) (void)saveImagePng(fs::temp_directory_path() / "ntc_report_albedo.png", img[0]);
    if (!img[1].empty()) (void)saveImagePng(fs::temp_directory_path() / "ntc_report_normal.png", img[1]);
    if (!img[2].empty()) (void)saveImagePng(fs::temp_directory_path() / "ntc_report_orm.png", img[2]);
}
