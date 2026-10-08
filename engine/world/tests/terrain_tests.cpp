#include <oxwald/world/splat_map.hpp>
#include <oxwald/world/terrain_brush.hpp>
#include <oxwald/world/terrain_gen.hpp>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>
#include <filesystem>

#include <tinyexr.h>

using namespace ox;
using namespace ox::world;

namespace {

f32 analyticH(f32 x, f32 z) { return 20.f + 10.f * std::sin(0.05f * x) * std::cos(0.03f * z); }
glm::vec3 analyticN(f32 x, f32 z) {
    const f32 dx = 10.f * 0.05f * std::cos(0.05f * x) * std::cos(0.03f * z);
    const f32 dz = -10.f * 0.03f * std::sin(0.05f * x) * std::sin(0.03f * z);
    return glm::normalize(glm::vec3(-dx, 1.f, -dz));
}

Heightfield makeAnalytic(u32 res = 257, f32 size = 256.f, HeightFormat fmt = HeightFormat::Float32) {
    HeightfieldDesc d;
    d.resolution = res;
    d.worldSize = size;
    d.heightScale = 64.f;
    d.heightOffset = -2.f;
    d.origin = {-100.f, 50.f};
    d.format = fmt;
    Heightfield hf(d);
    for (u32 z = 0; z < res; ++z) {
        for (u32 x = 0; x < res; ++x) {
            const glm::vec3 p = hf.samplePosition(x, z);
            hf.setHeightAtSample(x, z, analyticH(p.x, p.z));
        }
    }
    return hf;
}

std::filesystem::path tempPath(const char* name) {
    return std::filesystem::temp_directory_path() / (std::string("ox_world_") + name);
}

Heightfield makeRough(u32 res = 129, f32 heightScale = 60.f) {
    HeightfieldDesc d;
    d.resolution = res;
    d.worldSize = f32(res - 1);
    d.heightScale = heightScale;
    Heightfield hf(d);
    TerrainNoiseSettings ns;
    ns.fractal.type = FractalType::Ridged;
    ns.fractal.frequency = 1.f / 40.f;
    ns.fractal.octaves = 5;
    ns.fractal.seed = 99;
    ns.normalizeRange = true;
    generateNoise(hf, ns);
    return hf;
}

} // namespace

TEST(Heightfield, BilinearMatchesAnalytic) {
    const Heightfield hf = makeAnalytic();
    f32 maxErr = 0.f;
    for (int i = 0; i < 500; ++i) {
        const f32 x = -100.f + 3.f + std::fmod(f32(i) * 13.37f, 250.f);
        const f32 z = 50.f + 3.f + std::fmod(f32(i) * 7.91f, 250.f);
        maxErr = std::max(maxErr, std::abs(hf.sampleHeight({x, z}) - analyticH(x, z)));
    }
    EXPECT_LT(maxErr, 0.01f);
    // Exact at sample positions.
    const glm::vec3 p = hf.samplePosition(17, 33);
    EXPECT_NEAR(hf.sampleHeight({p.x, p.z}), analyticH(p.x, p.z), 1e-4f);
}

TEST(Heightfield, NormalsAndSlopeMatchAnalytic) {
    const Heightfield hf = makeAnalytic();
    f32 maxAngle = 0.f;
    for (int i = 0; i < 300; ++i) {
        const f32 x = -100.f + 5.f + std::fmod(f32(i) * 11.13f, 240.f);
        const f32 z = 50.f + 5.f + std::fmod(f32(i) * 5.77f, 240.f);
        const f32 c = glm::clamp(glm::dot(hf.sampleNormal({x, z}), analyticN(x, z)), -1.f, 1.f);
        maxAngle = std::max(maxAngle, glm::degrees(std::acos(c)));
        EXPECT_NEAR(hf.sampleSlope({x, z}), std::acos(analyticN(x, z).y), glm::radians(1.f));
    }
    EXPECT_LT(maxAngle, 1.f);
}

TEST(Heightfield, U16StorageQuantisesAndConverts) {
    Heightfield hf = makeAnalytic(65, 64.f, HeightFormat::UNorm16);
    EXPECT_EQ(hf.bytesPerSample(), 2u);
    EXPECT_EQ(hf.rawBytes().size(), 65u * 65u * 2u);
    const glm::vec3 p = hf.samplePosition(10, 20);
    EXPECT_NEAR(p.y, analyticH(p.x, p.z), 64.f / 65535.f * 1.01f);
    hf.convertFormat(HeightFormat::Float32);
    EXPECT_EQ(hf.bytesPerSample(), 4u);
    EXPECT_NEAR(hf.samplePosition(10, 20).y, p.y, 1e-4f);
}

