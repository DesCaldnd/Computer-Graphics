// Глава 32: Lua API сущностей — ScriptComponent, свойства, прокси компонентов, события, scene/physics
// (docs/guide/32-gameplay-components.md). Скрипты: scripts/pickup.lua, scripts/player.lua.
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace ox;
using namespace ox::gameplay;

namespace {

const std::filesystem::path kScripts = std::filesystem::path(OX_GUIDE_DIR) / "scripts";

GameplayConfig quietConfig() {
    GameplayConfig c;
    c.physicsWorld.workerThreads = 0;
    c.physicsWorld.maxBodies = 1024;
    c.physicsWorld.maxBodyPairs = 1024;
    c.physicsWorld.maxContactConstraints = 1024;
    c.physicsWorld.tempAllocatorBytes = 8u << 20;
    c.scriptVM.hotReloadInterval = 0.0;
    return c;
}

// Мир + сервисы + планировщик. Порядок членов важен: планировщик отсоединяется первым.
struct Game {
    GameplayAssetRegistry providers;
    World world;
    Services services;
    SystemScheduler scheduler;

    Game() {
        registerSceneTypes();
        registerGameplayTypes();
        // Имена скриптов → файлы (в Engine это делает база ассетов: "Scripts/pickup.lua", "pickup", UUID).
        providers.addScriptFile("pickup", (kScripts / "pickup.lua").string());
        providers.addScriptFile("player", (kScripts / "player.lua").string());
        providers.registerIn(services);
        addGameplaySystems(scheduler, services, quietConfig());
    }
    ~Game() { scheduler.detach(); }

    void run(int frames) {
        for (int i = 0; i < frames; ++i) scheduler.tick(world, services, 1.0 / 60.0);
    }
    template <class T>
    T selfValue(Entity e, const char* key, T fallback) {
        sol::table self = services.get<ScriptRuntime>().self(e);   // Lua-таблица self экземпляра
        if (!self.valid()) return fallback;
        sol::object v = self[key];
        return v.valid() && v.is<T>() ? v.as<T>() : fallback;
    }
};

} // namespace

TEST(GuideGameplayLua, PickupScoresAndDestroysItself) {
    Game g;
    Entity ground = g.world.create("Ground");
    ground.setPosition({0.f, -0.5f, 0.f});
    ground.add<ColliderComponent>().halfExtents = {10.f, 0.5f, 10.f};

    Entity pickup = g.world.create("Pickup");
    pickup.setPosition({0.f, 1.f, 0.f});
    pickup.add<ColliderComponent>();   // форму настроит скрипт
    auto& script = pickup.add<ScriptComponent>();
    script.script = "pickup";
    script.properties["spinSpeed"] = ScriptPropertyValue::makeNumber(180.0);   // переопределение, как в инспекторе
    script.properties["score"] = ScriptPropertyValue{script::ScriptPropertyType::Int, 25.0};

    Entity player = g.world.create("Player");
    player.setPosition({0.f, 4.f, 0.f});
    player.add<TagComponent>().tags = {"player"};
    auto& rb = player.add<RigidBodyComponent>();
    rb.mass = 70.f;
    auto& col = player.add<ColliderComponent>();
    col.type = ColliderType::Sphere;
    col.radius = 0.3f;
    player.add<ScriptComponent>().script = "player";

    g.scheduler.attach(g.world, g.services);
    g.scheduler.setPlaying(true);   // экземпляры скриптов существуют только в play mode
    g.run(1);
    EXPECT_DOUBLE_EQ(g.selfValue<double>(pickup, "spinSpeed", 0.0), 180.0);
    EXPECT_EQ(pickup.get<TriggerComponent>().requiredTag, "player");                // e:add("Trigger", {...})
    EXPECT_EQ(pickup.get<ColliderComponent>().type, ColliderType::Sphere);           // col.type = "Sphere"
    EXPECT_DOUBLE_EQ(g.selfValue<double>(player, "massAtStart", 0.0), 70.0);
    EXPECT_FLOAT_EQ(player.get<RigidBodyComponent>().mass, 80.f);                   // rb.mass = 80
    EXPECT_EQ(g.selfValue<std::string>(player, "motion", ""), "Dynamic");

    g.run(120);   // игрок падает сквозь бонус
    EXPECT_EQ(g.selfValue<int>(player, "score", 0), 25);
    EXPECT_FALSE(g.world.findByName("Pickup").valid());   // scene.destroy(self.entity)
    EXPECT_LT(player.worldPosition().y, 0.5f);           // лежит на полу
}

TEST(GuideGameplayLua, InlineScriptSceneAndPhysicsTables) {
    Game g;
    g.providers.addScript("probe", R"(
        function onStart(self)
            local e = self.entity
            e.name = "Probe"
            e.transform.worldPosition = vec3(0, 3, 0)
            local spawned = scene.create("Marker")
            spawned:add("Tags", { tags = { "marker" } })
            spawned.transform.position = vec3(2, 0, 0)
            spawned:setParent(e)
            self.children = #e:children()
            self.found = e:findChild("Marker") ~= nil
            local hit = physics.raycast(vec3(0, 10, 0), vec3(0, -1, 0), 50)
            self.hitName = hit and hit.entity.name or "none"
            self.hitY = hit and hit.point.y or -1
            self.markerHasTag = spawned:hasTag("marker")
        end
    )");
    Entity ground = g.world.create("Ground");
    ground.setPosition({0.f, -0.5f, 0.f});
    ground.add<ColliderComponent>().halfExtents = {10.f, 0.5f, 10.f};
    Entity probe = g.world.create("Unnamed");
    probe.add<ScriptComponent>().script = "probe";

    g.scheduler.attach(g.world, g.services);
    g.scheduler.setPlaying(true);
    g.run(2);
    EXPECT_EQ(probe.name(), "Probe");
    EXPECT_NEAR(probe.worldPosition().y, 3.f, 1e-4f);
    EXPECT_EQ(g.selfValue<int>(probe, "children", 0), 1);
    EXPECT_TRUE(g.selfValue<bool>(probe, "found", false));
    EXPECT_TRUE(g.selfValue<bool>(probe, "markerHasTag", false));
    EXPECT_EQ(g.selfValue<std::string>(probe, "hitName", ""), "Ground");   // луч попал в пол
    EXPECT_NEAR(g.selfValue<double>(probe, "hitY", -1.0), 0.0, 1e-3);
    Entity marker = g.world.findByName("Marker");
    ASSERT_TRUE(marker.valid());
    EXPECT_NEAR(marker.worldPosition().x, 2.f, 1e-4f);   // локальная позиция у родителя
}
