#include "gameplay_test_utils.hpp"

#include <oxwald/scene/prefab.hpp>
#include <oxwald/script/script_events.hpp>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

template <class T>
T selfValue(GameplayHarness& h, Entity e, const char* key, T fallback) {
    sol::table self = h.runtime<ScriptRuntime>().self(e);
    if (!self.valid()) return fallback;
    sol::object v = self[key];
    return v.valid() && v.is<T>() ? v.as<T>() : fallback;
}

Entity scripted(GameplayHarness& h, std::string_view name, const std::string& script) {
    Entity e = h.world.create(name);
    e.add<ScriptComponent>().script = script;
    return e;
}

} // namespace

TEST(GameplayScript, TriggerEventsReachLua) {
    GameplayHarness h;
    h.assets.addScript("zone", R"(
        function onCreate(self) self.enters = 0; self.exits = 0 end
        function onTriggerEnter(self, other) self.enters = self.enters + 1; self.lastName = other.name end
        function onTriggerExit(self, other) self.exits = self.exits + 1 end
    )");
    h.assets.addScript("ball", R"(
        function onCreate(self) self.zoneHits = 0 end
        function onTriggerEnter(self, other) self.zoneHits = self.zoneHits + 1; self.zone = other.name end
    )");
    Entity zone = scripted(h, "Zone", "zone");
    zone.setPosition({0.f, 2.f, 0.f});
    zone.add<ColliderComponent>().halfExtents = {2.f, 0.5f, 2.f};
    zone.add<TriggerComponent>();
    Entity ball = h.box("Ball", {0.f, 6.f, 0.f}, glm::vec3(0.25f));
    ball.add<ScriptComponent>().script = "ball";
    h.start();
    ASSERT_TRUE(h.runUntil([&] { return selfValue<int>(h, zone, "exits", 0) >= 1; }, 4.0));
    EXPECT_EQ(selfValue<int>(h, zone, "enters", 0), 1);
    EXPECT_EQ(selfValue<std::string>(h, zone, "lastName", ""), "Ball");
    EXPECT_EQ(selfValue<int>(h, ball, "zoneHits", 0), 1);
    EXPECT_EQ(selfValue<std::string>(h, ball, "zone", ""), "Zone");
    EXPECT_EQ(zone.get<TriggerComponent>().overlapCount, 0u);
}

TEST(GameplayScript, LuaMovesEntityThroughTransformHelpers) {
    GameplayHarness h;
    h.assets.addScript("mover", R"(
        function onStart(self)
            self.entity.transform.position = vec3(0, 1, 0)
        end
        function onUpdate(self, dt)
            self.entity.transform:translate(vec3(2, 0, 0) * dt)
            self.forwardZ = self.entity.transform.forward.z
        end
    )");
    h.assets.addScript("looker", R"(
        function onUpdate(self, dt)
            local target = scene.find("Mover")
            self.entity.transform:lookAt(target)
            self.entity.transform:rotate(vec3(0, 1, 0), 0)   -- no-op rotation, exercises the overload
        end
    )");
    Entity mover = scripted(h, "Mover", "mover");
    Entity looker = scripted(h, "Looker", "looker");
    looker.setPosition({0.f, 1.f, 10.f});
    h.start();
    h.run(1.0);
    EXPECT_NEAR(mover.worldPosition().x, 2.f, 0.05f);
    EXPECT_NEAR(mover.worldPosition().y, 1.f, 1e-4f);
    EXPECT_NEAR(selfValue<f64>(h, mover, "forwardZ", 0.0), -1.0, 1e-5);
    // Looker faces the mover (forward = -Z rotated towards it).
    const glm::vec3 fwd = looker.worldRotation() * glm::vec3(0, 0, -1);
    const glm::vec3 to = glm::normalize(mover.worldPosition() - looker.worldPosition());
    EXPECT_GT(glm::dot(fwd, to), 0.999f);
}