TEST(Heightfield, Raw16AndPng16RoundTrip) {
    const Heightfield hf = makeAnalytic(65, 64.f);
    Heightfield::ImportOptions o;
    o.worldSize = 64.f;
    o.heightScale = 64.f;
    o.heightOffset = -2.f;
    o.origin = {-100.f, 50.f};
    const auto raw = tempPath("rt.r16"), png = tempPath("rt.png");
    ASSERT_TRUE(hf.saveRaw16(raw));
    ASSERT_TRUE(hf.savePng16(png));
    for (const auto& path : {raw, png}) {
        std::string err;
        const auto loaded = Heightfield::load(path, o, &err);
        ASSERT_TRUE(loaded) << err;
        ASSERT_EQ(loaded->resolution(), 65u);
        for (u32 z = 0; z < 65; z += 7) {
            for (u32 x = 0; x < 65; x += 5) {
                EXPECT_NEAR(loaded->heightAtSample(i32(x), i32(z)), hf.heightAtSample(i32(x), i32(z)), 64.f / 65535.f * 1.01f);
            }
        }
    }
    std::filesystem::remove(raw);
    std::filesystem::remove(png);
}

TEST(Heightfield, ExrImportInMetres) {
    const int n = 33;
    std::vector<float> data(usize(n) * n);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            data[usize(z) * n + x] = 5.f + f32(x) * 0.5f + f32(z) * 0.25f;
        }
    }
    const auto path = tempPath("h.exr");
    const char* err = nullptr;
    ASSERT_EQ(SaveEXR(data.data(), n, n, 1, 0, path.string().c_str(), &err), TINYEXR_SUCCESS);
    Heightfield::ImportOptions o;
    o.worldSize = 32.f;
    o.heightScale = 100.f;
    o.heightOffset = 0.f;
    std::string e;
    const auto hf = Heightfield::load(path, o, &e);
    ASSERT_TRUE(hf) << e;
    EXPECT_NEAR(hf->heightAtSample(4, 6), 5.f + 2.f + 1.5f, 1e-3f);
    std::filesystem::remove(path);
}

