// CPU-only tests: cluster math, CSM splits + texel snapping, shadow atlas, Halton jitter, extract, feature registry.
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/render.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/world.hpp>

#include <gtest/gtest.h>

#include <map>
#include <set>

using namespace ox;
using namespace ox::render;

// --- clusters ---

TEST(Clusters, SliceMathRoundTrips) {
    ClusterGrid g;
    g.nearPlane = 0.1f;
    g.farPlane = 500.0f;
    EXPECT_EQ(g.slice(0.1f), 0u);
    EXPECT_EQ(g.slice(0.05f), 0u);       // clamped below near
    EXPECT_EQ(g.slice(10000.0f), g.z - 1); // clamped beyond far
    for (u32 s = 0; s < g.z; ++s) {
        const f32 mid = std::sqrt(g.sliceNear(s) * g.sliceFar(s)); // geometric middle of an exponential slice
        EXPECT_EQ(g.slice(mid), s) << "slice " << s;
    }
    EXPECT_NEAR(g.sliceNear(0), 0.1f, 1e-5f);
    EXPECT_NEAR(g.sliceNear(g.z), 500.0f, 1e-2f);
    // Monotonic.
    for (f32 d = 0.1f; d < 500.0f; d *= 1.07f) EXPECT_LE(g.slice(d), g.slice(d * 1.07f));
}

TEST(Clusters, IndexAndBoundsCoverTheFrustum) {
    ClusterGrid g;
    g.nearPlane = 0.1f;
    g.farPlane = 100.0f;
    EXPECT_EQ(g.clusterIndex({0.0f, 0.0f}, 0.1f), 0u);
    EXPECT_EQ(g.clusterIndex({0.999f, 0.999f}, 99.0f), g.clusterCount() - 1);
    EXPECT_EQ(g.bufferSize(), u64(g.clusterCount()) * (1 + g.maxLightsPerCluster) * 4);

    CameraParams cam = CameraParams::lookAt({0, 0, 0}, {0, 0, -1}, 60.0f, 0.1f, 100.0f);
    const glm::mat4 proj = cam.projectionMatrix(16.0f / 9.0f);
    const glm::mat4 invProj = glm::inverse(proj);
    // A view-space point must lie inside the AABB of the cluster it maps to.
    Random rng(7);
    for (int i = 0; i < 500; ++i) {
        const glm::vec2 uv{rng.range(0.01f, 0.99f), rng.range(0.01f, 0.99f)};
        const f32 depth = std::exp(rng.range(std::log(0.2f), std::log(90.0f)));
        glm::vec4 p = invProj * glm::vec4(uv * 2.0f - 1.0f, 1.0f, 1.0f);
        glm::vec3 v = glm::vec3(p) / p.w;
        v *= depth / -v.z;
        const u32 idx = g.clusterIndex(uv, depth);
        const u32 cx = idx % g.x, cy = (idx / g.x) % g.y, cz = idx / (g.x * g.y);
        AABB box = g.clusterBounds(cx, cy, cz, invProj);
        box.min -= glm::vec3(1e-3f);
        box.max += glm::vec3(1e-3f);
        EXPECT_TRUE(sphereIntersectsAabb(v, 0.0f, box)) << "uv " << uv.x << "," << uv.y << " depth " << depth;
    }
}

TEST(Clusters, SphereAabbTest) {
    const AABB box = AABB::fromMinMax({0, 0, 0}, {1, 1, 1});
    EXPECT_TRUE(sphereIntersectsAabb({0.5f, 0.5f, 0.5f}, 0.1f, box));
    EXPECT_TRUE(sphereIntersectsAabb({1.5f, 0.5f, 0.5f}, 0.6f, box));
    EXPECT_FALSE(sphereIntersectsAabb({1.5f, 0.5f, 0.5f}, 0.4f, box));
    EXPECT_FALSE(sphereIntersectsAabb({2, 2, 2}, 1.7f, box)); // corner distance √3 ≈ 1.732
}

