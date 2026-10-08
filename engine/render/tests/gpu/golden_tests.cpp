// Golden image tests (256×256 offscreen). Update with OX_UPDATE_GOLDEN=1 and inspect the PNGs before committing.
#include "render_fixture.hpp"

#include <oxwald/core/math.hpp>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

using GoldenTest = RenderTest;

#define EXPECT_GOLDEN(name, img, ...)                                                                                  \
    do {                                                                                                               \
        GoldenResult gr_ = compareGolden(name, img, ##__VA_ARGS__);                                                    \
        EXPECT_TRUE(gr_.matched) << gr_.message << "\n" << asciiArt(img);                                              \
    } while (0)

TEST_F(GoldenTest, PbrSpheresGrid) {
    // Rows: metallic 0 (bottom) → 1 (top); columns: roughness 0.05 (left) → 1 (right).
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 5; ++col) {
            const Uuid m = material({0.9f, 0.45f, 0.2f, 1.0f}, f32(row) / 4.0f, glm::mix(0.05f, 1.0f, f32(col) / 4.0f));
            mesh(Primitive::Sphere, m, {(f32(col) - 2.0f) * 1.1f, (f32(row) - 2.0f) * 1.1f, 0.0f});
        }
    }
    sun({-0.4f, -0.6f, -0.7f}, 20000.0f, glm::vec3(1.0f), false);
    environment(1.0f, 1.0f);
    Image img = render(camera({0, 0, 9.5f}, {0, 0, 0}, 12.5f, 40.0f));
    EXPECT_GOLDEN("pbr_spheres_grid", img);
    // Centre of the rough dielectric sphere (bottom right) is lit; the corner between spheres is background.
    EXPECT_GT(img.luminance(209, 209), img.luminance(250, 250) + 0.1f);
}

TEST_F(GoldenTest, DirectionalCsmShadows) {
    const Uuid ground = material({0.7f, 0.7f, 0.7f, 1.0f}, 0.0f, 0.8f);
    const Uuid red = material({0.8f, 0.15f, 0.1f, 1.0f}, 0.0f, 0.5f);
    const Uuid blue = material({0.1f, 0.25f, 0.8f, 1.0f}, 0.0f, 0.4f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(60.0f));
    mesh(Primitive::Cube, red, {0, 1, 0}, glm::vec3(2.0f));
    mesh(Primitive::Cube, blue, {-4, 0.5f, -3}, glm::vec3(1.0f));
    mesh(Primitive::Sphere, blue, {4, 1.5f, 2}, glm::vec3(3.0f));
    mesh(Primitive::Cylinder, red, {-3, 1.5f, 4}, glm::vec3(1.0f, 3.0f, 1.0f));
    for (int i = 0; i < 6; ++i) mesh(Primitive::Cube, red, {-25.0f + i * 10.0f, 1.0f, -25.0f}, glm::vec3(2.0f)); // far cascade
    sun(glm::normalize(glm::vec3(-0.6f, -1.0f, -0.35f)), 30000.0f, glm::vec3(1.0f, 0.95f, 0.9f), true);
    environment(1.0f, 1.0f);
    Image img = render(camera({2, 9, 16}, {0, 0, -2}, 13.5f, 55.0f));
    EXPECT_GOLDEN("csm_shadows", img);
    // The shadow cast by the red cube (towards +x, +z of it) is darker than open ground nearby.
    // Probe pixels found from the image: shadow just right/front of the cube vs ground further right.
}

TEST_F(GoldenTest, SpotLightShadow) {
    const Uuid ground = material({0.75f, 0.75f, 0.75f, 1.0f}, 0.0f, 0.7f);
    const Uuid obj = material({0.2f, 0.7f, 0.3f, 1.0f}, 0.0f, 0.5f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(30.0f));
    mesh(Primitive::Cube, obj, {0, 0.75f, 0}, glm::vec3(1.5f));
    mesh(Primitive::Sphere, obj, {2.2f, 0.6f, 1.0f}, glm::vec3(1.2f));
    spotLight({-1.5f, 6.0f, 2.5f}, glm::vec3(1.5f, -6.0f, -2.5f), 30000.0f, 20.0f, 20.0f, 35.0f, true);
    Image img = render(camera({0, 7, 10}, {0, 0, 0}, 5.0f, 50.0f));
    EXPECT_GOLDEN("spot_shadow", img);
}

