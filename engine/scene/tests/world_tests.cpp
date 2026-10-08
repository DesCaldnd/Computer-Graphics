#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/quaternion.hpp>

using namespace ox;

namespace {

struct WorldTest : ::testing::Test {
    void SetUp() override { registerSceneTypes(); }
    World world;
};

bool near(const glm::vec3& a, const glm::vec3& b, f32 eps = 1e-4f) { return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(eps))); }

} // namespace

TEST_F(WorldTest, CreateHasCoreComponentsAndUuidIndex) {
    Entity e = world.create("Player");
    ASSERT_TRUE(e.valid());
    EXPECT_TRUE((e.has<IdComponent, NameComponent, TransformComponent, HierarchyComponent, WorldTransformComponent, ActiveComponent>()));
    EXPECT_EQ(e.name(), "Player");
    EXPECT_TRUE(e.uuid().isValid());
    EXPECT_EQ(world.find(e.uuid()), e);
    EXPECT_EQ(world.resolve(e.ref()), e);
    EXPECT_EQ(world.findByName("Player"), e);
    EXPECT_EQ(world.entityCount(), 1u);

    auto& light = e.add<LightComponent>();
    light.intensity = 5.0f;
    EXPECT_TRUE(e.has<LightComponent>());
    EXPECT_EQ(e.get<LightComponent>().intensity, 5.0f);
    e.remove<LightComponent>();
    EXPECT_FALSE(e.has<LightComponent>());
    EXPECT_EQ(e.tryGet<LightComponent>(), nullptr);
}

TEST_F(WorldTest, DeferredDestroyRemovesSubtreeAtFlush) {
    Entity a = world.create("A");
    Entity b = world.create("B", a);
    Entity c = world.create("C", b);
    Entity other = world.create("Other");
    const Uuid cid = c.uuid();
    b.destroy();
    EXPECT_TRUE(b.valid()) << "destruction is deferred";
    EXPECT_TRUE(c.has<PendingDestroyTag>());
    EXPECT_EQ(world.flushDestroyed(), 2u);
    EXPECT_FALSE(b.valid());
    EXPECT_FALSE(c.valid());
    EXPECT_FALSE(world.find(cid).valid());
    EXPECT_TRUE(a.children().empty());
    EXPECT_TRUE(other.valid());
    world.destroyImmediate(a);
    EXPECT_FALSE(a.valid());
    EXPECT_EQ(world.roots().size(), 1u);
}

TEST_F(WorldTest, HierarchyOrderAndCycles) {
    Entity root = world.create("Root");
    Entity c1 = world.create("C1", root);
    Entity c2 = world.create("C2", root);
    Entity c3 = world.create("C3");
    c3.setParent(root, false, 0);
    ASSERT_EQ(root.childCount(), 3u);
    EXPECT_EQ(root.children()[0], c3);
    EXPECT_EQ(root.children()[1], c1);
    EXPECT_EQ(c1.parent(), root);
    EXPECT_TRUE(root.isAncestorOf(c2));
    EXPECT_FALSE(c2.isAncestorOf(root));
    c2.setParent({}, false);
    EXPECT_FALSE(c2.parent().valid());
    EXPECT_EQ(world.roots().back(), c2);

    std::vector<std::string> order;
    world.forEachInHierarchy([&](entt::entity e) { order.push_back(world.wrap(e).name()); });
    EXPECT_EQ(order, (std::vector<std::string>{"Root", "C3", "C1", "C2"}));
}

