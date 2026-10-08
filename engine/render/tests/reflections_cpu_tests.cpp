// CPU tests of the reflections-ao area: capture / mirror / oblique projection math, baked containers, components,
// extract hook, cvars + scalability.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/core/serial/value.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>
#include <oxwald/render/register_types.hpp>
#include <oxwald/render/render_view.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <filesystem>

using namespace ox;
using namespace ox::render;
using namespace ox::render::reflections;

namespace {

// C++ mirror of oxCubeDirection (render/common/math.glsl).
glm::vec3 cubeDirection(u32 face, glm::vec2 uv) {
    const glm::vec2 p = uv * 2.0f - 1.0f;
    switch (face) {
    case 0: return glm::normalize(glm::vec3(1.0f, -p.y, -p.x));
    case 1: return glm::normalize(glm::vec3(-1.0f, -p.y, p.x));
    case 2: return glm::normalize(glm::vec3(p.x, 1.0f, p.y));
    case 3: return glm::normalize(glm::vec3(p.x, -1.0f, -p.y));
    case 4: return glm::normalize(glm::vec3(p.x, -p.y, 1.0f));
    default: return glm::normalize(glm::vec3(-p.x, -p.y, -1.0f));
    }
}

glm::vec3 project(const glm::mat4& viewProj, glm::vec3 p) {
    const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
    return glm::vec3(c) / c.w;
}

} // namespace

TEST(ReflectionsMath, CubeFaceMatricesMatchShaderDirections) {
    const glm::vec3 origin(1.0f, 2.0f, -3.0f);
    for (u32 face = 0; face < 6; ++face) {
        glm::mat4 view, proj;
        cubeFaceMatrices(face, origin, 0.1f, view, proj);
        const glm::mat4 vp = proj * view;
        for (glm::vec2 uv : {glm::vec2(0.5f), glm::vec2(0.1f, 0.2f), glm::vec2(0.9f, 0.3f), glm::vec2(0.25f, 0.8f)}) {
            const glm::vec3 dir = cubeDirection(face, uv);
            const glm::vec3 ndc = project(vp, origin + dir * 5.0f);
            const glm::vec2 got = glm::vec2(ndc) * 0.5f + 0.5f;
            EXPECT_NEAR(got.x, uv.x, 1e-4f) << "face " << face;
            EXPECT_NEAR(got.y, uv.y, 1e-4f) << "face " << face;
            EXPECT_GT(ndc.z, 0.0f);
            EXPECT_LT(ndc.z, 1.0f);
        }
        // Reversed-Z: nearer points have larger depth.
        const glm::vec3 d = cubeDirection(face, glm::vec2(0.5f));
        EXPECT_GT(project(vp, origin + d * 1.0f).z, project(vp, origin + d * 10.0f).z);
        // Mirrored relative to a regular camera (det(view) > 0 with a Y-flipping projection): captures rasterise
        // clockwise front faces.
        EXPECT_GT(glm::determinant(glm::mat3(view)) * proj[1][1], 0.0f);
    }
}

TEST(ReflectionsMath, RegularCameraOrientation) {
    const CameraParams cam = CameraParams::lookAt({1, 2, 3}, {0, 0, 0});
    EXPECT_LT(glm::determinant(glm::mat3(cam.viewMatrix())) * cam.projectionMatrix(1.0f)[1][1], 0.0f);
}

TEST(ReflectionsMath, ReflectionMatrix) {
    const glm::vec3 n = glm::normalize(glm::vec3(0.2f, 1.0f, -0.1f));
    const glm::vec4 plane(n, -glm::dot(n, glm::vec3(0.0f, 1.5f, 0.0f)));
    const glm::mat4 m = reflectionMatrix(plane);
    const glm::vec3 p(2.0f, 4.0f, -1.0f);
    const glm::vec3 r(m * glm::vec4(p, 1.0f));
    const f32 dp = glm::dot(n, p) + plane.w, dr = glm::dot(n, r) + plane.w;
    EXPECT_NEAR(dp, -dr, 1e-4f);
    EXPECT_NEAR(glm::length(glm::cross(r - p, n)), 0.0f, 1e-4f); // moved along the normal only
    const glm::mat4 mm = m * m;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) EXPECT_NEAR(mm[c][rr], c == rr ? 1.0f : 0.0f, 1e-5f);
}