// --- CSM ---

TEST(Csm, PracticalSplitScheme) {
    const auto uni = cascadeSplits(1.0f, 100.0f, 4, 0.0f);
    EXPECT_NEAR(uni[0], 25.75f, 1e-3f);
    EXPECT_NEAR(uni[3], 100.0f, 1e-4f);
    const auto lg = cascadeSplits(1.0f, 100.0f, 4, 1.0f);
    EXPECT_NEAR(lg[0], std::pow(100.0f, 0.25f), 1e-3f);
    EXPECT_NEAR(lg[1], 10.0f, 1e-3f);
    const auto mix = cascadeSplits(1.0f, 100.0f, 4, 0.5f);
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(mix[i], 0.5f * (uni[i] + lg[i]), 1e-3f);
        if (i) EXPECT_GT(mix[i], mix[i - 1]);
    }
}

TEST(Csm, SubTexelCameraMotionKeepsSnappedMatrices) {
    CascadeInput in;
    in.cameraWorld = CameraParams::lookAt({3.3f, 2.0f, 7.1f}, {0, 0, 0}).world;
    in.aspect = 16.0f / 9.0f;
    in.shadowDistance = 80.0f;
    in.lightDirection = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.2f));
    in.resolution = 2048;
    const auto a = computeCascades(in);
    ASSERT_EQ(a.size(), 4u);
    // Move by a small fraction of the smallest texel along the light-space axes: matrices must stay identical
    // (unless a texel boundary is crossed, which this offset deliberately avoids by choosing the first cascade's
    // snapped origin as the reference).
    const f32 texel = a[0].texelWorld;
    int identical = 0;
    for (int k = 0; k < 20; ++k) {
        CascadeInput moved = in;
        moved.cameraWorld[3] += glm::vec4(texel * 0.01f * f32(k % 3), 0.0f, texel * 0.01f * f32(k / 7), 0.0f);
        const auto b = computeCascades(moved);
        bool same = true;
        for (u32 c = 0; c < 4; ++c) same &= b[c].viewProj == a[c].viewProj;
        identical += same;
    }
    EXPECT_GE(identical, 18) << "sub-texel motion should almost never change the snapped cascades";
    // Radii depend only on the split distances: rotating the camera does not change them.
    CascadeInput rotated = in;
    rotated.cameraWorld = CameraParams::lookAt({3.3f, 2.0f, 7.1f}, {10, 4, -3}).world;
    const auto r = computeCascades(rotated);
    for (u32 c = 0; c < 4; ++c) EXPECT_FLOAT_EQ(r[c].radius, a[c].radius);
    // Larger moves do change the matrices, by whole texels.
    CascadeInput far = in;
    far.cameraWorld[3] += glm::vec4(5.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_NE(computeCascades(far)[0].viewProj, a[0].viewProj);
}

TEST(Csm, CascadeContainsItsSlice) {
    CascadeInput in;
    in.cameraWorld = CameraParams::lookAt({0, 5, 10}, {0, 0, 0}).world;
    in.verticalFov = glm::radians(60.0f);
    in.aspect = 1.5f;
    in.shadowDistance = 60.0f;
    in.lightDirection = glm::normalize(glm::vec3(0.3f, -1.0f, 0.1f));
    const auto cs = computeCascades(in);
    const glm::vec3 camPos(in.cameraWorld[3]);
    const glm::vec3 fwd = -glm::vec3(in.cameraWorld[2]);
    for (const Cascade& c : cs) {
        // Points along the view axis inside the slice project inside the cascade's [0,1] uv / depth range.
        for (f32 t : {0.0f, 0.5f, 1.0f}) {
            const glm::vec3 p = camPos + fwd * glm::mix(c.splitNear, c.splitFar, t);
            glm::vec4 clip = c.viewProj * glm::vec4(p, 1.0f);
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            EXPECT_LE(std::abs(ndc.x), 1.0f);
            EXPECT_LE(std::abs(ndc.y), 1.0f);
            EXPECT_GE(ndc.z, 0.0f);
            EXPECT_LE(ndc.z, 1.0f);
        }
        EXPECT_NEAR(c.texelWorld, 2.0f * c.radius / f32(in.resolution), 1e-5f);
    }
}