TEST(GameplayScript, LuaReadsAndWritesReflectedComponentFields) {
    GameplayHarness h;
    h.assets.addScript("fields", R"(
        function onCreate(self)
            local e = self.entity
            local rb = e:get("RigidBody")
            self.mass = rb.mass
            self.motion = rb.motionType
            rb.mass = 42
            rb.motionType = "Kinematic"
            local col = e:get("Collider")
            col.halfExtents = vec3(1, 2, 3)
            self.hy = col.halfExtents.y
            col.children = { { type = "Sphere", radius = 2, position = vec3(1, 0, 0) } }
            self.childRadius = col.children[1].radius
            col.children[1].position = vec3(0, 5, 0)
            self.childCount = #col.children
            local t = e:get("Transform")
            t.position = vec3(0, 7, 0)
            t.rotation = quat.angleAxis(math.pi / 2, vec3(0, 1, 0))
            self.hasRb = e:has("RigidBody")
            self.hasSpline = e:has("SplineFollower")
            local f = e:add("SplineFollower", { speed = 3, loopMode = "PingPong" })
            f.spline = scene.find("Path")
            self.nameField = e:get("Name").name
            self.missing = e:get("Animator") == nil
            local ok, err = pcall(function() rb.doesNotExist = 1 end)
            self.badFieldFails = not ok
            scene.find("Receiver"):sendEvent("ping", { value = 5 })
        end
    )");
    h.assets.addScript("receiver", R"(
        function onEvent(self, name, payload) if name == "ping" then self.got = payload.value end end
    )");
    Entity path = h.world.create("Path");
    path.add<SplineComponent>();
    Entity receiver = scripted(h, "Receiver", "receiver");
    Entity e = h.box("Subject", {0.f, 0.f, 0.f}, glm::vec3(0.5f), physics::MotionType::Kinematic);
    e.get<RigidBodyComponent>().mass = 5.f;
    e.get<RigidBodyComponent>().motionType = physics::MotionType::Dynamic;
    e.add<ScriptComponent>().script = "fields";
    h.start();
    h.tick();

    EXPECT_DOUBLE_EQ(selfValue<f64>(h, e, "mass", 0.0), 5.0);
    EXPECT_EQ(selfValue<std::string>(h, e, "motion", ""), "Dynamic");
    EXPECT_FLOAT_EQ(e.get<RigidBodyComponent>().mass, 42.f);
    EXPECT_EQ(e.get<RigidBodyComponent>().motionType, physics::MotionType::Kinematic);
    EXPECT_EQ(e.get<ColliderComponent>().halfExtents, glm::vec3(1, 2, 3));
    EXPECT_DOUBLE_EQ(selfValue<f64>(h, e, "hy", 0.0), 2.0);
    ASSERT_EQ(e.get<ColliderComponent>().children.size(), 1u);
    EXPECT_EQ(e.get<ColliderComponent>().children[0].type, ColliderType::Sphere);
    EXPECT_EQ(e.get<ColliderComponent>().children[0].position, glm::vec3(0, 5, 0));
    EXPECT_DOUBLE_EQ(selfValue<f64>(h, e, "childRadius", 0.0), 2.0);
    EXPECT_EQ(selfValue<int>(h, e, "childCount", 0), 1);
    EXPECT_NEAR(e.worldPosition().y, 7.f, 1e-4f);
    EXPECT_NEAR(std::abs(glm::dot(e.worldRotation(), glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 1, 0)))), 1.f, 1e-5f);
    EXPECT_TRUE(selfValue<bool>(h, e, "hasRb", false));
    EXPECT_FALSE(selfValue<bool>(h, e, "hasSpline", true));
    ASSERT_TRUE(e.has<SplineFollowerComponent>());
    EXPECT_FLOAT_EQ(e.get<SplineFollowerComponent>().speed, 3.f);
    EXPECT_EQ(e.get<SplineFollowerComponent>().loopMode, spline::LoopMode::PingPong);
    EXPECT_EQ(e.get<SplineFollowerComponent>().spline, path.ref());
    EXPECT_EQ(selfValue<std::string>(h, e, "nameField", ""), "Subject");
    EXPECT_TRUE(selfValue<bool>(h, e, "missing", false));
    EXPECT_TRUE(selfValue<bool>(h, e, "badFieldFails", false));
    EXPECT_EQ(selfValue<int>(h, receiver, "got", 0), 5);
    // The rigid body was recreated with the new settings.
    auto& rt = h.runtime<PhysicsRuntime>();
    h.tick();
    EXPECT_EQ(rt.physicsWorld().getMotionType(rt.bodyOf(e)), physics::MotionType::Kinematic);
}

