#include "gameplay_test_utils.hpp"

#include <oxwald/core/debug_draw.hpp>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

TEST(GameplaySpline, FollowerMovesAlongSplineEntityAtConstantSpeed) {
    GameplayHarness h;
    Entity path = h.world.create("Path");
    path.setPosition({10.f, 0.f, 0.f});
    auto& sc = path.add<SplineComponent>();
    sc.type = spline::SplineType::CatmullRom;
    for (glm::vec3 p : {glm::vec3(0, 0, 0), glm::vec3(5, 0, -5), glm::vec3(10, 2, 0), glm::vec3(15, 0, 5), glm::vec3(20, 0, 0)}) {
        sc.points.push_back(SplinePoint{p});
    }
    sc.markers.push_back({"mid", 0.5f});
    Entity cart = h.world.create("Cart");
    auto& f = cart.add<SplineFollowerComponent>();
    f.spline = path.ref();
    f.speed = 3.f;
    f.loopMode = spline::LoopMode::Once;
    f.events.push_back({"start", 0.5f});
    std::vector<std::string> events;
    h.start();
    ScopedConnection c = h.runtime<SplineRuntime>().onEvent.connect([&](const SplineFollowerEvent& e) { events.push_back(e.name); });

    const f64 dt = 1.0 / 60.0;
    h.tick(dt);
    glm::vec3 prev = cart.worldPosition();
    std::vector<f32> steps;
    for (int i = 0; i < 119; ++i) {
        h.tick(dt);
        const glm::vec3 p = cart.worldPosition();
        steps.push_back(glm::distance(p, prev));
        prev = p;
    }
    // Constant world speed: every step covers speed*dt (chord of a gently curved arc).
    for (f32 s : steps) EXPECT_NEAR(s, 3.f * f32(dt), 2e-3f);
    const auto& fc = cart.get<SplineFollowerComponent>();
    EXPECT_NEAR(fc.distance, 3.f * 2.f, 1e-3f);
    const spline::Spline* s = h.runtime<SplineRuntime>().splineOf(path);
    ASSERT_NE(s, nullptr);
    const glm::vec3 expected = glm::vec3(10.f, 0.f, 0.f) + s->evaluateAtDistance(fc.distance).position;
    EXPECT_NEAR(glm::distance(cart.worldPosition(), expected), 0.f, 1e-3f);
    // Oriented along the path (-Z forward = tangent).
    const glm::vec3 fwd = cart.worldRotation() * glm::vec3(0, 0, -1);
    EXPECT_GT(glm::dot(fwd, s->evaluateAtDistance(fc.distance).tangent), 0.999f);
    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ(events[0], "start");
    EXPECT_EQ(events[1], "mid");
}

TEST(GameplayAnimation, AnimatorRootMotionMovesEntityAndFillsPalette) {
    GameplayHarness h;
    auto skel = std::make_shared<anim::Skeleton>();
    skel->addJoint("Root", anim::kNoJoint, {});
    skel->addJoint("Spine", 0, anim::Transform{{0.f, 1.f, 0.f}});
    skel->finalize();
    auto clip = std::make_shared<anim::AnimationClip>();
    clip->name = "Walk";
    clip->tracks.resize(2);
    clip->tracks[0].translation.times = {0.f, 1.f};
    clip->tracks[0].translation.values = {glm::vec3(0.f), glm::vec3(0.f, 0.f, -2.f)};
    clip->addEvent(0.5f, "footstep", 1.f);
    clip->computeDuration();
    anim::extractRootMotion(*clip, *skel, {.rootJoint = 0});
    const Uuid skelId = Uuid::generate(), clipId = Uuid::generate();
    h.assets.addSkeleton(skelId, skel);
    h.assets.addClip(clipId, clip);

    Entity hero = h.world.create("Hero");
    auto& a = hero.add<AnimatorComponent>();
    a.skeleton = skelId;
    a.inlineController.states.push_back({"Walk", clipId});
    a.applyRootMotion = true;
    hero.add<SkinnedMeshComponent>();
    int footsteps = 0;
    h.start();
    ScopedConnection c = h.runtime<AnimationRuntime>().onEvent.connect([&](const AnimationEvent& e) {
        if (e.name == "footstep" && e.entity == hero) ++footsteps;
    });
    h.run(1.0);
    EXPECT_NEAR(hero.worldPosition().z, -2.f, 0.1f);
    EXPECT_NEAR(hero.worldPosition().x, 0.f, 1e-4f);
    EXPECT_EQ(hero.get<AnimatorComponent>().currentState, "Walk");
    EXPECT_EQ(footsteps, 1);
    const auto& sm = hero.get<SkinnedMeshComponent>();
    ASSERT_EQ(sm.palette.size(), 2u);
    EXPECT_GT(sm.paletteVersion, 0u);
    // Root motion is extracted: the root joint stays in place, so the palette is identity-ish.
    EXPECT_NEAR(glm::length(glm::vec3(sm.palette[0][3])), 0.f, 1e-4f);
    // Rotated owner: root motion is in the owner's local space.
    hero.setPosition(glm::vec3(0.f));
    hero.setRotation(glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 1, 0)));
    h.run(0.5);
    EXPECT_NEAR(hero.worldPosition().x, -1.f, 0.1f);
}

