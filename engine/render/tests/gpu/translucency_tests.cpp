// Translucency / water / particles GPU tests: goldens (glass panes OIT vs sorted, refractive sphere with absorption,
// frosted glass, water with shore foam + caustics, underwater view, smoke + sparks, alpha-tested foliage), behaviour
// checks (OIT ≈ sorted, hashed alpha coverage, low-res ≈ full-res particles, toggles) and the 1080p timing report.
#include "render_fixture.hpp"

#include <oxwald/core/scalability.hpp>
#include <oxwald/render/components/translucency.hpp>
#include <oxwald/render/features/translucency/translucency.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

namespace {

class TranslucencyTest : public RenderTest {
protected:
    void SetUp() override {
        RenderTest::SetUp();
        if (!device) return;
        registerTranslucencyTypes();
        for (Scalability g : {Scalability::Effects, Scalability::Shading, Scalability::Reflections}) {
            scalability::setGroup(g, QualityLevel::High);
        }
    }

    Uuid materialEx(const assets::MaterialAsset& m) {
        const Uuid id = Uuid::fromName(std::format("test.translucency.material.{}", materialCounter++));
        renderer->resources().addMaterial(id, m);
        return id;
    }

    Uuid glass(glm::vec4 color, assets::BlendMode mode, f32 roughness = 0.05f, f32 ior = 1.5f,
               glm::vec3 absorption = glm::vec3(1.0f), f32 absorptionDistance = 0.0f, f32 thickness = 0.0f) {
        assets::MaterialAsset m;
        m.baseColor = color;
        m.roughness = roughness;
        m.blendMode = mode;
        m.ior = ior;
        m.absorptionColor = absorption;
        m.absorptionDistance = absorptionDistance;
        m.thickness = thickness;
        return materialEx(m);
    }

    Uuid texture(u32 w, u32 h, const std::vector<u8>& rgba, assets::TextureFilter filter = assets::TextureFilter::Linear) {
        assets::TextureData t;
        t.format = assets::TextureFormat::RGBA8Srgb;
        t.width = w;
        t.height = h;
        t.mipCount = 1;
        t.filter = filter;
        assets::TextureMip mip;
        mip.width = w;
        mip.height = h;
        mip.data.resize(rgba.size());
        std::memcpy(mip.data.data(), rgba.data(), rgba.size());
        t.mips.push_back(std::move(mip));
        const Uuid id = Uuid::fromName(std::format("test.translucency.texture.{}", materialCounter++));
        renderer->resources().addTexture(id, t);
        return id;
    }

    Uuid checkerMaterial(f32 tiling, glm::vec3 a = glm::vec3(0.9f), glm::vec3 b = glm::vec3(0.08f)) {
        std::vector<u8> px(64 * 64 * 4);
        for (u32 y = 0; y < 64; ++y) {
            for (u32 x = 0; x < 64; ++x) {
                const bool odd = ((x / 8) + (y / 8)) & 1;
                const glm::vec3 c = odd ? a : b;
                const usize i = (usize(y) * 64 + x) * 4;
                px[i] = u8(c.r * 255), px[i + 1] = u8(c.g * 255), px[i + 2] = u8(c.b * 255), px[i + 3] = 255;
            }
        }
        assets::MaterialAsset m;
        m.roughness = 0.7f;
        m.albedoTexture = texture(64, 64, px, assets::TextureFilter::Nearest);
        m.uvTiling = glm::vec2(tiling);
        return materialEx(m);
    }