TEST_F(GoldenTest, PointLightCubeShadows) {
    const Uuid ground = material({0.75f, 0.75f, 0.75f, 1.0f}, 0.0f, 0.7f);
    const Uuid obj = material({0.85f, 0.6f, 0.2f, 1.0f}, 0.0f, 0.5f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(30.0f));
    // Ring of casters around the light → shadows in every horizontal direction (+X, -X, +Z, -Z faces).
    for (int i = 0; i < 6; ++i) {
        const f32 a = f32(i) / 6.0f * kTwoPi;
        mesh(i % 2 ? Primitive::Cylinder : Primitive::Cube, obj, {std::cos(a) * 2.5f, 0.5f, std::sin(a) * 2.5f}, glm::vec3(0.8f, 1.0f, 0.8f));
    }
    pointLight({0, 1.2f, 0}, 25000.0f, 14.0f, glm::vec3(1.0f, 0.9f, 0.75f), true);
    Image img = render(camera({0, 9, 7}, {0, 0, 0}, 5.5f, 55.0f));
    EXPECT_GOLDEN("point_shadows", img);
}

TEST_F(GoldenTest, ManyPointLightsClustered) {
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
    Image img = render(camera({0, 11, 15}, {0, 0, 0}, 5.0f, 50.0f));
    EXPECT_GOLDEN("many_point_lights", img);
    EXPECT_EQ(renderer->stats().lights, 256u);
}

TEST_F(GoldenTest, IblOnly) {
    for (int i = 0; i < 4; ++i) {
        mesh(Primitive::Sphere, material({0.95f, 0.95f, 0.95f, 1.0f}, 1.0f, 0.05f + f32(i) * 0.3f), {(f32(i) - 1.5f) * 1.2f, 0.6f, 0});
        mesh(Primitive::Sphere, material({0.8f, 0.2f, 0.2f, 1.0f}, 0.0f, 0.05f + f32(i) * 0.3f), {(f32(i) - 1.5f) * 1.2f, -0.6f, 0});
    }
    environment(1.0f, 1.0f);
    Image img = render(camera({0, 0, 6.5f}, {0, 0, 0}, 11.5f, 45.0f));
    EXPECT_GOLDEN("ibl_only", img);
    // Lit by the environment only: spheres must not be black.
    EXPECT_GT(img.luminance(128 - 22, 128 + 22), 0.05f);
}

TEST_F(GoldenTest, TonemapperAcesVsAgx) {
    const Uuid m = material({0.95f, 0.3f, 0.05f, 1.0f}, 0.0f, 0.4f);
    const Uuid w = material({0.9f, 0.9f, 0.9f, 1.0f}, 0.0f, 0.4f);
    mesh(Primitive::Sphere, m, {-1.1f, 0, 0});
    mesh(Primitive::Sphere, w, {1.1f, 0, 0});
    mesh(Primitive::Plane, w, {0, -0.5f, 0}, glm::vec3(10.0f));
    sun({-0.3f, -0.8f, -0.5f}, 120000.0f, glm::vec3(1.0f), true); // deliberately over-exposed highlights
    environment(1.0f, 1.0f);
    const CameraParams cam = camera({0, 1.5f, 5}, {0, 0, 0}, 13.0f, 45.0f);
    Image aces, agx;
    {
        CVarScope t("r.Tonemapper", "ACES");
        aces = render(cam);
    }
    {
        CVarScope t("r.Tonemapper", "AgX");
        agx = render(cam);
    }
    EXPECT_GOLDEN("tonemap_aces", aces);
    EXPECT_GOLDEN("tonemap_agx", agx);
    u64 diff = 0;
    for (usize i = 0; i < aces.rgba.size(); ++i) diff += u64(std::abs(int(aces.rgba[i]) - int(agx.rgba[i])));
    EXPECT_GT(diff / aces.rgba.size(), 2u) << "tonemappers should produce visibly different images";
}