// --- shadow atlas ---

TEST(ShadowAtlas, QuadtreeAllocatesWithoutOverlapAndFrees) {
    ShadowAtlasAllocator a(1024, 64);
    std::vector<ShadowAtlasAllocator::Tile> tiles;
    for (u32 size : {512u, 256u, 256u, 128u, 128u, 128u, 128u, 64u}) {
        auto t = a.allocate(size);
        ASSERT_TRUE(t.has_value()) << size;
        EXPECT_EQ(t->size, size);
        EXPECT_EQ(t->x % size, 0u);
        EXPECT_EQ(t->y % size, 0u);
        tiles.push_back(*t);
    }
    for (usize i = 0; i < tiles.size(); ++i)
        for (usize j = i + 1; j < tiles.size(); ++j) {
            const auto& p = tiles[i];
            const auto& q = tiles[j];
            const bool overlap = p.x < q.x + q.size && q.x < p.x + p.size && p.y < q.y + q.size && q.y < p.y + p.size;
            EXPECT_FALSE(overlap) << i << " vs " << j;
        }
    EXPECT_FALSE(a.allocate(1024).has_value());
    a.free(tiles[0]);
    EXPECT_TRUE(a.allocate(512).has_value()) << "freed 512 tile must be reusable";
    a.clear();
    EXPECT_TRUE(a.allocate(1024).has_value());
    EXPECT_FALSE(a.allocate(100).has_value()) << "atlas full after the 1024 tile";
}

TEST(ShadowAtlas, ImportanceDrivesResolutionAndBudget) {
    ShadowBudget b;
    b.atlasSize = 2048;
    b.minResolution = 128;
    b.maxResolution = 1024;
    b.maxShadowedLights = 4;
    b.maxPointLights = 1;
    std::vector<ShadowRequest> req;
    auto spot = [&](u32 id, glm::vec3 pos, f32 range) {
        ShadowRequest r;
        r.lightIndex = id;
        r.position = pos;
        r.range = range;
        req.push_back(r);
    };
    spot(0, {0, 0, -60}, 5);  // far, small → low importance
    spot(1, {0, 0, -8}, 5);   // near → high importance
    spot(2, {0, 0, -20}, 5);  // medium
    ShadowRequest pointNear;
    pointNear.lightIndex = 3;
    pointNear.point = true;
    pointNear.position = {1, 0, -3};
    pointNear.range = 4;
    req.push_back(pointNear);
    ShadowRequest pointFar = pointNear;
    pointFar.lightIndex = 4;
    pointFar.position = {0, 0, -40};
    req.push_back(pointFar);
    spot(5, {0, 0, -100}, 1); // barely visible: dropped by maxShadowedLights

    ShadowAtlasAllocator atlas;
    const auto allocs = allocateShadows(req, glm::vec3(0.0f), glm::radians(60.0f), 1080.0f, b, atlas);
    ASSERT_EQ(allocs.size(), 4u);
    std::map<u32, ShadowAllocation> by;
    for (const auto& a : allocs) by[a.lightIndex] = a;
    EXPECT_TRUE(by.count(1) && by.count(2) && by.count(0) && by.count(3));
    EXPECT_FALSE(by.count(4)) << "only one point light slot";
    EXPECT_FALSE(by.count(5));
    EXPECT_GT(by[1].resolution, by[2].resolution);
    EXPECT_GE(by[2].resolution, by[0].resolution);
    EXPECT_EQ(by[3].cubeSlot, 0u);
    // Importance ordering of the result.
    for (usize i = 1; i < allocs.size(); ++i) EXPECT_GE(allocs[i - 1].importance, allocs[i].importance);
    // Inside the light's range importance is the full viewport height.
    EXPECT_FLOAT_EQ(shadowImportance(pointNear, glm::vec3(1, 0, -2), glm::radians(60.0f), 1080.0f), 1080.0f);
}