    // Renders `frames` frames with a fixed time step (particles), reading back the last one.
    Image renderSim(const CameraParams& cam, u32 frames, f32 dt, u32 width = 256, u32 height = 256,
                    const std::function<void(u32)>& afterFrame = {}) {
        ensureTarget(width, height, VK_FORMAT_R8G8B8A8_UNORM);
        ensureView(false);
        extractSnapshot();
        renderer->resources().flush();
        snapshot.deltaTime = dt;
        for (u32 f = 0; f < frames; ++f) {
            snapshot.time = f64(f) * dt;
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
            if (afterFrame) afterFrame(f);
        }
        device->waitIdle();
        Image img;
        img.width = width;
        img.height = height;
        img.rgba = device->readTexture(target);
        for (usize i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
        return img;
    }

    // GPU timings come from the last retired frame: render enough frames that it belongs to this call.
    Image shot(const CameraParams& cam) { return render(cam, {.frames = 4}); }

    bool ranPass(std::string_view part) const {
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.find(part) != std::string::npos) return true;
        }
        return false;
    }
    f64 passMs(std::string_view part) const {
        f64 ms = 0.0;
        for (const PassTiming& p : renderer->stats().passes) {
            if (p.name.find(part) != std::string::npos) ms += p.gpuMs;
        }
        return ms;
    }

    static f64 meanDifference(const Image& a, const Image& b) {
        u64 sum = 0;
        for (usize i = 0; i < a.rgba.size(); ++i) {
            if (i % 4 == 3) continue;
            sum += u64(std::abs(int(a.rgba[i]) - int(b.rgba[i])));
        }
        return f64(sum) / f64(a.width * a.height * 3);
    }
    static Image blocks(const Image& a, u32 n = 4) {
        Image o;
        o.width = a.width / n;
        o.height = a.height / n;
        o.rgba.assign(usize(o.width) * o.height * 4, 255);
        for (u32 y = 0; y < o.height; ++y)
            for (u32 x = 0; x < o.width; ++x)
                for (u32 c = 0; c < 3; ++c) {
                    u32 sum = 0;
                    for (u32 dy = 0; dy < n; ++dy)
                        for (u32 dx = 0; dx < n; ++dx) sum += a.rgba[((usize(y * n + dy) * a.width) + x * n + dx) * 4 + c];
                    o.rgba[(usize(y) * o.width + x) * 4 + c] = u8(sum / (n * n));
                }
        return o;
    }
    static glm::vec3 meanColor(const Image& img, u32 x0, u32 y0, u32 x1, u32 y1) {
        glm::dvec3 s(0.0);
        for (u32 y = y0; y < y1; ++y)
            for (u32 x = x0; x < x1; ++x) {
                const glm::u8vec4 c = img.at(x, y);
                s += glm::dvec3(c.r, c.g, c.b);
            }
        return glm::vec3(s / f64((x1 - x0) * (y1 - y0)) / 255.0);
    }

    void glassPanesScene() {
        mesh(Primitive::Plane, checkerMaterial(6.0f), {0, 0, 0}, glm::vec3(12.0f));
        mesh(Primitive::Cube, material({0.7f, 0.7f, 0.72f, 1.0f}, 0.0f, 0.5f), {0, 0.75f, -2.2f}, {3.0f, 1.5f, 0.3f});
        const glm::quat face(1, 0, 0, 0);
        mesh(Primitive::Cube, glass({1.0f, 0.15f, 0.1f, 0.45f}, assets::BlendMode::Transparent), {-0.35f, 1.0f, 0.6f},
             {1.1f, 1.2f, 0.03f}, face);
        mesh(Primitive::Cube, glass({0.1f, 0.9f, 0.2f, 0.45f}, assets::BlendMode::Transparent), {0.0f, 0.8f, 0.0f},
             {1.1f, 1.2f, 0.03f}, face);
        mesh(Primitive::Cube, glass({0.15f, 0.3f, 1.0f, 0.45f}, assets::BlendMode::Transparent), {0.35f, 0.6f, -0.6f},
             {1.1f, 1.2f, 0.03f}, face);
        sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 30000.0f);
        environment();
    }

    Entity water(glm::vec3 position, glm::vec2 size) {
        Entity e = world->create("Water");
        e.setPosition(position);
        auto& w = e.add<WaterSurfaceComponent>();
        w.size = size;
        w.waves = {{{1.0f, 0.3f}, 6.0f, 0.06f, 0.5f, 0.0f, 1.0f},
                   {{0.6f, -0.8f}, 3.1f, 0.035f, 0.5f, 1.1f, 1.0f},
                   {{-0.2f, 1.0f}, 1.7f, 0.015f, 0.4f, 2.3f, 1.0f}};
        return e;
    }

    void beachScene() {
        // Sloped sand: beach on the left (-X), deeper water towards +X; rocks under water for caustics.
        const Uuid sand = material({0.76f, 0.68f, 0.5f, 1.0f}, 0.0f, 0.9f);
        const Uuid rock = material({0.45f, 0.42f, 0.4f, 1.0f}, 0.0f, 0.8f);
        mesh(Primitive::Cube, sand, {0.0f, -0.5f, 0.0f}, {30.0f, 1.0f, 30.0f},
             glm::angleAxis(glm::radians(-7.0f), glm::vec3(0, 0, 1)));
        mesh(Primitive::Sphere, rock, {-1.5f, -0.4f, -1.0f}, glm::vec3(1.2f));
        mesh(Primitive::Cube, rock, {1.0f, -0.2f, -3.0f}, glm::vec3(0.8f));
        water({0.0f, 0.25f, 0.0f}, {40.0f, 40.0f});
        sun(glm::normalize(glm::vec3(-0.3f, -0.8f, -0.5f)), 60000.0f);
        environment();
    }
};