TEST_F(WorldTest, WorldTransformsPropagateThroughHierarchy) {
    Entity parent = world.create("Parent");
    Entity child = world.create("Child", parent);
    parent.setPosition({10, 0, 0});
    parent.setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)));
    parent.setScale(glm::vec3(2.0f));
    child.setPosition({1, 0, 0});

    EXPECT_GT(world.updateTransforms(), 0u);
    const glm::vec3 expected = glm::vec3(10, 0, -2); // +X rotated 90° about Y is -Z, scaled by 2
    EXPECT_TRUE(near(glm::vec3(child.get<WorldTransformComponent>().matrix[3]), expected));
    EXPECT_TRUE(near(child.worldPosition(), expected));
    EXPECT_EQ(world.updateTransforms(), 0u) << "nothing dirty";

    // Only the dirty subtree is recomputed.
    Entity unrelated = world.create("Unrelated");
    world.updateTransforms();
    child.setPosition({2, 0, 0});
    EXPECT_EQ(world.updateTransforms(), 1u);
    EXPECT_TRUE(near(child.worldPosition(), glm::vec3(10, 0, -4)));
    (void)unrelated;

    // Moving the parent moves the child.
    parent.setPosition({0, 5, 0});
    EXPECT_EQ(world.updateTransforms(), 2u);
    EXPECT_TRUE(near(glm::vec3(child.get<WorldTransformComponent>().matrix[3]), glm::vec3(0, 5, -4)));

    // patch() on the registry also marks dirty.
    child.patch<TransformComponent>([](TransformComponent& t) { t.position = {0, 0, 0}; });
    world.updateTransforms();
    EXPECT_TRUE(near(glm::vec3(child.get<WorldTransformComponent>().matrix[3]), glm::vec3(0, 5, 0)));
}

TEST_F(WorldTest, PreviousMatrixForMotionVectors) {
    Entity e = world.create("Mover");
    e.setPosition({1, 0, 0});
    world.updateTransforms();
    world.snapshotPreviousTransforms();
    e.setPosition({2, 0, 0});
    world.updateTransforms();
    const auto& wt = e.get<WorldTransformComponent>();
    EXPECT_EQ(wt.previous[3].x, 1.0f);
    EXPECT_EQ(wt.matrix[3].x, 2.0f);
}

TEST_F(WorldTest, ReparentKeepsWorldTransform) {
    Entity a = world.create("A");
    a.setPosition({5, 0, 0});
    a.setRotation(glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1)));
    a.setScale(glm::vec3(3.0f));
    Entity b = world.create("B");
    b.setPosition({1, 2, 3});
    b.setRotation(glm::angleAxis(glm::radians(30.0f), glm::vec3(1, 0, 0)));
    const glm::mat4 before = b.worldMatrix();

    b.setParent(a, true);
    EXPECT_TRUE(nearlyEqual(b.worldMatrix(), before, 1e-4f));
    EXPECT_FALSE(near(b.localTransform().position, glm::vec3(1, 2, 3)));

    b.setParent({}, true);
    EXPECT_TRUE(nearlyEqual(b.worldMatrix(), before, 1e-4f));
    EXPECT_TRUE(near(b.localTransform().position, glm::vec3(1, 2, 3)));

    // Without keepWorldTransform the local transform is kept.
    b.setParent(a, false);
    EXPECT_TRUE(near(b.localTransform().position, glm::vec3(1, 2, 3)));
}

TEST_F(WorldTest, WorldSpaceSetters) {
    Entity parent = world.create("P");
    parent.setPosition({0, 10, 0});
    parent.setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)));
    Entity child = world.create("C", parent);
    child.setWorldPosition({3, 4, 5});
    EXPECT_TRUE(near(child.worldPosition(), glm::vec3(3, 4, 5)));
    const glm::quat q = glm::angleAxis(glm::radians(10.0f), glm::vec3(1, 0, 0));
    child.setWorldRotation(q);
    EXPECT_TRUE(nearlyEqual(child.worldRotation(), q, 1e-4f));
    Transform t;
    t.position = {7, 8, 9};
    t.rotation = q;
    child.setWorldTransform(t);
    EXPECT_TRUE(near(child.worldTransform().position, t.position));
}

TEST_F(WorldTest, ActiveInHierarchy) {
    Entity a = world.create("A");
    Entity b = world.create("B", a);
    EXPECT_TRUE(b.activeInHierarchy());
    a.setActive(false);
    EXPECT_TRUE(b.active());
    EXPECT_FALSE(b.activeInHierarchy());
}