TEST(ReflectionsMath, ObliqueReversedZClipsAtThePlane) {
    const CameraParams cam = CameraParams::lookAt({0.0f, 3.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 60.0f, 0.1f, 0.0f);
    const glm::mat4 infinite = cam.projectionMatrix(16.0f / 9.0f);
    const glm::mat4 finite = finiteReversedZ(infinite, 0.1f, 100.0f);
    // Finite reversed-Z: near → 1, far → 0, same x/y.
    EXPECT_NEAR(project(finite, {0, 0, -0.1f}).z, 1.0f, 1e-4f);
    EXPECT_NEAR(project(finite, {0, 0, -100.0f}).z, 0.0f, 1e-4f);
    EXPECT_NEAR(project(finite, {1, 1, -5}).x, project(infinite, {1, 1, -5}).x, 1e-5f);
    EXPECT_NEAR(project(finite, {1, 1, -5}).y, project(infinite, {1, 1, -5}).y, 1e-5f);

    // Mirror about y = 0: the clip plane (in the mirrored view) becomes the near plane.
    const glm::vec4 plane(0, 1, 0, 0);
    const glm::mat4 reflView = cam.viewMatrix() * reflectionMatrix(plane);
    const glm::vec4 clipView = glm::transpose(glm::inverse(reflView)) * plane;
    const glm::mat4 oblique = obliqueReversedZ(finite, clipView);
    const glm::mat4 vp = oblique * reflView;
    // A point on the plane in front of the camera: depth 1 (near).
    EXPECT_NEAR(project(vp, {0.5f, 0.0f, -1.0f}).z, 1.0f, 1e-3f);
    // Points above the plane (visible in the mirror) lie inside [0, 1]; below the plane they are clipped (> 1).
    const f32 above = project(vp, {0.0f, 1.0f, -2.0f}).z;
    EXPECT_GT(above, 0.0f);
    EXPECT_LT(above, 1.0f);
    EXPECT_GT(project(vp, {0.0f, -1.0f, -2.0f}).z, 1.0f);
    // x/y of the oblique projection are unchanged.
    EXPECT_NEAR(project(vp, {0.3f, 1.0f, -2.0f}).x, project(finite * reflView, {0.3f, 1.0f, -2.0f}).x, 1e-4f);
}

TEST(ReflectionsBaked, OxCubeRoundTrip) {
    BakedCubemap c;
    c.size = 8;
    c.mips = 2;
    c.data.resize((8 * 8 + 4 * 4) * 6 * 8);
    for (usize i = 0; i < c.data.size(); ++i) c.data[i] = u8(i * 7);
    const auto path = std::filesystem::temp_directory_path() / "oxwald_reflections_test" / "probe.oxcube";
    ASSERT_TRUE(saveOxCube(path, c));
    auto loaded = loadOxCube(path);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->size, 8u);
    EXPECT_EQ(loaded->mips, 2u);
    EXPECT_EQ(loaded->data, c.data);
    c.mips = 3; // inconsistent with the data size
    EXPECT_FALSE(saveOxCube(path, c));
    EXPECT_FALSE(loadOxCube(path.parent_path() / "missing.oxcube"));
}

TEST(ReflectionsBaked, OxIrradianceRoundTrip) {
    BakedIrradianceVolume v;
    v.probeCount = {2, 3, 2};
    v.probes.resize(12);
    for (usize i = 0; i < v.probes.size(); ++i) v.probes[i].sh[0] = glm::vec4(f32(i), 1.0f, 2.0f, 1.0f);
    v.moments.assign(640 * 10 * 8, 3);
    const auto path = std::filesystem::temp_directory_path() / "oxwald_reflections_test" / "volume.oxirr";
    ASSERT_TRUE(saveOxIrradiance(path, v));
    auto loaded = loadOxIrradiance(path);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->probeCount, v.probeCount);
    ASSERT_EQ(loaded->probes.size(), 12u);
    EXPECT_EQ(loaded->probes[5].sh[0].x, 5.0f);
    EXPECT_EQ(loaded->moments, v.moments);
}