// --- transparency -----------------------------------------------------------------------------------------------

TEST_F(TranslucencyTest, GlassPanesOitVsSorted) {
    glassPanesScene();
    const CameraParams cam = camera({0.6f, 1.6f, 4.2f}, {0, 0.8f, 0}, 13.5f);
    Image oit, sorted;
    {
        CVarScope m("r.Translucency.Method", "OIT");
        oit = shot(cam);
        EXPECT_TRUE(ranPass("Translucency.OIT"));
        EXPECT_FALSE(ranPass("Translucency.Sorted"));
    }
    {
        CVarScope m("r.Translucency.Method", "Sorted");
        sorted = shot(cam);
        EXPECT_TRUE(ranPass("Translucency.Sorted"));
    }
    GoldenResult g1 = compareGolden("translucency_glass_oit", oit);
    EXPECT_TRUE(g1.matched) << g1.message;
    GoldenResult g2 = compareGolden("translucency_glass_sorted", sorted);
    EXPECT_TRUE(g2.matched) << g2.message;
    // Weighted blended OIT approximates the ordered result (it averages overlapping colours).
    const f64 diff = meanDifference(oit, sorted);
    std::printf("OIT vs sorted: mean difference %.2f / 255\n", diff);
    EXPECT_LT(diff, 6.0);
    // The panes tint what is behind them: red pane region is reddish.
    const glm::vec3 c = meanColor(oit, 60, 90, 80, 110);
    EXPECT_GT(c.r, c.b) << "red pane not visible";
}

TEST_F(TranslucencyTest, AutoMethodPicksSortedForSimpleScenes) {
    glassPanesScene();
    const CameraParams cam = camera({0.6f, 1.6f, 4.2f}, {0, 0.8f, 0}, 13.5f);
    CVarScope m("r.Translucency.Method", "Auto");
    {
        CVarScope n("r.Translucency.SortedMaxInstances", "4");
        shot(cam);
        EXPECT_TRUE(ranPass("Translucency.Sorted"));
        EXPECT_FALSE(ranPass("Translucency.OIT"));
    }
    {
        CVarScope n("r.Translucency.SortedMaxInstances", "1");
        shot(cam);
        EXPECT_TRUE(ranPass("Translucency.OIT"));
    }
}

// --- refraction ---------------------------------------------------------------------------------------------------