namespace {

void buildCorridorLevel(GameplayHarness& h) {
    h.ground({12.f, 0.5f, 12.f});
    Entity wall = h.world.create("Wall");
    wall.setPosition({0.f, 1.f, 2.f});
    wall.add<ColliderComponent>().halfExtents = {0.5f, 1.f, 6.f};
    Entity surface = h.world.create("NavSurface");
    auto& s = surface.add<NavMeshSurfaceComponent>();
    s.agentRadius = 0.4f;
    s.cellSize = 0.2f;
}

} // namespace

TEST(GameplayAI, NavAgentReachesTargetOnBakedNavMesh) {
    GameplayHarness h;
    buildCorridorLevel(h);
    Entity agent = h.world.create("Agent");
    agent.setPosition({-5.f, 0.f, 2.f});
    agent.add<NavAgentComponent>().radius = 0.4f;
    h.start();
    const glm::vec3 target{5.f, 0.f, 2.f};
    EXPECT_TRUE(h.runtime<AIRuntime>().moveTo(agent, target));
    bool crossedWall = false;
    ASSERT_TRUE(h.runUntil([&] {
        const glm::vec3 p = agent.worldPosition();
        if (std::abs(p.x) < 0.5f && p.z > -4.f && p.z < 8.f) crossedWall = true; // wall: x=0, z in [-4, 8]
        return h.runtime<AIRuntime>().reached(agent);
    }, 15.0));
    EXPECT_LT(glm::distance(agent.worldPosition(), target), 0.6f);
    EXPECT_FALSE(crossedWall) << "path goes around the wall";
    const auto* surface = h.world.findByName("NavSurface").tryGet<NavMeshSurfaceComponent>();
    ASSERT_NE(surface, nullptr);
    EXPECT_TRUE(surface->baked);
    EXPECT_FALSE(surface->bakedData.empty()) << "baked data is stored for saving with the scene";
}

TEST(GameplayAI, BehaviorTreeMoveToThenWait) {
    GameplayHarness h;
    buildCorridorLevel(h);
    Entity agent = h.world.create("Guard");
    agent.setPosition({-5.f, 0.f, -6.f});
    agent.add<NavAgentComponent>().maxSpeed = 5.f;
    auto& bt = agent.add<BehaviorTreeComponent>();
    bt.treeJson = R"({ "root": { "type": "Sequence", "children": [
        { "type": "MoveTo", "key": "goal", "acceptance": 0.4 },
        { "type": "SetBlackboard", "key": "arrived", "value": true },
        { "type": "Wait", "seconds": 0.5 },
        { "type": "SetBlackboard", "key": "done", "value": true } ] } })";
    bt.restartOnFinish = false;
    BlackboardEntry goal;
    goal.key = "goal";
    goal.type = BlackboardEntryType::Vec3;
    goal.vec3Value = {4.f, 0.f, -6.f};
    bt.blackboard.push_back(goal);
    h.start();
    int framesAfterArrive = -1;
    ASSERT_TRUE(h.runUntil([&] {
        ai::Blackboard* bb = h.runtime<AIRuntime>().blackboard(agent);
        if (!bb) return false;
        if (bb->getOr("arrived", false) && framesAfterArrive < 0) framesAfterArrive = 0;
        else if (framesAfterArrive >= 0) ++framesAfterArrive;
        return bb->getOr("done", false);
    }, 15.0));
    const glm::vec3 d = agent.worldPosition() - glm::vec3(4.f, 0.f, -6.f);
    EXPECT_LT(glm::length(glm::vec2(d.x, d.z)), 0.6f);
    EXPECT_LT(std::abs(d.y), 0.5f) << "on the navmesh surface";
    EXPECT_GE(framesAfterArrive, 29) << "Wait(0.5) held the sequence";
    EXPECT_EQ(agent.get<BehaviorTreeComponent>().status, ai::BTStatus::Success);
}