TEST(ShadowAtlas, FullAtlasDownsizesLowerImportanceLights) {
    ShadowBudget b;
    b.atlasSize = 1024;
    b.minResolution = 128;
    b.maxResolution = 1024;
    b.maxShadowedLights = 16;
    std::vector<ShadowRequest> req;
    for (u32 i = 0; i < 6; ++i) {
        ShadowRequest r;
        r.lightIndex = i;
        r.position = {0, 0, -2.0f - f32(i)};
        r.range = 10; // all inside range → equal importance
        req.push_back(r);
    }
    ShadowAtlasAllocator atlas;
    const auto allocs = allocateShadows(req, glm::vec3(0.0f), glm::radians(60.0f), 1080.0f, b, atlas);
    EXPECT_EQ(allocs.size(), 6u) << "lights are downsized instead of dropped";
    u64 texels = 0;
    for (const auto& a : allocs) texels += u64(a.resolution) * a.resolution;
    EXPECT_LE(texels, 1024ull * 1024ull);
}

// --- point light faces ---

TEST(PointShadows, FaceMatricesMatchShaderMapping) {
    const glm::vec3 pos{1, 2, 3};
    const f32 fov = cubeFaceFov(512, 4.0f);
    EXPECT_GT(fov, glm::radians(90.0f));
    const f32 tanHalf = std::tan(fov * 0.5f);
    Random rng(3);
    for (int i = 0; i < 200; ++i) {
        const glm::vec3 d = rng.unitVector() * rng.range(0.5f, 9.0f);
        // Shader-side face selection and projection (see shadows.glsl oxPointShadow).
        const glm::vec3 a = glm::abs(d);
        const u32 face = a.x >= a.y && a.x >= a.z ? (d.x >= 0 ? 0 : 1) : a.y >= a.z ? (d.y >= 0 ? 2 : 3) : (d.z >= 0 ? 4 : 5);
        const glm::vec3 f = cubeFaceForward(face), u = cubeFaceUp(face), s = glm::normalize(glm::cross(f, u));
        const f32 zf = glm::dot(d, f);
        const glm::vec2 ndc = glm::vec2(glm::dot(d, s), glm::dot(d, u)) / (zf * tanHalf);
        const f32 n = 0.05f, fa = 10.0f;
        const f32 depth = n * (fa - zf) / (zf * (fa - n));
        const glm::vec4 clip = cubeFaceViewProj(pos, face, fov, n, fa) * glm::vec4(pos + d, 1.0f);
        EXPECT_NEAR(clip.x / clip.w, ndc.x, 1e-4f);
        EXPECT_NEAR(clip.y / clip.w, ndc.y, 1e-4f);
        EXPECT_NEAR(clip.z / clip.w, depth, 1e-4f);
        EXPECT_LE(std::abs(ndc.x), 1.0f / tanHalf + 1e-4f);
    }
}

// --- jitter ---

TEST(Jitter, HaltonSequence) {
    EXPECT_FLOAT_EQ(halton(1, 2), 0.5f);
    EXPECT_FLOAT_EQ(halton(2, 2), 0.25f);
    EXPECT_FLOAT_EQ(halton(3, 2), 0.75f);
    EXPECT_NEAR(halton(1, 3), 1.0f / 3.0f, 1e-6f);
    EXPECT_NEAR(halton(2, 3), 2.0f / 3.0f, 1e-6f);
    EXPECT_NEAR(halton(4, 3), 4.0f / 9.0f, 1e-6f); // 4 = 11₃ → 1/3 + 1/9
    EXPECT_EQ(haltonJitter(5, 0), glm::vec2(0.0f));
    std::set<std::pair<int, int>> unique;
    glm::vec2 mean(0.0f);
    for (u64 f = 0; f < 16; ++f) {
        const glm::vec2 j = haltonJitter(f, 16);
        EXPECT_GT(j.x, -0.5f);
        EXPECT_LE(j.x, 0.5f);
        EXPECT_GT(j.y, -0.5f);
        EXPECT_LE(j.y, 0.5f);
        mean += j;
        unique.insert({int(j.x * 1e4f), int(j.y * 1e4f)});
    }
    EXPECT_EQ(unique.size(), 16u);
    EXPECT_LT(glm::length(mean / 16.0f), 0.08f) << "well distributed around the pixel centre";
    EXPECT_EQ(haltonJitter(3, 8), haltonJitter(11, 8)) << "periodic";
}