TEST_F(TranslucencyTest, RefractiveSphereAbsorption) {
    mesh(Primitive::Plane, checkerMaterial(8.0f), {0, 0, 0}, glm::vec3(10.0f));
    // Green-tinted glass (Beer-Lambert over the back-face thickness: colour reached after 1 m).
    mesh(Primitive::Sphere, glass({1, 1, 1, 1}, assets::BlendMode::Refractive, 0.02f, 1.5f, {0.45f, 0.9f, 0.55f}, 1.0f),
         {0, 0.75f, 0}, glm::vec3(1.5f));
    sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 30000.0f);
    environment();
    const CameraParams cam = camera({0.0f, 2.4f, 3.2f}, {0, 0.5f, 0}, 13.5f);
    Image img = shot(cam);
    EXPECT_TRUE(ranPass("Translucency.Refractive"));
    EXPECT_TRUE(ranPass("Translucency.BackfaceDepth"));
    GoldenResult g = compareGolden("translucency_refractive_sphere", img);
    EXPECT_TRUE(g.matched) << g.message;
    const glm::vec3 centre = meanColor(img, 118, 100, 138, 120);
    EXPECT_GT(centre.g, centre.r) << "absorption tint missing";

    // Without the back-face prepass the material thickness (0) is used: no absorption → less green.
    CVarScope b("r.Refraction.BackfaceDepth", "false");
    Image thin = shot(cam);
    EXPECT_FALSE(ranPass("Translucency.BackfaceDepth"));
    const glm::vec3 c2 = meanColor(thin, 118, 100, 138, 120);
    EXPECT_GT(c2.r / std::max(c2.g, 1e-3f), centre.r / std::max(centre.g, 1e-3f));
}

TEST_F(TranslucencyTest, FrostedGlass) {
    mesh(Primitive::Plane, checkerMaterial(8.0f), {0, 0, 0}, glm::vec3(10.0f));
    mesh(Primitive::Cube, glass({0.95f, 0.97f, 1.0f, 1}, assets::BlendMode::Refractive, 0.4f, 1.5f, {0.9f, 0.95f, 1.0f}, 1.0f),
         {0.6f, 0.9f, 0.4f}, {1.4f, 1.6f, 0.1f});
    mesh(Primitive::Cube, glass({0.95f, 0.97f, 1.0f, 1}, assets::BlendMode::Refractive, 0.02f, 1.5f), {-0.9f, 0.9f, 0.4f},
         {1.4f, 1.6f, 0.1f});
    sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 30000.0f);
    environment();
    const CameraParams cam = camera({0.0f, 1.6f, 3.6f}, {0, 0.6f, 0}, 13.5f);
    Image img = shot(cam);
    GoldenResult g = compareGolden("translucency_frosted_glass", img);
    EXPECT_TRUE(g.matched) << g.message;
    // The rough pane blurs the checker: much lower local contrast than through the clear pane.
    auto contrast = [&](u32 x0, u32 y0) {
        f32 lo = 1.0f, hi = 0.0f;
        for (u32 y = y0; y < y0 + 24; ++y)
            for (u32 x = x0; x < x0 + 24; ++x) lo = std::min(lo, img.luminance(x, y)), hi = std::max(hi, img.luminance(x, y));
        return hi - lo;
    };
    const f32 frosted = contrast(160, 140), clear = contrast(52, 140);
    std::printf("checker contrast through clear %.3f, frosted %.3f\n", clear, frosted);
    EXPECT_LT(frosted, clear * 0.6f);
}

// --- water --------------------------------------------------------------------------------------------------------

TEST_F(TranslucencyTest, WaterShoreFoamRefractionCaustics) {
    beachScene();
    const CameraParams cam = camera({0.0f, 3.2f, 6.5f}, {0, 0, -1.0f}, 14.0f);
    Image img = shot(cam);
    EXPECT_TRUE(ranPass("Water.Surface"));
    EXPECT_TRUE(ranPass("Water.Caustics"));
    GoldenResult g = compareGolden("translucency_water", img);
    EXPECT_TRUE(g.matched) << g.message;

    // Caustics brighten the underwater floor on average (multiplicative pattern).
    Image noCaustics;
    {
        CVarScope c("r.Water.Caustics", "false");
        noCaustics = shot(cam);
        EXPECT_FALSE(ranPass("Water.Caustics"));
    }
    const f64 diff = meanDifference(img, noCaustics);
    EXPECT_GT(diff, 0.3);
    // Water toggle removes the surface pass.
    CVarScope off("r.Feature.Water", "false");
    shot(cam);
    EXPECT_FALSE(ranPass("Water.Surface"));
}