TEST(GameplayAI, PerceptionSeesTargetAndRespectsWalls) {
    GameplayHarness h;
    h.ground();
    Entity guard = h.world.create("Guard");
    guard.setPosition({0.f, 0.f, 0.f}); // looks down -Z
    guard.add<PerceptionComponent>().team = 1;
    Entity player = h.world.create("Player");
    player.setPosition({0.f, 0.f, -8.f});
    auto& pp = player.add<PerceptionComponent>();
    pp.team = 2;
    pp.listener = false;
    h.start();
    h.run(0.2);
    EXPECT_EQ(guard.get<PerceptionComponent>().target, player.ref());
    EXPECT_TRUE(guard.get<PerceptionComponent>().targetVisible);

    // A wall between them blocks the line of sight.
    Entity wall = h.world.create("Wall");
    wall.setPosition({0.f, 1.5f, -4.f});
    wall.add<ColliderComponent>().halfExtents = {3.f, 1.5f, 0.2f};
    h.run(0.2);
    EXPECT_FALSE(guard.get<PerceptionComponent>().targetVisible);
}

TEST(GameplayAudio, SourcePositionFollowsTransform) {
    auto setup = [](Services& s) {
        auto& engine = s.emplace<audio::AudioEngine>();
        ASSERT_TRUE(engine.init({.offline = true}));
    };
    GameplayHarness h(testConfig(), setup);
    auto& engine = h.services.get<audio::AudioEngine>();
    const Uuid toneId = Uuid::generate();
    h.assets.addSound(toneId, engine.createSine(440.f, 0.5f));
    Entity listener = h.world.create("Camera");
    listener.add<AudioListenerComponent>();
    Entity radio = h.world.create("Radio");
    radio.setPosition({2.f, 0.f, 0.f});
    auto& src = radio.add<AudioSourceComponent>();
    src.clip = toneId;
    src.loop = true;
    src.minDistance = 1.f;
    src.maxDistance = 200.f;
    h.start();
    h.run(0.1);
    auto& rt = h.runtime<AudioRuntime>();
    const audio::SoundHandle handle = rt.handleOf(radio);
    ASSERT_TRUE(engine.isValid(handle));
    EXPECT_TRUE(radio.get<AudioSourceComponent>().playing);
    std::vector<f32> buffer(512 * engine.channels());
    engine.render(buffer.data(), 512);
    const f32 nearGain = engine.audibility(handle);

    radio.setPosition({20.f, 0.f, 0.f});
    h.run(0.1);
    engine.render(buffer.data(), 512);
    const f32 farGain = engine.audibility(handle);
    EXPECT_GT(nearGain, 0.f);
    EXPECT_LT(farGain, nearGain * 0.3f) << "inverse attenuation: 1/2 vs 1/20";

    // Leaving play mode stops the voice.
    h.scheduler.setPlaying(false);
    h.tick();
    EXPECT_FALSE(engine.isValid(handle) && engine.isPlaying(handle));
}

TEST(GameplayDebugDraw, EditModeVisualisesSplinesAndColliders) {
    GameplayHarness h(testConfig(), [](Services& s) { s.emplace<DebugDraw>(); });
    Entity path = h.world.create("Path");
    auto& sc = path.add<SplineComponent>();
    sc.points = {SplinePoint{{0, 0, 0}}, SplinePoint{{5, 0, 0}}, SplinePoint{{5, 0, 5}}};
    h.ground();
    h.runtime<PhysicsRuntime>().debugDraw = true;
    h.start(false);
    h.tick();
    auto& draw = h.services.get<DebugDraw>();
    draw.flush(0.f);
    EXPECT_GT(draw.depthTestedLines().size() + draw.overlayLines().size(), 50u);
    EXPECT_EQ(h.runtime<PhysicsRuntime>().physicsWorld().bodyCount(), 0u) << "edit mode: visualisation only";
}

