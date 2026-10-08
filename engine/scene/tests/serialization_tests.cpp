#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

using namespace ox;

namespace {

struct FollowerComponent {
    EntityRef target;
    std::vector<EntityRef> waypoints;
    f32 speed = 1.0f;
    f32 runtimeOnly = 0.0f;
};

void registerTestComponents() {
    registerSceneTypes();
    OX_REFLECT_TYPE(FollowerComponent, "Test.Follower")
        .attributes(attr::Category{"Gameplay"})
        .field("target", &FollowerComponent::target)
        .field("waypoints", &FollowerComponent::waypoints)
        .field("speed", &FollowerComponent::speed, attr::SaveGame{})
        .field("runtimeOnly", &FollowerComponent::runtimeOnly, attr::NoSerialize{});
    ComponentRegistry::instance().add<FollowerComponent>();
}

// A small but feature-complete scene: hierarchy, every core component, entity references.
void buildScene(World& world) {
    Entity sun = world.create("Sun");
    sun.setRotation(glm::angleAxis(glm::radians(-45.0f), glm::vec3(1, 0, 0)));
    auto& l = sun.add<LightComponent>();
    l.type = LightType::Directional;
    l.intensity = 100000.0f;
    l.color = {1.0f, 0.95f, 0.9f};

    Entity env = world.create("Environment");
    auto& envc = env.add<EnvironmentComponent>();
    envc.sun = sun.ref();
    envc.skybox = Uuid::fromName("sky.hdr");
    envc.fogEnabled = true;

    Entity cam = world.create("Camera");
    cam.add<CameraComponent>().primary = true;
    cam.setPosition({0, 2, 5});

    Entity car = world.create("Car");
    car.setPosition({1, 0, 0});
    auto& mr = car.add<MeshRendererComponent>();
    mr.mesh = Uuid::fromName("car.mesh");
    mr.materials = {Uuid::fromName("paint.mat"), Uuid::fromName("glass.mat")};
    car.add<TagComponent>().tags = {"vehicle", "player"};
    car.add<SaveGameComponent>();
    for (int i = 0; i < 4; ++i) {
        Entity wheel = world.create("Wheel" + std::to_string(i), car);
        wheel.setPosition({i < 2 ? -1.0f : 1.0f, 0.3f, i % 2 ? -1.5f : 1.5f});
        wheel.add<MeshRendererComponent>().mesh = Uuid::fromName("wheel.mesh");
    }
    auto& follower = cam.add<FollowerComponent>();
    follower.target = car.ref();
    follower.waypoints = {sun.ref(), env.ref()};
    follower.speed = 3.5f;
    follower.runtimeOnly = 99.0f;
    world.updateTransforms();
}

struct SceneSerialization : ::testing::Test {
    void SetUp() override { registerTestComponents(); }
};

void expectSameScene(const World& a, const World& b) {
    ASSERT_EQ(a.entityCount(), b.entityCount());
    World& wa = const_cast<World&>(a);
    a.forEachInHierarchy([&](entt::entity h) {
        Entity ea{h, &wa};
        Entity eb = b.find(ea.uuid());
        ASSERT_TRUE(eb.valid()) << ea.name();
        EXPECT_EQ(ea.name(), eb.name());
        EXPECT_EQ(ea.parent().uuid(), eb.parent().uuid());
        EXPECT_EQ(ea.childCount(), eb.childCount());
        for (usize i = 0; i < ea.childCount(); ++i) EXPECT_EQ(ea.children()[i].uuid(), eb.children()[i].uuid());
        for (const ComponentInfo* info : ComponentRegistry::instance().componentsOf(a, h)) {
            if (!info->serializable) continue;
            ASSERT_TRUE(info->has(b, eb.handle())) << info->name;
            EXPECT_EQ(info->serialize(a, h), info->serialize(b, eb.handle())) << ea.name() << "." << info->name;
        }
        EXPECT_TRUE(nearlyEqual(ea.worldMatrix(), eb.worldMatrix()));
    });
}

std::filesystem::path tempDir() {
    auto dir = std::filesystem::temp_directory_path() / ("ox_scene_" + Uuid::generate().toString());
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_F(SceneSerialization, RoundTripBinaryAndJson) {
    World world;
    buildScene(world);
    const auto dir = tempDir();
    ASSERT_TRUE(saveScene(world, dir / "level.oxscene"));
    ASSERT_TRUE(saveScene(world, dir / "level.oxscene.json"));

    World fromBinary;
    auto st = loadScene(fromBinary, dir / "level.oxscene");
    ASSERT_TRUE(st) << st.error().message;
    expectSameScene(world, fromBinary);

    World fromJson;
    st = loadScene(fromJson, dir / "level.oxscene.json");
    ASSERT_TRUE(st) << st.error().message;
    expectSameScene(world, fromJson);

    // Reference resolution and NoSerialize.
    Entity cam = fromBinary.findByName("Camera");
    const auto& f = cam.get<FollowerComponent>();
    EXPECT_EQ(fromBinary.resolve(f.target).name(), "Car");
    EXPECT_EQ(fromBinary.resolve(f.waypoints[1]).name(), "Environment");
    EXPECT_EQ(f.runtimeOnly, 0.0f);
    EXPECT_EQ(fromBinary.resolve(fromBinary.findByName("Environment").get<EnvironmentComponent>().sun).name(), "Sun");

    // JSON is human readable: enums by name, components by name.
    auto text = serial::readFileBytes(dir / "level.oxscene.json");
    ASSERT_TRUE(text);
    const std::string json(reinterpret_cast<const char*>(text->data()), text->size());
    EXPECT_NE(json.find("\"Directional\""), std::string::npos);
    EXPECT_NE(json.find("\"MeshRenderer\""), std::string::npos);

    // Saving the loaded world again gives the same bytes (deterministic writer).
    const auto a = serial::encodeBinary(serializeWorld(world));
    const auto b = serial::encodeBinary(serializeWorld(fromJson));
    EXPECT_EQ(a, b);

    // Loading into a world that already has these ids fails cleanly.
    EXPECT_FALSE(loadScene(fromBinary, dir / "level.oxscene"));
    std::filesystem::remove_all(dir);
}

TEST_F(SceneSerialization, UnknownComponentsArePreserved) {
    World world;
    buildScene(world);
    auto doc = serializeWorld(world);
    // Simulate data from a module this build does not have: a "Ghost" component referencing the car.
    const Uuid carId = world.findByName("Car").uuid();
    serial::Value* entities = doc.root.find("entities");
    serial::Value& camValue = [&]() -> serial::Value& {
        for (auto& ev : entities->items()) {
            if (ev.find("name")->getString() == "Camera") return ev;
        }
        throw std::runtime_error("no camera");
    }();
    serial::Value ghost = serial::Value::makeObject("Ghost");
    ghost.set("haunts", serial::Value::makeEntityRef(carId));
    ghost.set("scariness", serial::Value::makeF32(11.0f));
    ghost.set("moans", serial::Value::makeArray());
    ghost.find("moans")->push(serial::Value::makeString("boo"));
    camValue.find("components")->set("Ghost", ghost);

    const auto bytes = serial::encodeBinary(doc);
    World loaded;
    ASSERT_TRUE(deserializeWorld(loaded, *serial::decodeBinary(bytes)));
    Entity cam = loaded.findByName("Camera");
    ASSERT_TRUE(cam.has<UnknownComponents>());
    EXPECT_EQ(cam.get<UnknownComponents>().entries.front().first, "Ghost");

    // Saved again without loss: identical bytes.
    EXPECT_EQ(serial::encodeBinary(serializeWorld(loaded)), bytes);

    // Copy/paste remaps the ghost's reference to the pasted car.
    Entity selection[] = {loaded.findByName("Car"), cam};
    auto clip = copyEntities(loaded, selection);
    auto pasted = pasteEntities(loaded, clip);
    ASSERT_TRUE(pasted);
    ASSERT_EQ(pasted->size(), 2u);
    Entity newCar = (*pasted)[0].name() == "Car" ? (*pasted)[0] : (*pasted)[1];
    Entity newCam = (*pasted)[0].name() == "Camera" ? (*pasted)[0] : (*pasted)[1];
    const auto& entry = newCam.get<UnknownComponents>().entries.front().second;
    EXPECT_EQ(entry.find("haunts")->getUuid(), newCar.uuid());
    EXPECT_NE(newCar.uuid(), carId);
}

TEST_F(SceneSerialization, CopyPasteRemapsReferencesAndKeepsHierarchy) {
    World world;
    buildScene(world);
    Entity car = world.findByName("Car");
    Entity cam = world.findByName("Camera");
    Entity wheel = car.children()[2];
    // Selecting a child together with its parent copies the subtree once.
    Entity selection[] = {wheel, car, cam};
    const auto clip = copyEntities(world, selection);
    Entity parent = world.create("Group");
    auto pasted = pasteEntities(world, clip, parent);
    ASSERT_TRUE(pasted) << pasted.error().message;
    ASSERT_EQ(pasted->size(), 2u);
    EXPECT_EQ(parent.childCount(), 2u);
    Entity newCam = (*pasted)[0].name() == "Camera" ? (*pasted)[0] : (*pasted)[1];
    Entity newCar = (*pasted)[0].name() == "Car" ? (*pasted)[0] : (*pasted)[1];
    EXPECT_EQ(newCar.childCount(), 4u);
    EXPECT_EQ(newCar.children()[2].name(), "Wheel2");
    EXPECT_NE(newCar.uuid(), car.uuid());
    const auto& f = newCam.get<FollowerComponent>();
    EXPECT_EQ(f.target, newCar.ref()) << "reference inside the copied set is remapped";
    EXPECT_EQ(world.resolve(f.waypoints[0]).name(), "Sun") << "reference outside the set is kept";
    EXPECT_EQ(world.entityCount(), 8u + 1u + 6u);
}

TEST_F(SceneSerialization, FiltersForSaveGames) {
    World world;
    buildScene(world);
    SceneSerializeOptions opts;
    opts.entityFilter = [](Entity e) { return e.has<SaveGameComponent>(); };
    const auto doc = serializeWorld(world, opts);
    const auto* entities = doc.root.find("entities");
    ASSERT_EQ(entities->size(), 1u) << "only the car (children are skipped with their parent)";
    EXPECT_EQ(entities->items()[0].find("name")->getString(), "Car");
}

TEST_F(SceneSerialization, PrefabInstantiateOverridesApplyRevert) {
    World world;
    Entity lamp = world.create("Lamp");
    lamp.add<MeshRendererComponent>().mesh = Uuid::fromName("lamp.mesh");
    Entity bulb = world.create("Bulb", lamp);
    bulb.setPosition({0, 2, 0});
    auto& light = bulb.add<LightComponent>();
    light.intensity = 800.0f;
    bulb.add<FollowerComponent>().target = lamp.ref();

    serial::Document prefab = createPrefab(world, lamp);
    EXPECT_EQ(prefab.kind, "prefab");
    const Uuid pid = prefabId(prefab);
    EXPECT_TRUE(lamp.get<PrefabInstanceComponent>().isRoot);
    EXPECT_EQ(bulb.get<PrefabInstanceComponent>().prefab, pid);

    // Prefab files round trip through the archive.
    auto reloaded = serial::decodeBinary(serial::encodeBinary(prefab));
    ASSERT_TRUE(reloaded);
    prefab = *reloaded;

    auto inst = instantiatePrefab(world, prefab);
    ASSERT_TRUE(inst) << inst.error().message;
    Entity iRoot = *inst;
    iRoot.setPosition({10, 0, 0});
    Entity iBulb = iRoot.children().at(0);
    EXPECT_NE(iBulb.uuid(), bulb.uuid());
    EXPECT_EQ(iBulb.get<FollowerComponent>().target, iRoot.ref()) << "internal refs point into the instance";
    EXPECT_EQ(prefabInstanceRoot(iBulb), iRoot);
    EXPECT_TRUE(detectOverrides(iRoot, prefab).empty()) << "root transform is instance-specific";
    EXPECT_TRUE(detectOverrides(iBulb, prefab).empty());

    // Override a field on the instance.
    iBulb.get<LightComponent>().intensity = 1500.0f;
    const auto detected = detectOverrides(iBulb, prefab);
    ASSERT_EQ(detected, (std::vector<std::string>{"Light.intensity"}));
    EXPECT_EQ(recordDetectedOverrides(iRoot, prefab), 1u);
    EXPECT_TRUE(isOverridden(iBulb, "Light.intensity"));

    // The prefab changes: colour and intensity. The override wins for intensity, colour propagates.
    World editWorld;
    auto source = instantiatePrefab(editWorld, prefab);
    ASSERT_TRUE(source);
    Entity sBulb = source->children()[0];
    sBulb.get<LightComponent>().intensity = 400.0f;
    sBulb.get<LightComponent>().color = {1, 0, 0};
    world.create("Extra"); // unrelated
    Entity added = editWorld.create("Shade", *source);
    added.add<MeshRendererComponent>().mesh = Uuid::fromName("shade.mesh");
    serial::Document prefabV2 = applyInstanceToPrefab(editWorld, *source, prefab);
    EXPECT_EQ(prefabId(prefabV2), pid);

    EXPECT_EQ(updatePrefabInstances(world, prefabV2), 2u) << "the original and the instance";
    EXPECT_EQ(iBulb.get<LightComponent>().intensity, 1500.0f);
    EXPECT_EQ(iBulb.get<LightComponent>().color, glm::vec3(1, 0, 0));
    EXPECT_EQ(iRoot.get<TransformComponent>().position, glm::vec3(10, 0, 0));
    EXPECT_EQ(iRoot.childCount(), 2u) << "entity added to the prefab appears in instances";
    EXPECT_EQ(iRoot.children()[1].name(), "Shade");
    EXPECT_EQ(lamp.children()[0].get<LightComponent>().intensity, 400.0f) << "non-overridden instance follows";

    // Revert the override.
    EXPECT_TRUE(revertOverride(iBulb, "Light.intensity", prefabV2));
    EXPECT_EQ(iBulb.get<LightComponent>().intensity, 400.0f);
    EXPECT_FALSE(isOverridden(iBulb, "Light.intensity"));

    // Component added on the instance, recorded and reverted.
    iBulb.add<TagComponent>().tags = {"x"};
    recordOverride(iBulb, "Tags");
    updatePrefabInstances(world, prefabV2);
    EXPECT_TRUE(iBulb.has<TagComponent>()) << "added component survives updates when recorded";
    revertAllOverrides(iRoot, prefabV2);
    EXPECT_FALSE(iBulb.has<TagComponent>());

    // Entity removed from the prefab disappears from instances.
    Entity shadeInEdit = source->children()[1];
    editWorld.destroyImmediate(shadeInEdit);
    serial::Document prefabV3 = applyInstanceToPrefab(editWorld, *source, prefabV2);
    updatePrefabInstances(world, prefabV3);
    EXPECT_EQ(iRoot.childCount(), 1u);
}

TEST_F(SceneSerialization, PlayInEditorCloneDiscard) {
    World edit;
    buildScene(edit);
    const auto before = serial::encodeBinary(serializeWorld(edit));
    {
        auto play = edit.clone();
        play->findByName("Car").setPosition({100, 0, 0});
        play->destroy(play->findByName("Sun"));
        play->flushDestroyed();
        play->updateTransforms();
        EXPECT_EQ(play->entityCount(), edit.entityCount() - 1);
    }
    EXPECT_EQ(serial::encodeBinary(serializeWorld(edit)), before);
}

// Writes a sample scene for the oxdump CLI tests (path from OX_SAMPLE_SCENE_OUT, else a temp file).
TEST_F(SceneSerialization, WriteSampleScene) {
    World world;
    buildScene(world);
    const char* out = std::getenv("OX_SAMPLE_SCENE_OUT");
    const std::filesystem::path path = out ? std::filesystem::path(out) : tempDir() / "sample.oxscene";
    ASSERT_TRUE(saveScene(world, path));
    auto info = serial::inspectBinary(*serial::readFileBytes(path));
    ASSERT_TRUE(info);
    EXPECT_EQ(info->kind, "scene");
    for (const auto& c : info->chunks) EXPECT_TRUE(c.crcValid);
    if (!out) std::filesystem::remove_all(path.parent_path());
}