TEST_F(TranslucencyTest, UnderwaterView) {
    beachScene();
    const CameraParams cam = camera({3.5f, -0.12f, 2.5f}, {1.0f, -0.3f, -3.0f}, 14.5f, 70.0f);
    Image img = shot(cam);
    EXPECT_TRUE(ranPass("Water.Underwater"));
    GoldenResult g = compareGolden("translucency_underwater", img);
    EXPECT_TRUE(g.matched) << g.message;
    const glm::vec3 c = meanColor(img, 0, 0, 256, 256);
    EXPECT_GT(c.g + c.b, c.r * 2.0f) << "underwater fog should be blue-green";

    // Above the surface the post effect does not run.
    shot(camera({0.0f, 3.2f, 6.5f}, {0, 0, -1.0f}, 14.0f));
    EXPECT_FALSE(ranPass("Water.Underwater"));
}

TEST_F(TranslucencyTest, GerstnerCpuMatchesPacking) {
    const std::vector<WaterWave> waves = {{{1.0f, 0.0f}, 8.0f, 0.3f, 0.6f, 0.2f, 1.0f}, {{0.0f, 1.0f}, 3.0f, 0.1f, 0.4f, 1.0f, 1.2f}};
    const GerstnerParams p = packGerstnerWaves(waves, 1.5f, 2.0f);
    EXPECT_EQ(p.info.x, 2.0f);
    EXPECT_FLOAT_EQ(p.waves[0].dirK.z, 6.28318530718f / 8.0f);
    EXPECT_NEAR(p.waves[0].dirK.w, std::sqrt(9.81f * p.waves[0].dirK.z), 1e-4f);
    EXPECT_NEAR(p.waves[0].amp.y, 0.6f / (p.waves[0].dirK.z * 2.0f), 1e-5f);
    // Eulerian height at a displaced point equals the Lagrangian height of its rest point.
    const glm::vec2 x0(3.0f, -2.0f);
    const glm::vec3 d = gerstnerDisplacement(p, x0, 2.0f);
    const f32 h = gerstnerHeight(p, x0 + glm::vec2(d.x, d.z), 2.0f, 8);
    EXPECT_NEAR(h, 1.5f + d.y, 1e-3f);
    EXPECT_NEAR(gerstnerAmplitudeSum(p), 0.4f, 1e-6f);
}

// --- particles ----------------------------------------------------------------------------------------------------

class ParticlesTest : public TranslucencyTest {
protected:
    void particleScene() {
        mesh(Primitive::Plane, material({0.1f, 0.1f, 0.11f, 1.0f}, 0.0f, 0.9f), {0, 0, 0}, glm::vec3(12.0f));
        mesh(Primitive::Cube, material({0.25f, 0.2f, 0.18f, 1.0f}, 0.0f, 0.7f), {0.9f, 0.5f, -0.3f}, glm::vec3(1.0f));
        sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 20000.0f);
        environment();

        Entity smoke = world->create("Smoke");
        smoke.setPosition({-0.4f, 0.05f, 0.0f});
        auto& s = smoke.add<ParticleEmitterComponent>();
        s.maxParticles = 512;
        s.spawnRate = 40.0f;
        s.lifetime = {2.5f, 3.5f};
        s.shape = ParticleShape::Sphere;
        s.radius = 0.25f;
        s.speed = {0.1f, 0.3f};
        s.velocity = {0.0f, 0.6f, 0.0f};
        s.drag = 0.4f;
        s.turbulence = 0.6f;
        s.turbulenceFrequency = 1.2f;
        s.size = {0.5f, 0.8f};
        s.sizeOverLife = {{0.0f, 0.6f}, {1.0f, 2.2f}};
        s.colorOverLife = {{0.0f, {0.9f, 0.9f, 0.92f, 0.0f}}, {0.15f, {0.9f, 0.9f, 0.92f, 0.6f}}, {1.0f, {0.8f, 0.8f, 0.82f, 0.0f}}};
        s.sprite = ParticleSprite::Smoke;
        s.blend = ParticleBlend::Alpha;
        s.lit = true;
        s.emissive = 0.0f;
        s.softDistance = 0.5f;
        s.rotationSpeed = {-30.0f, 30.0f};
        s.sort = true;
        s.seed = 3;