TEST_F(WorldTest, CloneIsDeepAndIndependent) {
    Entity a = world.create("A");
    Entity b = world.create("B", a);
    b.add<LightComponent>().intensity = 42.0f;
    b.add<EnvironmentComponent>().sun = a.ref();
    world.create("Doomed").destroy(); // pending destroy is carried over
    world.updateTransforms();

    auto copy = world.clone();
    ASSERT_EQ(copy->entityCount(), world.entityCount());
    Entity cb = copy->find(b.uuid());
    ASSERT_TRUE(cb.valid());
    EXPECT_EQ(cb.name(), "B");
    EXPECT_EQ(cb.parent().uuid(), a.uuid());
    EXPECT_EQ(cb.get<LightComponent>().intensity, 42.0f);
    EXPECT_EQ(copy->resolve(cb.get<EnvironmentComponent>().sun).name(), "A");

    cb.get<LightComponent>().intensity = 1.0f;
    copy->find(a.uuid()).setPosition({9, 9, 9});
    EXPECT_EQ(b.get<LightComponent>().intensity, 42.0f);
    EXPECT_EQ(a.localTransform().position, glm::vec3(0.0f));
    copy->updateTransforms();
    EXPECT_TRUE(near(cb.worldPosition(), glm::vec3(9, 9, 9)));
    EXPECT_EQ(copy->flushDestroyed(), 1u);
    EXPECT_EQ(world.entityCount(), 3u);
}

TEST_F(WorldTest, CameraExposureAndProjection) {
    CameraComponent cam;
    cam.aperture = 1.0f;
    cam.shutterSpeed = 1.0f;
    cam.iso = 100.0f;
    EXPECT_NEAR(cam.ev100(), 0.0f, 1e-5f);
    cam.aperture = 16.0f;
    cam.shutterSpeed = 1.0f / 100.0f;
    EXPECT_NEAR(cam.ev100(), std::log2(256.0f * 100.0f), 1e-3f); // sunny-16 ≈ EV 14.6
    const glm::mat4 p = cam.projectionMatrix(16.0f / 9.0f);
    const glm::vec4 nearPt = p * glm::vec4(0, 0, -cam.nearPlane, 1);
    const glm::vec4 farPt = p * glm::vec4(0, 0, -cam.farPlane, 1);
    EXPECT_NEAR(nearPt.z / nearPt.w, 1.0f, 1e-4f) << "reversed-Z: near maps to 1";
    EXPECT_NEAR(farPt.z / farPt.w, 0.0f, 1e-4f);
}

TEST_F(WorldTest, ComponentRegistryGenericAccess) {
    const auto& reg = ComponentRegistry::instance();
    const ComponentInfo* light = reg.find("Light");
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->category, "Rendering");
    EXPECT_EQ(light->icon, "light");
    EXPECT_TRUE(light->removable);
    EXPECT_EQ(reg.find<LightComponent>(), light);
    EXPECT_FALSE(reg.find("Transform")->removable);
    EXPECT_TRUE(reg.find("Hierarchy")->hiddenInInspector);

    Entity e = world.create("E");
    EXPECT_FALSE(light->has(world, e.handle()));
    light->add(world, e.handle());
    EXPECT_TRUE(e.has<LightComponent>());
    auto ref = light->ref(world, e.handle());
    ASSERT_TRUE(reflect::resolvePath(ref, "color.g").setAs(0.25f));
    EXPECT_EQ(e.get<LightComponent>().color.g, 0.25f);
    ASSERT_TRUE(reflect::resolvePath(ref, "type").set(serial::Value::makeEnum("Spot")));
    EXPECT_EQ(e.get<LightComponent>().type, LightType::Spot);

    // Editing the transform through the generic path + notifyChanged marks it dirty.
    world.updateTransforms();
    const ComponentInfo* tr = reg.find("Transform");
    ASSERT_TRUE(reflect::resolvePath(tr->ref(world, e.handle()), "position.y").setAs(3.0f));
    tr->notifyChanged(world, e.handle());
    world.updateTransforms();
    EXPECT_EQ(e.get<WorldTransformComponent>().matrix[3].y, 3.0f);

    const auto comps = reg.componentsOf(world, e.handle());
    EXPECT_NE(std::find(comps.begin(), comps.end(), light), comps.end());
    light->remove(world, e.handle());
    EXPECT_FALSE(e.has<LightComponent>());
}