TEST(Camera, ProjectionHasVulkanYFlipAndReversedZ) {
    CameraParams c = CameraParams::lookAt({0, 0, 0}, {0, 0, -1}, 60.0f, 0.5f, 100.0f);
    const glm::mat4 vp = c.projectionMatrix(1.0f) * c.viewMatrix();
    auto ndc = [&](glm::vec3 p) {
        glm::vec4 h = vp * glm::vec4(p, 1.0f);
        return glm::vec3(h) / h.w;
    };
    EXPECT_NEAR(ndc({0, 0, -0.5f}).z, 1.0f, 1e-5f);  // near → 1
    EXPECT_NEAR(ndc({0, 0, -100.0f}).z, 0.0f, 1e-5f); // far → 0
    EXPECT_LT(ndc({0, 1, -5}).y, 0.0f) << "world up maps to NDC -y (top of the image)";
    c.farPlane = 0.0f;
    const glm::vec4 inf = c.projectionMatrix(1.0f) * glm::vec4(0, 0, -1e6f, 1);
    EXPECT_NEAR(inf.z / inf.w, 0.0f, 1e-5f);
}

// --- extract ---

TEST(Extract, SnapshotContainsRenderableComponents) {
    registerSceneTypes();
    World world;
    Entity cam = world.create("Camera");
    cam.add<CameraComponent>().primary = true;
    cam.setPosition({0, 1, 5});
    Entity parent = world.create("Parent");
    parent.setPosition({10, 0, 0});
    Entity box = world.create("Box", parent);
    box.setPosition({1, 0, 0});
    auto& mr = box.add<MeshRendererComponent>();
    mr.mesh = primitiveUuid(Primitive::Cube);
    mr.materials = {Uuid::fromName("a"), Uuid::fromName("b")};
    mr.castShadows = false;
    Entity hidden = world.create("Hidden");
    hidden.add<MeshRendererComponent>().mesh = primitiveUuid(Primitive::Sphere);
    hidden.setActive(false);
    Entity sun = world.create("Sun");
    sun.add<LightComponent>().type = LightType::Directional;
    Entity lamp = world.create("Lamp");
    lamp.add<LightComponent>().type = LightType::Point;
    lamp.setPosition({0, 3, 0});
    Entity env = world.create("Env");
    env.add<EnvironmentComponent>().sun = sun.ref();

    world.updateTransforms();
    world.snapshotPreviousTransforms();
    parent.setPosition({11, 0, 0}); // moves the child: previous != current
    world.updateTransforms();

    DebugDraw dd;
    dd.line({0, 0, 0}, {1, 0, 0}, debug_color::kRed);
    dd.line({0, 0, 0}, {0, 1, 0}, debug_color::kGreen, 0.0f, false);
    dd.flush(0.0f);

    RenderSnapshot snap;
    snap.selection = {42};
    extract(world, snap, {.debugDraw = &dd, .time = 2.5, .deltaTime = 0.016f, .frame = 7});
    EXPECT_EQ(snap.frame, 7u);
    EXPECT_DOUBLE_EQ(snap.time, 2.5);
    ASSERT_EQ(snap.cameras.size(), 1u);
    EXPECT_EQ(snap.primaryCamera(), 0);
    ASSERT_EQ(snap.meshes.size(), 1u) << "inactive entity skipped";
    const SnapshotMesh& m = snap.meshes[0];
    EXPECT_EQ(m.entityId, encodeEntityId(u32(entt::to_integral(box.handle()))));
    EXPECT_EQ(m.mesh, primitiveUuid(Primitive::Cube));
    ASSERT_EQ(m.materialCount, 2u);
    EXPECT_EQ(snap.materials[m.materialOffset + 1], Uuid::fromName("b"));
    EXPECT_EQ(m.flags & kMeshCastShadows, 0u);
    EXPECT_NEAR(m.world[3].x, 12.0f, 1e-5f);
    EXPECT_NEAR(m.prevWorld[3].x, 11.0f, 1e-5f) << "previous transform for motion vectors";
    ASSERT_EQ(snap.lights.size(), 2u);
    ASSERT_TRUE(snap.environment.has_value());
    ASSERT_GE(snap.environment->sunLight, 0);
    EXPECT_EQ(snap.lights[usize(snap.environment->sunLight)].light.type, LightType::Directional);
    EXPECT_EQ(snap.debugLines.size(), 2u);
    EXPECT_EQ(snap.debugLinesOverlay.size(), 2u);
    EXPECT_EQ(snap.selection, std::vector<u32>{42}) << "editor selection survives extraction";
}