        Entity sparks = world->create("Sparks");
        sparks.setPosition({0.6f, 1.05f, 0.4f});
        auto& k = sparks.add<ParticleEmitterComponent>();
        k.maxParticles = 1024;
        k.spawnRate = 0.0f;
        k.bursts = {{0.0f, 80, 0, 0.5f}};
        k.lifetime = {0.8f, 1.4f};
        k.shape = ParticleShape::Cone;
        k.coneAngle = 40.0f;
        k.radius = 0.05f;
        k.speed = {1.5f, 3.0f};
        k.gravityScale = 1.0f;
        k.size = {0.03f, 0.05f};
        k.color = {1.0f, 0.55f, 0.15f, 1.0f};
        k.colorOverLife = {{0.0f, {1, 1, 1, 1}}, {1.0f, {1.0f, 0.3f, 0.1f, 0.0f}}};
        k.emissive = 6.0f;
        k.blend = ParticleBlend::Additive;
        k.renderMode = ParticleRenderMode::StretchedBillboard;
        k.stretch = 0.06f;
        k.sprite = ParticleSprite::Spark;
        k.collision = ParticleCollision::Bounce;
        k.bounce = 0.35f;
        k.seed = 7;
    }
};

TEST_F(ParticlesTest, SmokeAndSparksSoftParticles) {
    particleScene();
    const CameraParams cam = camera({0.0f, 1.4f, 4.0f}, {0, 0.9f, 0}, 12.5f);
    Image img = renderSim(cam, 90, 1.0f / 60.0f);
    EXPECT_TRUE(ranPass("Particles.Simulate"));
    EXPECT_TRUE(ranPass("Particles.Render"));
    EXPECT_TRUE(ranPass("Particles.Composite")); // half resolution at High
    GoldenResult g = compareGolden("translucency_particles", img, 3.0, 0.02);
    EXPECT_TRUE(g.matched) << g.message;

    // Low-res rendering + depth-aware upsampling stays close to full resolution (compared on 4×4 block averages:
    // sub-pixel sparks alias differently at half resolution).
    // Fresh renderer + world so the simulation restarts from the same state.
    device->waitIdle();
    renderer.reset();
    renderer = Renderer::create(*device);
    view = 0;
    world = std::make_unique<World>();
    particleScene();
    CVarScope full("r.Particles.ResolutionDivisor", "1");
    Image ref = renderSim(cam, 90, 1.0f / 60.0f);
    EXPECT_FALSE(ranPass("Particles.Composite"));
    writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "translucency_particles_fullres.png", ref);
    const f64 diff = meanDifference(blocks(img), blocks(ref));
    std::printf("particles half vs full resolution: mean difference %.2f / 255\n", diff);
    EXPECT_LT(diff, 4.0);
}

TEST_F(ParticlesTest, ParticlesVisibleAndToggle) {
    particleScene();
    const CameraParams cam = camera({0.0f, 1.4f, 4.0f}, {0, 0.9f, 0}, 12.5f);
    Image with = renderSim(cam, 40, 1.0f / 60.0f);
    CVarScope off("r.Particles", "false");
    Image without = renderSim(cam, 4, 1.0f / 60.0f);
    EXPECT_FALSE(ranPass("Particles.Render"));
    EXPECT_GT(meanDifference(with, without), 0.5);
}

