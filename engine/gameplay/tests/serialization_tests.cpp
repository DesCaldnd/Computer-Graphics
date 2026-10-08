#include "gameplay_test_utils.hpp"

#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <filesystem>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

void fillEverything(World& world, Entity e, Entity other) {
    auto& rb = e.add<RigidBodyComponent>();
    rb.motionType = physics::MotionType::Kinematic;
    rb.layer = "Props";
    rb.mass = 12.5f;
    rb.lockAxes = physics::lock::Plane2D;
    rb.bodyId = 77; // runtime: not serialized
    auto& col = e.add<ColliderComponent>();
    col.type = ColliderType::Compound;
    col.children.push_back({ColliderType::Sphere, glm::vec3(1.f), 0.75f, 0.2f, {}, {0.f, 1.f, 0.f}, glm::quat(1, 0, 0, 0)});
    col.children.push_back({ColliderType::ConvexHull, glm::vec3(1.f), 0.5f, 0.5f, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}});
    col.heights = {0.f, 1.f, 2.f, 3.f};
    col.sampleCount = 2;
    col.mesh = Uuid::generate();
    col.offsetRotation = glm::angleAxis(0.3f, glm::vec3(0, 1, 0));
    auto& cc = e.add<CharacterControllerComponent>();
    cc.height = 1.6f;
    cc.desiredVelocity = {1, 2, 3}; // runtime
    e.add<TriggerComponent>().requiredTag = "Player";
    auto& j = e.add<JointComponent>();
    j.type = physics::ConstraintType::Hinge;
    j.target = other.ref();
    j.motorMode = physics::MotorMode::Velocity;

    auto& an = e.add<AnimatorComponent>();
    an.skeleton = Uuid::generate();
    an.inlineController.parameters.push_back({"speed", anim::ParamType::Float, 0.5f});
    an.inlineController.states.push_back({"Idle", Uuid::generate(), 1.f, "", true});
    an.inlineController.transitions.push_back({"*", "Idle", "speed", anim::ConditionOp::Less, 0.1f});
    an.parameters["speed"] = 2.f;
    an.applyRootMotion = true;
    auto& sm = e.add<SkinnedMeshComponent>();
    sm.mesh = Uuid::generate();
    sm.materials = {Uuid::generate(), Uuid::generate()};
    sm.skinningMethod = anim::SkinningMethod::DualQuaternion;
    IKChainDesc chain;
    chain.rootJoint = "UpperArm";
    chain.target = other.ref();
    e.add<IKComponent>().chains.push_back(chain);

    auto& sp = e.add<SplineComponent>();
    sp.type = spline::SplineType::Bezier;
    sp.points.push_back({{1, 2, 3}, {-1, 0, 0}, {1, 0, 0}, spline::HandleMode::Mirrored, 0.1f, 1.f, glm::vec3(0, 1, 0)});
    sp.points.push_back({{4, 5, 6}});
    sp.markers.push_back({"gate", 0.5f});
    auto& fo = e.add<SplineFollowerComponent>();
    fo.spline = other.ref();
    fo.loopMode = spline::LoopMode::PingPong;
    fo.events.push_back({"horn", 3.f});
    fo.distance = 4.5f;

    auto& as = e.add<AudioSourceComponent>();
    as.clipPath = "sfx/hum.wav";
    as.attenuation = audio::AttenuationModel::Exponential;
    as.volume = 0.7f;
    e.add<AudioListenerComponent>().active = false;

    auto& nav = e.add<NavMeshSurfaceComponent>();
    nav.agentRadius = 0.3f;
    nav.bakedData = {1, 2, 3, 4, 5};
    auto& ag = e.add<NavAgentComponent>();
    ag.maxSpeed = 6.f;
    ag.destination = {1, 0, 1};
    ag.hasDestination = true;
    e.add<NavObstacleComponent>().shape = NavObstacleShape::Box;
    auto& bt = e.add<BehaviorTreeComponent>();
    bt.treeJson = R"({"root":{"type":"Wait","seconds":1}})";
    BlackboardEntry be;
    be.key = "friend";
    be.type = BlackboardEntryType::Entity;
    be.entityValue = other.ref();
    bt.blackboard.push_back(be);
    auto& pc = e.add<PerceptionComponent>();
    pc.team = 3;
    pc.sight.range = 33.f;
    pc.hearing.enabled = false;

    auto& sc = e.add<ScriptComponent>();
    sc.script = "scripts/door.lua";
    sc.properties["speed"] = ScriptPropertyValue::makeNumber(2.5);
    sc.properties["tint"] = ScriptPropertyValue{script::ScriptPropertyType::Color, 0.0, false, {}, {1, 0.5f, 0, 1}};
    auto& ni = e.add<NetworkIdentityComponent>();
    ni.netType = "Door";
    ni.relevancy = net::Relevancy::Distance;
    ni.netId = 99; // runtime
    e.add<NetworkTransformComponent>().predicted = true;
    (void)world;
}