TEST(ReflectionsTypes, ComponentsAreReflectedAndExtracted) {
    registerSceneTypes();
    registerRenderTypes();
    ASSERT_NE(ComponentRegistry::instance().find("ReflectionProbe"), nullptr);
    ASSERT_NE(ComponentRegistry::instance().find("PlanarReflector"), nullptr);
    ASSERT_NE(ComponentRegistry::instance().find("IrradianceVolume"), nullptr);

    ReflectionProbeComponent probe;
    probe.extents = {3, 2, 1};
    probe.update = ReflectionProbeUpdate::Realtime;
    probe.priority = 7;
    const serial::Value v = serial::toValue(probe);
    ReflectionProbeComponent back;
    serial::fromValue(v, back);
    EXPECT_EQ(back.extents, probe.extents);
    EXPECT_EQ(back.update, ReflectionProbeUpdate::Realtime);
    EXPECT_EQ(back.priority, 7);

    World world;
    Entity a = world.create("Probe");
    a.setPosition({1, 2, 3});
    a.add<ReflectionProbeComponent>();
    Entity b = world.create("Mirror");
    b.add<PlanarReflectorComponent>().size = {2.0f, 3.0f};
    Entity c = world.create("Volume");
    c.add<IrradianceVolumeComponent>().probeCount = {3, 2, 3};
    Entity d = world.create("Disabled");
    d.add<ReflectionProbeComponent>().enabled = false;
    world.updateTransforms();
    RenderSnapshot snap;
    extract(world, snap);
    const ReflectionSnapshot* ext = snap.findExtension<ReflectionSnapshot>();
    ASSERT_NE(ext, nullptr);
    ASSERT_EQ(ext->probes.size(), 1u);
    EXPECT_EQ(glm::vec3(ext->probes[0].world[3]), glm::vec3(1, 2, 3));
    EXPECT_TRUE(ext->probes[0].uuid.isValid());
    ASSERT_EQ(ext->planars.size(), 1u);
    EXPECT_EQ(ext->planars[0].reflector.size, glm::vec2(2.0f, 3.0f));
    ASSERT_EQ(ext->volumes.size(), 1u);
    EXPECT_EQ(ext->volumes[0].volume.probeCount, glm::ivec3(3, 2, 3));
    // Re-extract keeps the extension object but clears it.
    a.get<ReflectionProbeComponent>().enabled = false;
    extract(world, snap);
    EXPECT_TRUE(snap.findExtension<ReflectionSnapshot>()->probes.empty());
}

TEST(ReflectionsCVars, ScalabilityLevels) {
    registerReflectionCVars();
    auto& reg = CVarRegistry::instance();
    for (const char* name : {"r.SSR", "r.SSR.Quality", "r.SSR.MaxSteps", "r.SSR.HalfRes", "r.ReflectionProbes.Resolution",
                             "r.ReflectionProbes.Realtime", "r.PlanarReflections.ResolutionScale", "r.AO.Method",
                             "r.AO.Quality", "r.AO.HalfRes", "r.GI.IrradianceVolumes"}) {
        EXPECT_NE(reg.find(name), nullptr) << name;
    }
    const auto before = scalability::currentLevel(Scalability::Reflections);
    const auto beforeGi = scalability::currentLevel(Scalability::GlobalIllumination);
    scalability::setGroup(Scalability::Reflections, QualityLevel::Low);
    scalability::setGroup(Scalability::GlobalIllumination, QualityLevel::Low);
    EXPECT_EQ(reg.find("r.SSR")->toString(), "false");
    EXPECT_EQ(reg.find("r.AO.Method")->toString(), "1");
    EXPECT_EQ(reg.find("r.AO.HalfRes")->toString(), "true");
    scalability::setGroup(Scalability::Reflections, QualityLevel::Ultra);
    scalability::setGroup(Scalability::GlobalIllumination, QualityLevel::Ultra);
    EXPECT_EQ(reg.find("r.SSR")->toString(), "true");
    EXPECT_EQ(reg.find("r.SSR.MaxSteps")->toString(), "96");
    EXPECT_EQ(reg.find("r.ReflectionProbes.Resolution")->toString(), "256");
    EXPECT_EQ(reg.find("r.AO.Method")->toString(), "2");
    EXPECT_EQ(reg.find("r.AO.Quality")->toString(), "3");
    // Restore (other tests in this process expect the declared defaults).
    for (Scalability g : {Scalability::Reflections, Scalability::GlobalIllumination}) {
        if (ICVar* sg = reg.find("sg." + std::string(scalability::groupName(g)))) sg->reset();
        for (ICVar* c : reg.inGroup(g)) c->reset();
    }
    (void)before;
    (void)beforeGi;
}