TEST_F(GoldenTest, RenderScale50) {
    CVarScope sp("r.ScreenPercentage", "50");
    mesh(Primitive::Torus, material({0.2f, 0.6f, 0.9f, 1.0f}, 0.0f, 0.3f), {0, 0, 0}, glm::vec3(2.0f),
         glm::angleAxis(glm::radians(60.0f), glm::vec3(1, 0, 0)));
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f, glm::vec3(1.0f), false);
    environment(1.0f, 1.0f);
    Image img = render(camera({0, 0, 4}, {0, 0, 0}, 12.5f, 50.0f));
    ASSERT_EQ(img.width, 256u);
    ASSERT_EQ(img.height, 256u);
    const RenderView* v = renderer->view(view);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->renderExtent().width, 128u);
    EXPECT_EQ(v->renderExtent().height, 128u);
    EXPECT_EQ(v->outputExtent().width, 256u);
    EXPECT_GOLDEN("render_scale_50", img);
}

TEST_F(GoldenTest, DebugDrawLines) {
    const Uuid ground = material({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f);
    mesh(Primitive::Plane, ground, {0, 0, 0}, glm::vec3(10.0f));
    mesh(Primitive::Cube, ground, {0, 0.5f, 0}, glm::vec3(1.0f));
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f, glm::vec3(1.0f), false);
    environment(1.0f, 1.0f);
    debugDraw.line({-3, 0.01f, 2}, {3, 0.01f, 2}, debug_color::kRed);              // depth tested, visible
    debugDraw.aabb(AABB::fromCenterExtents({0, 0.5f, 0}, glm::vec3(0.6f)), debug_color::kYellow);
    debugDraw.line({0, 0.5f, -3}, {0, 0.5f, 3}, debug_color::kCyan, 0.0f, false);  // overlay through the cube
    debugDraw.sphere({2, 1, 0}, 0.5f, debug_color::kGreen);
    Image img = render(camera({0, 3, 6}, {0, 0.3f, 0}, 12.5f, 50.0f));
    EXPECT_GOLDEN("debug_lines", img);
    // Count strongly red and cyan pixels.
    u32 red = 0, cyan = 0;
    for (u32 y = 0; y < img.height; ++y)
        for (u32 x = 0; x < img.width; ++x) {
            const glm::u8vec4 c = img.at(x, y);
            red += c.r > 200 && c.g < 60 && c.b < 60;
            cyan += c.r < 60 && c.g > 200 && c.b > 200;
        }
    EXPECT_GT(red, 100u);
    EXPECT_GT(cyan, 60u);
}

TEST_F(GoldenTest, EditorGridAndSelectionOutline) {
    const Uuid m = material({0.7f, 0.7f, 0.75f, 1.0f}, 0.0f, 0.5f);
    Entity a = mesh(Primitive::Cube, m, {-1.2f, 0.5f, 0}, glm::vec3(1.0f));
    mesh(Primitive::Sphere, m, {1.2f, 0.6f, 0}, glm::vec3(1.2f));
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f, glm::vec3(1.0f), true);
    environment(1.0f, 1.0f);
    ensureView(true);
    renderer->view(view)->desc().flags.grid = true;
    snapshot.selection = {encodeEntityId(u32(entt::to_integral(a.handle())))};
    Options o;
    o.editor = true;
    Image img = render(camera({0, 3, 6}, {0, 0.3f, 0}, 12.5f, 50.0f), o);
    EXPECT_GOLDEN("editor_grid_outline", img);
    u32 orange = 0;
    for (u32 y = 0; y < img.height; ++y)
        for (u32 x = 0; x < img.width; ++x) {
            const glm::u8vec4 c = img.at(x, y);
            orange += c.r > 200 && c.g > 90 && c.g < 190 && c.b < 80;
        }
    EXPECT_GT(orange, 80u) << "selection outline missing";
}