TEST_F(ParticlesTest, MeshParticlesAndSurfaceEmission) {
    mesh(Primitive::Plane, material({0.4f, 0.4f, 0.4f, 1.0f}, 0.0f, 0.9f), {0, 0, 0}, glm::vec3(10.0f));
    sun(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)), 20000.0f);
    environment();
    Entity e = world->create("Debris");
    e.setPosition({0.0f, 1.0f, 0.0f});
    auto& p = e.add<ParticleEmitterComponent>();
    p.maxParticles = 256;
    p.spawnRate = 0.0f;
    p.bursts = {{0.0f, 64, 1, 1.0f}};
    p.lifetime = {5.0f, 5.0f};
    p.shape = ParticleShape::MeshSurface;
    p.shapeMesh = primitiveUuid(Primitive::Sphere);
    p.speed = {0.5f, 1.0f};
    p.size = {0.08f, 0.12f};
    p.renderMode = ParticleRenderMode::Mesh;
    p.lit = true;
    p.emissive = 0.0f;
    p.color = {0.9f, 0.3f, 0.2f, 1.0f};
    p.collision = ParticleCollision::Kill;
    const CameraParams cam = camera({0.0f, 1.6f, 3.0f}, {0, 1.0f, 0}, 12.5f);
    Image img = renderSim(cam, 10, 1.0f / 60.0f);
    const glm::vec3 c = meanColor(img, 96, 64, 160, 128);
    EXPECT_GT(c.r, c.b + 0.02f) << "red mesh particles not visible";
}

// --- alpha test ---------------------------------------------------------------------------------------------------

TEST_F(TranslucencyTest, AlphaTestedFoliage) {
    // Procedural leaf cluster: ellipses with alpha gradients + a vein hole pattern.
    std::vector<u8> px(128 * 128 * 4);
    for (u32 y = 0; y < 128; ++y) {
        for (u32 x = 0; x < 128; ++x) {
            f32 a = 0.0f;
            for (int l = 0; l < 5; ++l) {
                const f32 cx = 0.5f + 0.28f * std::cos(f32(l) * 1.256f), cy = 0.5f + 0.28f * std::sin(f32(l) * 1.256f);
                const f32 ang = f32(l) * 1.256f;
                const f32 u = (f32(x) / 128.0f - cx), v = (f32(y) / 128.0f - cy);
                const f32 ru = u * std::cos(ang) + v * std::sin(ang), rv = -u * std::sin(ang) + v * std::cos(ang);
                const f32 d = std::sqrt(ru * ru / 0.04f + rv * rv / 0.01f);
                a = std::max(a, std::clamp(1.2f - d, 0.0f, 1.0f));
            }
            const usize i = (usize(y) * 128 + x) * 4;
            px[i] = 60, px[i + 1] = u8(120 + (x * 7 % 40)), px[i + 2] = 40, px[i + 3] = u8(a * 255);
        }
    }
    assets::MaterialAsset leaf;
    leaf.blendMode = assets::BlendMode::AlphaTest;
    leaf.alphaCutoff = 0.5f;
    leaf.doubleSided = true;
    leaf.roughness = 0.6f;
    leaf.albedoTexture = texture(128, 128, px);
    const Uuid leafMat = materialEx(leaf);
    mesh(Primitive::Plane, material({0.5f, 0.5f, 0.55f, 1.0f}, 0.0f, 0.9f), {0, 0, 0}, glm::vec3(8.0f));
    mesh(Primitive::Plane, leafMat, {0, 1.0f, 0}, glm::vec3(1.8f), glm::angleAxis(glm::radians(80.0f), glm::vec3(1, 0, 0)));
    sun(glm::normalize(glm::vec3(-0.3f, -0.8f, -0.6f)), 30000.0f);
    environment();
    const CameraParams cam = camera({0.0f, 1.2f, 2.6f}, {0, 1.0f, 0}, 13.0f);
    CVarScope off("r.AlphaTest.Dither", "Off");
    Image img = shot(cam);
    GoldenResult g = compareGolden("translucency_foliage_alpha_test", img);
    EXPECT_TRUE(g.matched) << g.message;

    // Hashed alpha keeps the leaf coverage (TAA turns the dither into soft edges).
    auto leafPixels = [](const Image& im) {
        u32 n = 0;
        for (u32 y = 0; y < im.height; ++y)
            for (u32 x = 0; x < im.width; ++x) {
                const glm::u8vec4 c = im.at(x, y);
                if (c.g > c.r + 25 && c.g > c.b + 25) ++n;
            }
        return n;
    };
    CVarScope on("r.AlphaTest.Dither", "On");
    Image dithered = shot(cam);
    const f64 a = leafPixels(img), b = leafPixels(dithered);
    std::printf("foliage coverage: alpha test %.0f px, hashed %.0f px\n", a, b);
    EXPECT_GT(a, 500.0);
    EXPECT_NEAR(b / a, 1.0, 0.15);
    EXPECT_GT(meanDifference(img, dithered), 0.05);
}

