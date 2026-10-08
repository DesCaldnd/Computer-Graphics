// Глава 32: геймплейные компоненты — подключение систем, edit/play mode, play-in-editor, значения по умолчанию
// (docs/guide/32-gameplay-components.md).
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace ox;
using namespace ox::gameplay;

namespace {

// Детерминированная и лёгкая конфигурация для тестов (в игре всё это делает Engine).
GameplayConfig quietConfig() {
    GameplayConfig c;
    c.physicsWorld.workerThreads = 0;
    c.physicsWorld.maxBodies = 1024;
    c.physicsWorld.maxBodyPairs = 1024;
    c.physicsWorld.maxContactConstraints = 1024;
    c.physicsWorld.tempAllocatorBytes = 8u << 20;
    c.scriptVM.hotReloadInterval = 0.0;
#if defined(OX_GAMEPLAY_HAS_WORLD)
    c.worldSystems.streamingExecutor = WorldSystemsConfig::Executor::Inline;
#endif
    return c;
}

// Пол 40×1×40 м (статический: Collider без RigidBody) и ящик, который падает на него.
void buildScene(World& world) {
    Entity ground = world.create("Ground");
    ground.setPosition({0.f, -0.5f, 0.f});
    ground.add<ColliderComponent>().halfExtents = {20.f, 0.5f, 20.f};

    Entity crate = world.create("Crate");
    crate.setPosition({0.f, 5.f, 0.f});
    auto& rb = crate.add<RigidBodyComponent>();
    rb.motionType = physics::MotionType::Dynamic;
    rb.mass = 20.f;
    auto& col = crate.add<ColliderComponent>();
    col.type = ColliderType::Box;
    col.halfExtents = glm::vec3(0.5f);
}

} // namespace

TEST(GuideGameplaySystems, EditModeDoesNotSimulatePlayModeDoes) {
    registerSceneTypes();
    registerGameplayTypes();   // рефлексия + ComponentRegistry (идемпотентно)

    World world;
    buildScene(world);
    Services services;
    GameplayAssetRegistry providers;   // провайдеры в памяти; в Engine их даёт база ассетов
    providers.registerIn(services);
    SystemScheduler scheduler;
    addGameplaySystems(scheduler, services, quietConfig());   // создаёт PhysicsWorld, ScriptVM, рантаймы и системы
    scheduler.attach(world, services);

    Entity crate = world.findByName("Crate");
    // Edit mode (по умолчанию): физика, скрипты, ИИ не работают.
    for (int i = 0; i < 30; ++i) scheduler.tick(world, services, 1.0 / 60.0);
    EXPECT_FLOAT_EQ(crate.worldPosition().y, 5.f);
    EXPECT_EQ(crate.get<RigidBodyComponent>().bodyId, physics::BodyHandle::kInvalid);   // тел ещё нет

    // Play mode: на первом кадре Gameplay.Lifecycle создаёт тела, затем идёт симуляция.
    scheduler.setPlaying(true);
    for (int i = 0; i < 180; ++i) scheduler.tick(world, services, 1.0 / 60.0);
    EXPECT_NEAR(crate.worldPosition().y, 0.5f, 0.05f);   // лежит на полу
    EXPECT_NE(services.get<PhysicsRuntime>().bodyOf(crate).id, physics::BodyHandle::kInvalid);

    // Системы находятся по имени: например, временно выключить ИИ.
    ASSERT_NE(scheduler.find(systems::kPhysicsStep), nullptr);
    scheduler.setEnabled(systems::kBehaviorTrees, false);
    EXPECT_FALSE(scheduler.isEnabled(systems::kBehaviorTrees));
    scheduler.detach();
}

TEST(GuideGameplaySystems, PlayInEditorSimulatesAClone) {
    registerSceneTypes();
    registerGameplayTypes();
    World editWorld;
    buildScene(editWorld);

    // Play-in-editor: клон мира + планировщик на клоне. Редактируемый мир не меняется.
    std::unique_ptr<World> playWorld = editWorld.clone();
    Services services;
    GameplayAssetRegistry providers;
    providers.registerIn(services);
    SystemScheduler scheduler;
    addGameplaySystems(scheduler, services, quietConfig());
    scheduler.attach(*playWorld, services);
    scheduler.setPlaying(true);
    for (int i = 0; i < 60; ++i) scheduler.tick(*playWorld, services, 1.0 / 60.0);
    scheduler.detach();

    EXPECT_LT(playWorld->findByName("Crate").worldPosition().y, 4.f);
    EXPECT_FLOAT_EQ(editWorld.findByName("Crate").worldPosition().y, 5.f);
    // В компонентах нет «живых» хэндлов: bodyId в клоне не сериализуется и не копируется в edit-мир.
    EXPECT_EQ(editWorld.findByName("Crate").get<RigidBodyComponent>().bodyId, physics::BodyHandle::kInvalid);
}