TEST(Heightfield, LoadErrorsAreReported) {
    std::string err;
    EXPECT_FALSE(Heightfield::load("/nonexistent/terrain.png", {}, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(Heightfield::load("terrain.xyz", {}, &err));
}

TEST(Heightfield, HolesAndTiles) {
    Heightfield hf = makeAnalytic(65, 64.f);
    EXPECT_FALSE(hf.hasHoles());
    hf.setHole(10, 10, true);
    EXPECT_TRUE(hf.isHole(10, 10));
    EXPECT_FALSE(hf.isHole(11, 10));
    const Heightfield tile = hf.extractTile({8, 8, 25, 25});
    EXPECT_EQ(tile.resolution(), 17u);
    EXPECT_TRUE(tile.isHole(2, 2));
    EXPECT_NEAR(tile.desc().worldSize, 16.f, 1e-5f);
    EXPECT_NEAR(tile.samplePosition(3, 4).y, hf.samplePosition(11, 12).y, 1e-5f);
    EXPECT_EQ(tile.samplePosition(3, 4).x, hf.samplePosition(11, 12).x);
}

TEST(Noise, DeterministicAndInRange) {
    FractalSettings s;
    s.seed = 42;
    s.warpStrength = 30.f;
    const FractalNoise a(s), b(s);
    s.seed = 43;
    const FractalNoise c(s);
    int differs = 0;
    for (int i = 0; i < 200; ++i) {
        const glm::vec2 p(f32(i) * 17.3f, f32(i) * -4.1f);
        EXPECT_EQ(a.sample(p), b.sample(p));
        differs += a.sample(p) != c.sample(p);
        EXPECT_GE(a.sample(p), -0.1f);
        EXPECT_LE(a.sample(p), 1.1f);
    }
    EXPECT_GT(differs, 150);
    const Noise2D n(7);
    for (int i = 0; i < 1000; ++i) {
        const glm::vec2 p(f32(i) * 0.37f, f32(i) * 0.11f);
        EXPECT_LE(std::abs(n.perlin(p)), 1.05f);
        EXPECT_LE(std::abs(n.simplex(p)), 1.05f);
    }
}

TEST(TerrainGen, NoiseIsSeamlessAcrossTiles) {
    TerrainNoiseSettings ns;
    ns.fractal.seed = 5;
    ns.fractal.frequency = 1.f / 64.f;
    HeightfieldDesc d;
    d.resolution = 33;
    d.worldSize = 32.f;
    Heightfield a(d);
    d.origin = {32.f, 0.f};
    Heightfield b(d);
    generateNoise(a, ns);
    generateNoise(b, ns);
    for (u32 z = 0; z < 33; ++z) {
        EXPECT_EQ(a.normalized(32, z), b.normalized(0, z));
    }
}

TEST(TerrainBrush, RaiseReturnsTightDirtyRect) {
    Heightfield hf = makeAnalytic(129, 128.f);
    const Heightfield before = hf;
    BrushSettings b;
    b.op = BrushOp::Raise;
    b.radius = 10.f;
    b.strength = 2.f;
    const glm::vec2 c = hf.sampleToWorld({64.f, 40.f});
    const IRect r = applyBrush(hf, c, b);
    ASSERT_FALSE(r.empty());
    IRect changed{1 << 30, 1 << 30, -1, -1};
    for (u32 z = 0; z < 129; ++z) {
        for (u32 x = 0; x < 129; ++x) {
            const f32 d = hf.heightAtSample(i32(x), i32(z)) - before.heightAtSample(i32(x), i32(z));
            if (d != 0.f) {
                EXPECT_TRUE(r.contains(i32(x), i32(z))) << x << "," << z;
                EXPECT_GT(d, 0.f);
                changed = changed.merged({i32(x), i32(z), i32(x) + 1, i32(z) + 1});
            }
        }
    }
    EXPECT_EQ(r, changed);
    EXPECT_LE(r.width(), 21);
    EXPECT_NEAR(hf.heightAtSample(64, 40) - before.heightAtSample(64, 40), 2.f, 1e-3f);
    // dt scales the effect, falloff reaches zero at the radius.
    BrushSettings lower = b;
    lower.op = BrushOp::Lower;
    applyBrush(hf, c, lower, 0.5f);
    EXPECT_NEAR(hf.heightAtSample(64, 40) - before.heightAtSample(64, 40), 1.f, 1e-3f);
    EXPECT_EQ(brushWeight(b, 1.f), 0.f);
    EXPECT_EQ(brushWeight(b, 0.f), 1.f);
}

TEST(TerrainBrush, SmoothFlattenNoiseAndHoles) {
    Heightfield hf = makeRough(65);
    const glm::vec2 c = hf.sampleToWorld({32.f, 32.f});
    auto localLaplacian = [&] {
        f32 s = 0.f;
        for (i32 z = 28; z <= 36; ++z) {
            for (i32 x = 28; x <= 36; ++x) {
                s += std::abs(hf.heightAtSample(x + 1, z) + hf.heightAtSample(x - 1, z) + hf.heightAtSample(x, z + 1) +
                              hf.heightAtSample(x, z - 1) - 4.f * hf.heightAtSample(x, z));
            }
        }
        return s;
    };
    const f32 rough = localLaplacian();
    BrushSettings b;
    b.op = BrushOp::Smooth;
    b.radius = 8.f;
    b.strength = 1.f;
    for (int i = 0; i < 5; ++i) {
        applyBrush(hf, c, b);
    }
    EXPECT_LT(localLaplacian(), rough * 0.5f);

    b.op = BrushOp::Flatten;
    b.targetHeight = 12.f;
    b.hardness = 0.5f;
    for (int i = 0; i < 3; ++i) {
        applyBrush(hf, c, b);
    }
    EXPECT_NEAR(hf.heightAtSample(32, 32), 12.f, 1e-3f);

    b.op = BrushOp::Noise;
    b.strength = 1.f;
    EXPECT_FALSE(applyBrush(hf, c, b).empty());

    b.op = BrushOp::SetHole;
    b.radius = 2.f;
    const IRect hr = applyBrush(hf, c, b);
    EXPECT_TRUE(hf.isHole(32, 32));
    EXPECT_TRUE(hr.contains(32, 32));
    b.op = BrushOp::ClearHole;
    applyBrush(hf, c, b);
    EXPECT_FALSE(hf.isHole(32, 32));
    // Brush entirely outside the terrain changes nothing.
    EXPECT_TRUE(applyBrush(hf, {1e5f, 1e5f}, b).empty());
}

TEST(TerrainErosion, ThermalConservesMassAndReducesSlopes) {
    Heightfield hf = makeRough(97);
    const HeightStats before = computeStats(hf);
    ThermalErosionSettings s;
    s.iterations = 80;
    s.talusAngleDeg = 30.f;
    erodeThermal(hf, s);
    const HeightStats after = computeStats(hf);
    EXPECT_NEAR(after.sum, before.sum, std::abs(before.sum) * 1e-4);
    EXPECT_LT(after.maxSlopeDeg, before.maxSlopeDeg);
    EXPECT_LT(after.meanAbsLaplacian, before.meanAbsLaplacian);
    EXPECT_LE(after.maxHeight, before.maxHeight);
}

TEST(TerrainErosion, HydraulicRoughlyConservesMassAndBluntsPeaks) {
    Heightfield hf = makeRough(129, 20.f);
    const Heightfield original = hf;
    const HeightStats before = computeStats(hf);
    HydraulicErosionSettings s;
    s.droplets = 30000;
    s.seed = 3;
    erodeHydraulic(hf, s);
    const HeightStats after = computeStats(hf);
    const f64 volume = before.sum - f64(before.minHeight) * 129.0 * 129.0; // material above the lowest point
    EXPECT_LT(std::abs(after.sum - before.sum) / volume, 0.05) << "before " << before.sum << " after " << after.sum;
    EXPECT_LT(after.maxHeight, before.maxHeight);
    EXPECT_LT(after.meanAbsLaplacian, before.meanAbsLaplacian);
    // Peaks: the highest 1% of samples get lowered on average.
    std::vector<std::pair<f32, u32>> samples;
    for (u32 i = 0; i < 129u * 129u; ++i) {
        samples.emplace_back(original.heightAtSample(i32(i % 129), i32(i / 129)), i);
    }
    std::sort(samples.begin(), samples.end(), std::greater<>());
    f64 delta = 0.0;
    const usize top = samples.size() / 100;
    for (usize i = 0; i < top; ++i) {
        const u32 idx = samples[i].second;
        delta += hf.heightAtSample(i32(idx % 129), i32(idx / 129)) - samples[i].first;
    }
    EXPECT_LT(delta / f64(top), -0.05);
    // Deterministic.
    Heightfield again = original;
    erodeHydraulic(again, s);
    EXPECT_EQ(again.toNormalizedFloats(), hf.toNormalizedFloats());
}

TEST(TerrainGen, TerraceCreatesPlateaus) {
    HeightfieldDesc d;
    d.resolution = 65;
    d.worldSize = 64.f;
    d.heightScale = 100.f;
    Heightfield hf(d);
    for (u32 z = 0; z < 65; ++z) {
        for (u32 x = 0; x < 65; ++x) {
            hf.setNormalized(x, z, f32(x) / 64.f);
        }
    }
    terrace(hf, 4, 0.8f);
    // Within a step the slope is ~0 away from the risers.
    EXPECT_NEAR(hf.normalized(2, 0), hf.normalized(4, 0), 1e-3f);
    EXPECT_NEAR(hf.normalized(2, 0), 0.f, 1e-3f);
    EXPECT_NEAR(hf.normalized(20, 0), 0.25f, 1e-3f);
    EXPECT_GE(hf.normalized(40, 0), hf.normalized(30, 0));
}

TEST(SplatMap, AutoPaintByHeightAndSlope) {
    HeightfieldDesc d;
    d.resolution = 65;
    d.worldSize = 64.f;
    d.heightScale = 64.f;
    Heightfield hf(d);
    // Left half flat at 2 m, right half: steep ramp (45°+) going up.
    for (u32 z = 0; z < 65; ++z) {
        for (u32 x = 0; x < 65; ++x) {
            hf.setHeightAtSample(x, z, x < 32 ? 2.f : 2.f + 1.5f * f32(x - 32));
        }
    }
    SplatMap splat(65, 3, d.origin, d.worldSize);
    const SplatRule rules[] = {
        {.layer = 1, .minSlopeDeg = 40.f, .maxSlopeDeg = 90.f, .slopeBlendDeg = 2.f},              // rock on steep
        {.layer = 2, .minHeight = 40.f, .maxHeight = 1000.f, .heightBlend = 1.f, .maxSlopeDeg = 20.f}, // snow high & flat
    };
    autoPaint(splat, hf, rules);
    EXPECT_EQ(splat.dominantLayer({10.f, 10.f}), 0u);
    EXPECT_EQ(splat.dominantLayer({50.f, 10.f}), 1u);
    for (u32 z = 0; z < 65; z += 8) {
        for (u32 x = 0; x < 65; x += 8) {
            u32 sum = 0;
            for (u32 l = 0; l < 3; ++l) {
                sum += splat.weight(x, z, l);
            }
            EXPECT_EQ(sum, 255u);
        }
    }
    const auto rgba = splat.packRgba8(0, {0, 0, 2, 1});
    ASSERT_EQ(rgba.size(), 8u);
    EXPECT_EQ(rgba[0], 255);

    BrushSettings b;
    b.radius = 5.f;
    b.strength = 1.f;
    b.falloff = BrushFalloff::Constant;
    const IRect r = paintSplat(splat, {10.f, 10.f}, 2, b);
    EXPECT_FALSE(r.empty());
    EXPECT_EQ(splat.dominantLayer({10.f, 10.f}), 2u);
}