// --- performance --------------------------------------------------------------------------------------------------

TEST_F(ParticlesTest, PerfReport1080p) {
    particleScene();
    glassPanesScene();
    mesh(Primitive::Sphere, glass({1, 1, 1, 1}, assets::BlendMode::Refractive, 0.02f, 1.5f, {0.4f, 0.8f, 0.5f}, 0.5f),
         {-1.5f, 0.6f, 1.0f}, glm::vec3(1.0f));
    mesh(Primitive::Cube, glass({1, 1, 1, 1}, assets::BlendMode::Refractive, 0.5f, 1.5f), {1.6f, 0.8f, 1.0f},
         {1.0f, 1.2f, 0.1f});
    water({0.0f, 0.02f, 6.0f}, {12.0f, 6.0f});
    for (auto& e : world->registry().view<ParticleEmitterComponent>()) {
        auto& p = world->registry().get<ParticleEmitterComponent>(e);
        p.maxParticles *= 8;
        p.spawnRate *= 8.0f;
        for (auto& b : p.bursts) b.count *= 8;
    }
    const CameraParams cam = camera({0.0f, 2.0f, 9.0f}, {0, 0.8f, 0}, 13.0f, 50.0f);
    const char* passes[] = {"Translucency.RefractionSource", "Translucency.DepthCopy", "Translucency.BackfaceDepth",
                            "Translucency.Refractive", "Translucency.OIT", "Translucency.Sorted", "Water.Caustics",
                            "Water.Surface", "Particles.Simulate", "Particles.LowResDepth", "Particles.Render",
                            "Particles.Composite"};
    for (QualityLevel q : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
        for (Scalability gr : {Scalability::Effects, Scalability::Shading, Scalability::Reflections}) scalability::setGroup(gr, q);
        // Average the last 16 retired frames (single-frame timestamps are noisy on MoltenVK).
        std::map<std::string, f64> sum;
        f64 frameSum = 0.0;
        u32 samples = 0;
        Image img = renderSim(cam, 40, 1.0f / 60.0f, 1920, 1080, [&](u32 f) {
            if (f < 24) return;
            for (const char* p : passes) sum[p] += passMs(p);
            frameSum += renderer->stats().gpuFrameMs;
            ++samples;
        });
        if (q == QualityLevel::High) writePng(std::filesystem::temp_directory_path() / "oxwald_render_out" / "translucency_perf_1080p.png", img);
        std::string line;
        for (const char* p : passes) line += std::format(" {} {:.3f}", p, sum[p] / samples);
        std::printf("translucency 1080p %-6s: frame %.2f ms |%s\n",
                    q == QualityLevel::Low ? "Low" : q == QualityLevel::Medium ? "Medium" : q == QualityLevel::High ? "High" : "Ultra",
                    frameSum / samples, line.c_str());
    }
    for (Scalability gr : {Scalability::Effects, Scalability::Shading, Scalability::Reflections})
        scalability::setGroup(gr, QualityLevel::High);
    EXPECT_GT(renderer->stats().gpuFrameMs, 0.0);
}

} // namespace