std::vector<const ComponentInfo*> gameplayComponents() {
    std::vector<const ComponentInfo*> out;
    for (const ComponentInfo* info : ComponentRegistry::instance().all()) {
        const std::string& cat = info->category;
        if (cat == "Physics" || cat == "Animation" || cat == "Splines" || cat == "Audio" || cat == "AI" ||
            cat == "Scripting" || cat == "Networking") {
            out.push_back(info);
        }
    }
    return out;
}

} // namespace

TEST(GameplaySerialization, SceneRoundTripWithAllGameplayComponents) {
    registerGameplayTypes();
    const auto infos = gameplayComponents();
    EXPECT_EQ(infos.size(), 20u);
    World world;
    Entity other = world.create("Other");
    Entity e = world.create("Everything");
    fillEverything(world, e, other);

    const auto dir = std::filesystem::temp_directory_path() / "ox_gameplay_tests";
    std::filesystem::create_directories(dir);
    for (const char* name : {"all.oxscene", "all.oxscene.json"}) {
        const auto path = dir / name;
        ASSERT_TRUE(saveScene(world, path)) << name;
        World loaded;
        ASSERT_TRUE(loadScene(loaded, path)) << name;
        const Entity le = loaded.find(e.uuid());
        ASSERT_TRUE(le.valid());
        for (const ComponentInfo* info : infos) {
            ASSERT_TRUE(info->has(loaded, le.handle())) << info->name << " in " << name;
            EXPECT_EQ(info->serialize(world, e.handle()), info->serialize(loaded, le.handle())) << info->name << " in " << name;
        }
        // Runtime-only fields are not persisted.
        EXPECT_EQ(le.get<RigidBodyComponent>().bodyId, physics::BodyHandle::kInvalid);
        EXPECT_EQ(le.get<NetworkIdentityComponent>().netId, net::kInvalidNetId);
        EXPECT_EQ(le.get<CharacterControllerComponent>().desiredVelocity, glm::vec3(0.f));
        EXPECT_EQ(le.get<JointComponent>().target, other.ref());
        EXPECT_EQ(le.get<NavMeshSurfaceComponent>().bakedData, (std::vector<u8>{1, 2, 3, 4, 5}));
        EXPECT_EQ(le.get<SplineComponent>().points[0].up, std::optional<glm::vec3>(glm::vec3(0, 1, 0)));
        EXPECT_FLOAT_EQ(le.get<SplineFollowerComponent>().distance, 4.5f);
    }
    // Binary <-> JSON are lossless.
    auto bytes = serial::readFileBytes(dir / "all.oxscene");
    ASSERT_TRUE(bytes);
    auto json = serial::binaryToJson(*bytes);
    ASSERT_TRUE(json);
    auto back = serial::jsonToBinary(*json);
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, *bytes);
}

TEST(GameplayPrefab, PrefabWithGameplayComponentsInstantiatesAndSimulates) {
    GameplayHarness h;
    {
        World proto;
        Entity barrel = proto.create("Barrel");
        barrel.add<RigidBodyComponent>().mass = 10.f;
        auto& c = barrel.add<ColliderComponent>();
        c.type = ColliderType::Cylinder;
        c.radius = 0.5f;
        c.halfHeight = 0.5f;
        barrel.add<ScriptComponent>().script = "counter";
        Entity label = proto.create("Label", barrel);
        label.setPosition({0.f, 1.f, 0.f});
        h.assets.addPrefab("Barrel", createPrefab(proto, barrel, {.linkSource = false}));
    }
    h.assets.addScript("counter", R"(
        function onCreate(self) self.ticks = 0 end
        function onFixedUpdate(self, dt) self.ticks = self.ticks + 1 end
    )");
    h.ground();
    h.start();
    auto& scripts = h.runtime<ScriptRuntime>();
    const glm::vec3 left{-2.f, 4.f, 0.f}, right{2.f, 6.f, 0.f};
    Entity a = scripts.spawnPrefab("Barrel", &left);
    Entity b = scripts.spawnPrefab("Barrel", &right);
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    EXPECT_NE(a.uuid(), b.uuid());
    h.run(3.0);
    for (Entity e : {a, b}) {
        EXPECT_NEAR(e.worldPosition().y, 0.5f, 0.06f) << e.name();
        ASSERT_EQ(e.childCount(), 1u);
        EXPECT_NEAR(e.children()[0].worldPosition().y, 1.5f, 0.08f);
        sol::table self = scripts.self(e);
        ASSERT_TRUE(self.valid());
        EXPECT_GT(self["ticks"].get_or(0), 150);
    }
    EXPECT_NEAR(a.worldPosition().x, -2.f, 0.05f);
    EXPECT_NEAR(b.worldPosition().x, 2.f, 0.05f);
}