TEST(Extract, SnapshotBufferAlternatesSlots) {
    SnapshotBuffer b;
    b.writeSlot().frame = 1;
    b.publish();
    EXPECT_EQ(b.readSlot().frame, 1u);
    b.writeSlot().frame = 2;
    EXPECT_EQ(b.readSlot().frame, 1u) << "writing does not disturb the published snapshot";
    b.publish();
    EXPECT_EQ(b.readSlot().frame, 2u);
}

// --- feature registry ---

namespace {
struct TestFeature final : IRenderFeature {
    std::string n;
    InjectionMask mask;
    std::string group;
    i32 prio, ord;
    bool enabled;
    TestFeature(std::string name, InjectionMask m, std::string g = {}, i32 p = 0, i32 o = 0, bool e = true)
        : n(std::move(name)), mask(m), group(std::move(g)), prio(p), ord(o), enabled(e) {}
    std::string_view name() const override { return n; }
    InjectionMask injectionPoints() const override { return mask; }
    std::string_view exclusiveGroup() const override { return group; }
    i32 priority() const override { return prio; }
    i32 order() const override { return ord; }
    bool isEnabled(const RenderSettings& s, const rhi::DeviceCaps& caps) const override {
        return enabled && (group != "Shadows" || prio == 0 || (s.rayTracing && caps.rayTracingSupported()));
    }
    void setup(FeatureContext&) override {}
};
} // namespace