TEST(GameplayScript, PropertyOverridesFromComponent) {
    GameplayHarness h;
    h.assets.addScript("props", R"(
        properties = {
            speed = { type = "float", default = 1, min = 0, max = 10 },
            label = { type = "string", default = "none" },
            offset = { type = "vec3", default = vec3(0, 0, 0) },
            godMode = false,
        }
        function onCreate(self) self.seenSpeed = self.speed end
        function onUpdate(self, dt) self.currentSpeed = self.speed end
    )");
    Entity e = h.world.create("Player");
    auto& sc = e.add<ScriptComponent>();
    sc.script = "props";
    sc.properties["speed"] = ScriptPropertyValue::makeNumber(7.0);
    sc.properties["label"] = ScriptPropertyValue{script::ScriptPropertyType::String, 0.0, false, "hero"};
    sc.properties["offset"] = ScriptPropertyValue{script::ScriptPropertyType::Vec3, 0.0, false, {}, {1.f, 2.f, 3.f, 0.f}};
    sc.properties["godMode"] = ScriptPropertyValue{script::ScriptPropertyType::Bool, 0.0, true};
    h.start();
    h.tick();
    EXPECT_DOUBLE_EQ(selfValue<f64>(h, e, "seenSpeed", 0.0), 7.0);
    EXPECT_EQ(selfValue<std::string>(h, e, "label", ""), "hero");
    EXPECT_TRUE(selfValue<bool>(h, e, "godMode", false));
    sol::table self = h.runtime<ScriptRuntime>().self(e);
    EXPECT_EQ(self["offset"].get<glm::vec3>(), glm::vec3(1, 2, 3));

    // Editing the overrides at runtime (inspector / scripts) applies them to the live instance (clamped).
    e.patch<ScriptComponent>([](ScriptComponent& c) { c.properties["speed"] = ScriptPropertyValue::makeNumber(50.0); });
    h.tick();
    EXPECT_DOUBLE_EQ(selfValue<f64>(h, e, "currentSpeed", 0.0), 10.0);
}

TEST(GameplayScript, LifecycleAndDestroyFromLua) {
    GameplayHarness h;
    h.assets.addScript("life", R"(
        function onCreate(self) self.created = true; self.fixed = 0; self.updates = 0 end
        function onStart(self) self.started = true end
        function onFixedUpdate(self, dt) self.fixed = self.fixed + 1 end
        function onUpdate(self, dt)
            self.updates = self.updates + 1
            if self.updates == 3 then self.entity:destroy() end
        end
        function onDestroy(self) events.publish("destroyed", { name = self.entity.name, x = self.entity.transform.position.x }) end
    )");
    Entity e = scripted(h, "Doomed", "life");
    e.setPosition({4.f, 0.f, 0.f});
    std::string destroyedName;
    f64 destroyedX = 0.0;
    h.start();
    h.runtime<ScriptRuntime>().vm().events().subscribe("destroyed", [&](std::string_view, const sol::object& p) {
        destroyedName = p.as<sol::table>()["name"].get<std::string>();
        destroyedX = p.as<sol::table>()["x"].get<f64>();
    });
    h.tick();
    EXPECT_TRUE(selfValue<bool>(h, e, "created", false));
    EXPECT_TRUE(selfValue<bool>(h, e, "started", false));
    EXPECT_GE(selfValue<int>(h, e, "fixed", 0), 1);
    h.run(0.1);
    EXPECT_FALSE(h.world.valid(e.handle()));
    EXPECT_EQ(destroyedName, "Doomed") << "onDestroy ran while the entity still existed";
    EXPECT_DOUBLE_EQ(destroyedX, 4.0);
}

TEST(GameplayScript, SpawnPrefabFromLuaAndApiTables) {
    GameplayHarness h;
    {
        World proto;
        Entity root = proto.create("Coin");
        root.add<ColliderComponent>().type = ColliderType::Sphere;
        root.add<RigidBodyComponent>();
        h.assets.addPrefab("Coin", createPrefab(proto, root, {.linkSource = false}));
    }
    h.assets.addScript("spawner", R"(
        function onStart(self)
            self.coin = scene.spawn("Coin", vec3(0, 3, 0))
            local hit = physics.raycast(vec3(0, 10, 0), vec3(0, -1, 0), 50)
            self.hitName = hit and hit.entity and hit.entity.name
            self.coins = #scene.findAll("RigidBody")
        end
        function onUpdate(self, dt)
            if self.coin and self.coin:isValid() then self.coinY = self.coin.transform.worldPosition.y end
        end
    )");
    h.ground();
    scripted(h, "Spawner", "spawner");
    h.start();
    h.run(1.5);
    Entity spawner = h.world.findByName("Spawner");
    EXPECT_EQ(selfValue<int>(h, spawner, "coins", 0), 1);
    EXPECT_EQ(selfValue<std::string>(h, spawner, "hitName", ""), "Ground") << "body of the spawned coin not created yet";
    EXPECT_NEAR(selfValue<f64>(h, spawner, "coinY", 99.0), 0.5, 0.05);
}