TEST(GuideGameplaySystems, CharacterControllerAndTrigger) {
    registerSceneTypes();
    registerGameplayTypes();
    World world;
    buildScene(world);
    world.destroyImmediate(world.findByName("Crate"));

    Entity hero = world.create("Hero");
    hero.setPosition({0.f, 0.f, 0.f});   // позиция = ступни
    auto& cc = hero.add<CharacterControllerComponent>();
    cc.height = 1.8f;
    cc.radius = 0.3f;

    // Зона-триггер (сенсор) и два падающих сквозь неё шара: только один с тегом "loot".
    Entity zone = world.create("Zone");
    zone.setPosition({5.f, 2.f, 5.f});
    zone.add<ColliderComponent>().halfExtents = {2.f, 0.5f, 2.f};
    auto& trigger = zone.add<TriggerComponent>();
    trigger.requiredTag = "loot";   // события только от сущностей с этим тегом (TagComponent)
    trigger.once = true;            // после первого Enter триггер выключается
    for (const char* name : {"Coin", "Rock"}) {
        Entity ball = world.create(name);
        ball.setPosition({5.f, 6.f, 5.f});
        ball.add<RigidBodyComponent>();
        auto& col = ball.add<ColliderComponent>();
        col.type = ColliderType::Sphere;
        col.radius = 0.25f;
        if (std::string_view(name) == "Coin") ball.add<TagComponent>().tags = {"loot"};
    }

    Services services;
    GameplayAssetRegistry providers;
    providers.registerIn(services);
    SystemScheduler scheduler;
    addGameplaySystems(scheduler, services, quietConfig());
    std::vector<std::string> entered;
    ScopedConnection c = services.get<PhysicsRuntime>().onTrigger.connect([&](const TriggerEvent& e) {
        if (e.phase == TriggerPhase::Enter && e.trigger == zone) entered.push_back(e.other.name());
    });
    scheduler.attach(world, services);
    scheduler.setPlaying(true);
    for (int i = 0; i < 120; ++i) {
        hero.get<CharacterControllerComponent>().desiredVelocity = {3.f, 0.f, 0.f};   // ввод — каждый кадр
        scheduler.tick(world, services, 1.0 / 60.0);
    }
    scheduler.detach();
    EXPECT_GT(hero.worldPosition().x, 4.f);   // ~2 с × 3 м/с
    EXPECT_EQ(hero.get<CharacterControllerComponent>().groundState, physics::GroundState::OnGround);
    EXPECT_EQ(entered, std::vector<std::string>{"Coin"});
    EXPECT_TRUE(zone.get<TriggerComponent>().fired);
}

// Значения по умолчанию из справочных таблиц главы.
TEST(GuideGameplaySystems, DefaultsMatchTheReference) {
    const RigidBodyComponent rb;
    EXPECT_EQ(rb.motionType, physics::MotionType::Dynamic);
    EXPECT_FLOAT_EQ(rb.friction, 0.5f);
    EXPECT_FLOAT_EQ(rb.linearDamping, 0.05f);
    const ColliderComponent col;
    EXPECT_EQ(col.type, ColliderType::Box);
    EXPECT_FLOAT_EQ(col.density, 1000.f);
    const CharacterControllerComponent cc;
    EXPECT_FLOAT_EQ(cc.maxSlopeAngle, 45.f);
    EXPECT_FLOAT_EQ(cc.maxStepHeight, 0.35f);
    EXPECT_FLOAT_EQ(cc.jumpSpeed, 5.f);
    const AudioSourceComponent audio;
    EXPECT_EQ(audio.bus, "SFX");
    EXPECT_FLOAT_EQ(audio.maxDistance, 100.f);
    const NavAgentComponent agent;
    EXPECT_FLOAT_EQ(agent.maxSpeed, 3.5f);
    const NavMeshSurfaceComponent nav;
    EXPECT_FLOAT_EQ(nav.cellSize, 0.3f);
    EXPECT_FLOAT_EQ(nav.agentRadius, 0.5f);
    const SplineFollowerComponent follower;
    EXPECT_EQ(follower.loopMode, spline::LoopMode::Loop);
    EXPECT_EQ(follower.forwardAxis, glm::vec3(0.f, 0.f, -1.f));
    const NetworkTransformComponent nt;
    EXPECT_FLOAT_EQ(nt.positionResolution, 0.01f);
    EXPECT_EQ(nt.rotationBits, 10u);
    const PredictedCharacterComponent pc;
    EXPECT_EQ(pc.moveAction, "Move");
    EXPECT_FLOAT_EQ(pc.correctionTolerance, 0.05f);

    // Имена компонентов в сценах, Lua и инспекторе — без суффикса Component.
    registerSceneTypes();
    registerGameplayTypes();
    const ComponentRegistry& reg = ComponentRegistry::instance();
    for (const char* name : {"RigidBody", "Collider", "CharacterController", "Trigger", "Joint", "Animator", "SkinnedMesh",
                             "IK", "Spline", "SplineFollower", "AudioSource", "AudioListener", "NavMeshSurface", "NavAgent",
                             "NavObstacle", "BehaviorTree", "Perception", "Script", "NetworkIdentity", "NetworkTransform",
                             "PredictedCharacter"}) {
        EXPECT_NE(reg.find(name), nullptr) << name;
    }
    EXPECT_EQ(reg.find<RigidBodyComponent>()->category, "Physics");
}