TEST(FeatureRegistry, ExclusiveGroupsOrderingAndToggles) {
    FeatureRegistry reg;
    reg.emplace<TestFeature>("ShadowsRasterT", maskOf(InjectionPoint::Shadows), "Shadows", 0);
    reg.emplace<TestFeature>("ShadowsRTT", maskOf(InjectionPoint::Shadows), "Shadows", 100);
    reg.emplace<TestFeature>("BloomT", maskOf(InjectionPoint::PostProcess), "", 0, 200);
    reg.emplace<TestFeature>("ExposureT", maskOf(InjectionPoint::PostProcess), "", 0, 100);
    reg.emplace<TestFeature>("FSR1T", maskOf(InjectionPoint::Upscale), "", 0);
    reg.emplace<TestFeature>("DLSST", maskOf(InjectionPoint::Upscale), "", 10);
    reg.emplace<TestFeature>("SSRT", maskOf(InjectionPoint::AfterDepth, InjectionPoint::AfterOpaque));

    RenderSettings s;
    rhi::DeviceCaps noRt;
    auto names = [](const std::vector<IRenderFeature*>& v) {
        std::vector<std::string> out;
        for (auto* f : v) out.emplace_back(f->name());
        return out;
    };
    auto resolved = reg.resolve(s, noRt);
    auto shadows = names(FeatureRegistry::at(resolved, InjectionPoint::Shadows));
    EXPECT_EQ(shadows, std::vector<std::string>{"ShadowsRasterT"});
    s.rayTracing = true;
    EXPECT_EQ(names(FeatureRegistry::at(reg.resolve(s, noRt), InjectionPoint::Shadows)),
              std::vector<std::string>{"ShadowsRasterT"}) << "RT variant needs caps";
    rhi::DeviceCaps rt;
    rt.accelerationStructure = rt.rayQuery = true;
    EXPECT_EQ(names(FeatureRegistry::at(reg.resolve(s, rt), InjectionPoint::Shadows)),
              std::vector<std::string>{"ShadowsRTT"}) << "exactly one feature per exclusive group";

    EXPECT_EQ(names(FeatureRegistry::at(resolved, InjectionPoint::PostProcess)),
              (std::vector<std::string>{"ExposureT", "BloomT"})) << "ordered by order()";
    EXPECT_EQ(names(FeatureRegistry::at(resolved, InjectionPoint::Upscale)), std::vector<std::string>{"DLSST"})
        << "Upscale is a single slot";
    EXPECT_EQ(FeatureRegistry::at(resolved, InjectionPoint::AfterOpaque).size(), 1u);
    EXPECT_EQ(FeatureRegistry::at(resolved, InjectionPoint::AfterDepth).size(), 1u);

    FeatureRegistry::toggle("BloomT").set(false);
    EXPECT_TRUE(FeatureRegistry::at(reg.resolve(s, noRt), InjectionPoint::PostProcess).size() == 1u);
    FeatureRegistry::toggle("BloomT").set(true);
    EXPECT_NE(CVarRegistry::instance().find("r.Feature.BloomT"), nullptr);
    EXPECT_TRUE(reg.remove("BloomT"));
    EXPECT_EQ(reg.find("BloomT"), nullptr);
}

TEST(FeatureRegistry, FactoriesAndInjectionNames) {
    registerFeatureFactory("FactoryT", [] { return std::make_unique<TestFeature>("FactoryT", maskOf(InjectionPoint::Debug)); });
    bool found = false;
    for (auto& [name, f] : featureFactories()) found |= name == "FactoryT" && f() != nullptr;
    EXPECT_TRUE(found);
    EXPECT_STREQ(injectionPointName(InjectionPoint::Translucency), "Translucency");
    EXPECT_EQ(maskOf(InjectionPoint::PreDepth, InjectionPoint::Debug), 1u | (1u << u32(InjectionPoint::Debug)));
}

// --- settings / quality ---

TEST(Settings, CVarsDriveTheSnapshotAndScalability) {
    registerRenderCVars();
    auto& reg = CVarRegistry::instance();
    ASSERT_NE(reg.find("r.ScreenPercentage"), nullptr);
    ASSERT_NE(reg.find("r.Shadows.CSM.Resolution"), nullptr);
    ASSERT_NE(reg.find("r.RayTracing"), nullptr);
    ASSERT_NE(reg.find("r.Upscaler"), nullptr);
    scalability::setGroup(Scalability::Shadows, QualityLevel::Low);
    RenderSettings low = RenderSettings::fromCVars();
    EXPECT_EQ(low.csmResolution, 1024);
    EXPECT_EQ(low.csmCascades, 2);
    EXPECT_FALSE(low.pcss);
    scalability::setGroup(Scalability::Shadows, QualityLevel::Ultra);
    RenderSettings ultra = RenderSettings::fromCVars();
    EXPECT_EQ(ultra.csmResolution, 4096);
    EXPECT_TRUE(ultra.pcss);
    EXPECT_NE(low.topologyHash(), ultra.topologyHash());
    scalability::setGroup(Scalability::Shadows, QualityLevel::High);
    reg.set("r.Tonemapper", "AgX", CVarSource::Code);
    EXPECT_EQ(RenderSettings::fromCVars().tonemapper, Tonemapper::AgX);
    reg.set("r.Tonemapper", "ACES", CVarSource::Code);
}