TEST(GameplayAudio, PhysicsOcclusionCountsWalls) {
    GameplayHarness h;
    Entity wall = h.world.create("Wall");
    wall.setPosition({0.f, 0.f, -5.f});
    wall.add<ColliderComponent>().halfExtents = {5.f, 5.f, 0.2f};
    h.start();
    h.tick();
    PhysicsOcclusionProvider occlusion(h.runtime<PhysicsRuntime>(), 0.6f);
    EXPECT_NEAR(occlusion.occlusion({0, 0, 0}, {0, 0, -10}), 0.6f, 1e-4f);
    EXPECT_NEAR(occlusion.occlusion({0, 0, 0}, {0, 0, 4}), 0.f, 1e-6f);
}

TEST(GameplayScript, LuaModuleApis) {
    GameplayConfig cfg = testConfig();
    cfg.physicsWorld.gravity = glm::vec3(0.f);
    GameplayHarness h(cfg);
    auto skel = std::make_shared<anim::Skeleton>();
    skel->addJoint("Root", anim::kNoJoint, {});
    skel->finalize();
    const Uuid skelId = Uuid::generate();
    h.assets.addSkeleton(skelId, skel);
    h.assets.addScript("apis", R"(
        function onStart(self)
            local e = self.entity
            e.body:addImpulse(vec3(0, 0, -10))
            self.len = scene.find("Path").spline:length()
            self.p = scene.find("Path").spline:positionAt(1)
            self.sound = audio.play("does/not/exist.wav")
            e.animator:setFloat("speed", 2)
            self.state = e.animator.currentState
            self.moved = e.agent:moveTo(vec3(1, 0, 0))
            self.noise = ai.findPath(vec3(0, 0, 0), vec3(1, 0, 0))
            self.children = #e:children()
            self.child = e:findChild("Grandchild").name
            self.removed = e:remove("AudioListener")
        end
        function onUpdate(self, dt) self.vz = self.entity.body.linearVelocity.z end
    )");
    Entity path = h.world.create("Path");
    path.setPosition({0.f, 0.f, 3.f});
    path.add<SplineComponent>().points = {SplinePoint{{0, 0, 0}}, SplinePoint{{4, 0, 0}}};
    Entity e = h.box("Probe", {0.f, 0.f, 0.f});
    e.get<RigidBodyComponent>().mass = 2.f;
    auto& an = e.add<AnimatorComponent>();
    an.skeleton = skelId;
    an.inlineController.parameters.push_back({"speed"});
    an.inlineController.states.push_back({"Idle"});
    e.add<NavAgentComponent>();
    e.add<AudioListenerComponent>();
    Entity child = h.world.create("Child", e);
    h.world.create("Grandchild", child);
    e.add<ScriptComponent>().script = "apis";
    h.start();
    h.run(0.2);
    auto& vm = h.runtime<ScriptRuntime>().vm();
    EXPECT_EQ(vm.errorCount(), 0u) << vm.lastError();
    sol::table self = h.runtime<ScriptRuntime>().self(e);
    EXPECT_NEAR(self["vz"].get_or(0.0), -5.0, 0.05) << "impulse 10 on 2 kg";
    EXPECT_NEAR(self["len"].get_or(0.0), 4.0, 1e-3);
    EXPECT_EQ(self["p"].get<glm::vec3>(), glm::vec3(1, 0, 3));
    EXPECT_FALSE(self["sound"].valid() && self["sound"].get_type() != sol::type::lua_nil);
    EXPECT_EQ(self["state"].get_or(std::string()), "Idle");
    EXPECT_TRUE(self["moved"].get_or(false)) << "pending until a navmesh exists";
    EXPECT_EQ(self["children"].get_or(0), 1);
    EXPECT_EQ(self["child"].get_or(std::string()), "Grandchild");
    EXPECT_TRUE(self["removed"].get_or(false));
    EXPECT_FALSE(e.has<AudioListenerComponent>());
    EXPECT_FLOAT_EQ(e.get<AnimatorComponent>().parameters.count("speed") ? 1.f : 0.f, 0.f);
}