TEST(Quality, ScoreToLevels) {
    auto low = levelsForScore(10.0, false);
    auto mid = levelsForScore(50.0, false);
    auto ultra = levelsForScore(400.0, true);
    EXPECT_EQ(low[usize(Scalability::Shadows)], QualityLevel::Low);
    EXPECT_EQ(mid[usize(Scalability::Shadows)], QualityLevel::Medium);
    EXPECT_EQ(levelsForScore(100.0, false)[usize(Scalability::Textures)], QualityLevel::High);
    EXPECT_EQ(ultra[usize(Scalability::Shadows)], QualityLevel::Ultra);
    EXPECT_EQ(ultra[usize(Scalability::RayTracing)], QualityLevel::High);
    EXPECT_EQ(levelsForScore(400.0, false)[usize(Scalability::RayTracing)], QualityLevel::Low);
}

// --- primitives ---

TEST(Primitives, AllPrimitivesAreClosedWellFormedMeshes) {
    for (u32 p = 0; p < u32(Primitive::Count); ++p) {
        const assets::MeshData m = makePrimitive(Primitive(p));
        SCOPED_TRACE(primitiveName(Primitive(p)));
        ASSERT_GT(m.positions.size(), 3u);
        EXPECT_EQ(m.positions.size(), m.attributes.size());
        EXPECT_EQ(m.indices.size() % 3, 0u);
        for (u32 i : m.indices) ASSERT_LT(i, m.positions.size());
        ASSERT_EQ(m.submeshes.size(), 1u);
        EXPECT_EQ(m.submeshes[0].lods[0].indexCount, m.indices.size());
        EXPECT_TRUE(m.bounds.valid());
        // Outward winding: geometric normal agrees with the vertex normals.
        u32 agree = 0;
        for (usize t = 0; t < m.indices.size(); t += 3) {
            const glm::vec3 a = m.positions[m.indices[t]], b = m.positions[m.indices[t + 1]], c = m.positions[m.indices[t + 2]];
            const glm::vec3 n = m.attributes[m.indices[t]].normal;
            agree += glm::dot(glm::cross(b - a, c - a), n) > 0.0f;
        }
        EXPECT_EQ(agree, m.indices.size() / 3);
        for (const auto& a : m.attributes) {
            EXPECT_NEAR(glm::length(a.normal), 1.0f, 1e-3f);
            EXPECT_NEAR(glm::length(glm::vec3(a.tangent)), 1.0f, 1e-3f);
            EXPECT_NEAR(std::abs(glm::dot(glm::vec3(a.tangent), a.normal)), 0.0f, 1e-3f);
        }
    }
    EXPECT_NE(primitiveUuid(Primitive::Cube), primitiveUuid(Primitive::Sphere));
    const assets::MeshData cube = makePrimitive(Primitive::Cube, {.size = 2.0f});
    EXPECT_NEAR(cube.bounds.max.x, 1.0f, 1e-5f);
}

TEST(RangeAllocator, ReusesAndCoalescesFreedRanges) {
    RangeAllocator a(100);
    const u64 x = a.allocate(10), y = a.allocate(20), z = a.allocate(30);
    EXPECT_EQ(x, 0u);
    EXPECT_EQ(y, 10u);
    EXPECT_EQ(z, 30u);
    a.free(y, 20);
    EXPECT_EQ(a.allocate(15), 10u);
    a.free(x, 10);
    EXPECT_EQ(a.allocate(10), 0u);
    EXPECT_EQ(a.allocate(50), ~0ull);
    a.grow(200);
    EXPECT_EQ(a.allocate(50), 60u);
}
