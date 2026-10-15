#include "gameplay_test_utils.hpp"

#include <oxwald/scene/scene_serializer.hpp>

#include <filesystem>

using namespace ox;
using namespace ox::gameplay;
using namespace ox::gameplay::test;

namespace {

Entity destructibleBox(GameplayHarness& h, std::string_view name, glm::vec3 position, glm::vec3 half, u32 chunks,
                       physics::MotionType motion = physics::MotionType::Static) {
    Entity e = h.world.create(name);
    e.setPosition(position);
    e.add<ColliderComponent>().halfExtents = half;
    if (motion != physics::MotionType::Static) e.add<RigidBodyComponent>().motionType = motion;
    auto& mr = e.add<MeshRendererComponent>();
    mr.mesh = Uuid::fromName("test.cube");
    mr.materials = {Uuid::fromName("test.outer")};
    auto& d = e.add<DestructibleComponent>();
    d.chunkCount = chunks;
    d.seed = 7;
    d.interiorMaterial = Uuid::fromName("test.inner");
    return e;
}

// Projectile flying along +X without gravity.
Entity projectile(GameplayHarness& h, glm::vec3 position, f32 speed, f32 mass) {
    Entity p = h.box("Projectile", position, glm::vec3(0.2f));
    auto& rb = p.get<RigidBodyComponent>();
    rb.mass = mass;
    rb.gravityFactor = 0.f;
    rb.linearDamping = 0.f;
    rb.initialLinearVelocity = {speed, 0.f, 0.f};
    rb.ccd = true;
    return p;
}

} // namespace

TEST(GameplayDestruction, ComponentSerializationRoundTrip) {
    registerGameplayTypes();
    World world;
    Entity e = world.create("Wall");
    auto& d = e.add<DestructibleComponent>();
    d.chunkCount = 31;
    d.seed = 1234;
    d.sourceMesh = Uuid::generate();
    d.health = 12.5f;
    d.impulseThreshold = 900.f;
    d.structural = true;
    d.anchor = DestructionAnchor::None;
    d.debrisLayer = "Debris";
    d.maxActiveDebris = 9;
    d.broken = true;
    const auto dir = std::filesystem::temp_directory_path() / "ox_gameplay_tests";
    std::filesystem::create_directories(dir);
    const ComponentInfo* info = ComponentRegistry::instance().find("Destructible");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->category, "Physics");
    const ComponentInfo* chunkInfo = ComponentRegistry::instance().find("DestructionChunk");
    ASSERT_NE(chunkInfo, nullptr);
    EXPECT_FALSE(chunkInfo->serializable);
    for (const char* name : {"destructible.oxscene", "destructible.oxscene.json"}) {
        ASSERT_TRUE(saveScene(world, dir / name));
        World loaded;
        ASSERT_TRUE(loadScene(loaded, dir / name));
        const Entity le = loaded.find(e.uuid());
        ASSERT_TRUE(le.valid() && le.has<DestructibleComponent>()) << name;
        const auto& l = le.get<DestructibleComponent>();
        EXPECT_EQ(l.chunkCount, 31u);
        EXPECT_EQ(l.seed, 1234u);
        EXPECT_EQ(l.sourceMesh, d.sourceMesh);
        EXPECT_FLOAT_EQ(l.health, 12.5f);
        EXPECT_FLOAT_EQ(l.impulseThreshold, 900.f);
        EXPECT_TRUE(l.structural);
        EXPECT_EQ(l.anchor, DestructionAnchor::None);
        EXPECT_EQ(l.debrisLayer, "Debris");
        EXPECT_EQ(l.maxActiveDebris, 9u);
        EXPECT_TRUE(l.broken);
        EXPECT_EQ(info->serialize(world, e.handle()), info->serialize(loaded, le.handle()));
    }
}

TEST(GameplayDestruction, PrepareIsDeterministicAndShared) {
    GameplayHarness h;
    Entity a = destructibleBox(h, "A", {0, 1, 0}, {0.5f, 0.5f, 0.5f}, 10);
    Entity b = destructibleBox(h, "B", {3, 1, 0}, {0.5f, 0.5f, 0.5f}, 10);
    h.start();
    auto& rt = h.runtime<DestructionRuntime>();
    auto fa = rt.prepare(a);
    auto fb = rt.prepare(b);
    ASSERT_TRUE(fa && fb);
    EXPECT_EQ(fa.get(), fb.get()); // same source + settings: one fracture, one set of chunk meshes
    EXPECT_EQ(fa->result->chunks.size(), 10u);
    EXPECT_EQ(rt.chunkMeshes().size(), 10u);
    b.patch<DestructibleComponent>([](DestructibleComponent& d) { d.seed = 8; });
    auto fb2 = rt.prepare(b);
    ASSERT_TRUE(fb2);
    EXPECT_NE(fb2->key, fa->key);
    EXPECT_EQ(rt.chunkMeshes().size(), 20u);
}

TEST(GameplayDestruction, BreaksOnImpulseOverThresholdOnly) {
    for (const bool strong : {false, true}) {
        SCOPED_TRACE(strong);
        GameplayHarness h;
        Entity wall = destructibleBox(h, "Wall", {0, 0, 0}, {0.2f, 1.f, 1.f}, 8);
        wall.get<DestructibleComponent>().impulseThreshold = 30.f;
        // 2 kg at 8 m/s ~ 16 N*s (under), 2 kg at 25 m/s ~ 50 N*s (over).
        projectile(h, {-2.f, 0.f, 0.f}, strong ? 25.f : 8.f, 2.f);
        u32 events = 0;
        DestructionEvent last;
        h.start();
        ScopedConnection c = h.runtime<DestructionRuntime>().onDestruction.connect([&](const DestructionEvent& ev) {
            ++events;
            last = ev;
        });
        h.run(1.0);
        const auto& d = wall.get<DestructibleComponent>();
        EXPECT_EQ(d.broken, strong);
        EXPECT_EQ(events, strong ? 1u : 0u);
        EXPECT_EQ(h.runtime<DestructionRuntime>().chunksOf(wall).size(), strong ? 8u : 0u);
        if (strong) {
            EXPECT_EQ(last.cause, DestructionCause::Impact);
            EXPECT_EQ(last.totalChunks, 8u);
            EXPECT_GT(last.impulse.x, 0.f); // pushed along the projectile's motion
            EXPECT_FALSE(wall.has<ColliderComponent>());
            EXPECT_FALSE(wall.has<MeshRendererComponent>());
            // Chunks render the fracture meshes with outer + interior materials and have bodies.
            for (Entity ch : h.runtime<DestructionRuntime>().chunksOf(wall)) {
                const auto& mr = ch.get<MeshRendererComponent>();
                EXPECT_EQ(mr.materials.back(), Uuid::fromName("test.inner"));
                EXPECT_TRUE(h.runtime<PhysicsRuntime>().bodyOf(ch).valid());
                EXPECT_EQ(ch.get<ColliderComponent>().type, ColliderType::ConvexHull);
            }
        } else {
            EXPECT_TRUE(wall.has<ColliderComponent>());
        }
    }
}

TEST(GameplayDestruction, DamageAccumulatesAndRespectsThreshold) {
    GameplayHarness h;
    Entity box = destructibleBox(h, "Crate", {0, 1, 0}, glm::vec3(0.5f), 6);
    auto& d = box.get<DestructibleComponent>();
    d.health = 50.f;
    d.damageThreshold = 10.f;
    h.start();
    h.tick();
    auto& rt = h.runtime<DestructionRuntime>();
    EXPECT_FALSE(rt.applyDamage(box, 5.f, box.worldPosition())); // below threshold: ignored
    EXPECT_FLOAT_EQ(box.get<DestructibleComponent>().health, 50.f);
    EXPECT_FALSE(rt.applyDamage(box, 30.f, box.worldPosition()));
    EXPECT_FLOAT_EQ(box.get<DestructibleComponent>().health, 20.f);
    EXPECT_TRUE(rt.applyDamage(box, 30.f, box.worldPosition(), {0, 0, 50}));
    EXPECT_TRUE(rt.isBroken(box));
    EXPECT_FALSE(rt.applyDamage(box, 30.f, box.worldPosition())); // already broken
    EXPECT_EQ(rt.chunksOf(box).size(), 6u);
}

TEST(GameplayDestruction, ChunksInheritVelocity) {
    GameplayHarness h;
    Entity box = destructibleBox(h, "Moving", {0, 5, 0}, glm::vec3(0.5f), 8, physics::MotionType::Dynamic);
    auto& rb = box.get<RigidBodyComponent>();
    rb.gravityFactor = 0.f;
    rb.linearDamping = 0.f;
    rb.initialLinearVelocity = {3.f, 0.f, 0.f};
    h.start();
    h.run(0.2);
    auto& rt = h.runtime<DestructionRuntime>();
    ASSERT_TRUE(rt.fracture(box, box.worldPosition()));
    h.tick();
    h.tick();
    const auto chunks = rt.chunksOf(box);
    ASSERT_EQ(chunks.size(), 8u);
    glm::vec3 mean{0.f};
    for (Entity c : chunks) mean += h.runtime<PhysicsRuntime>().linearVelocity(c);
    mean /= f32(chunks.size());
    EXPECT_NEAR(mean.x, 3.f, 0.5f);
    EXPECT_NEAR(mean.z, 0.f, 0.5f);
}

TEST(GameplayDestruction, DebrisBudgetIsRespected) {
    GameplayHarness h;
    h.ground();
    Entity a = destructibleBox(h, "A", {0, 1, 0}, glm::vec3(0.5f), 12);
    Entity b = destructibleBox(h, "B", {4, 1, 0}, glm::vec3(0.5f), 12);
    b.get<DestructibleComponent>().maxActiveDebris = 3; // per object
    h.start();
    h.tick();
    auto& rt = h.runtime<DestructionRuntime>();
    rt.setMaxActiveDebris(5); // global
    ASSERT_TRUE(rt.fracture(a, a.worldPosition()));
    EXPECT_LE(rt.activeDebrisCount(), 5u);
    EXPECT_GE(rt.fadingCount(), 7u);
    ASSERT_TRUE(rt.fracture(b, b.worldPosition()));
    EXPECT_LE(rt.activeDebrisCount(), 5u);
    u32 activeB = 0;
    for (Entity c : rt.chunksOf(b)) activeB += c.has<RigidBodyComponent>() ? 1u : 0u;
    EXPECT_LE(activeB, 3u);
    // Faded chunks are destroyed.
    h.run(1.0);
    EXPECT_EQ(rt.fadingCount(), 0u);
    EXPECT_LE(rt.chunksOf(a).size() + rt.chunksOf(b).size(), 5u);
    EXPECT_LE(rt.activeDebrisCount(), 5u);
}

TEST(GameplayDestruction, RestingDebrisFadesAfterLifetime) {
    GameplayHarness h;
    h.ground();
    Entity box = destructibleBox(h, "Crate", {0, 0.5f, 0}, glm::vec3(0.5f), 6);
    auto& d = box.get<DestructibleComponent>();
    d.debrisLifetime = 0.5f;
    d.fadeDuration = 0.25f;
    h.start();
    h.tick();
    auto& rt = h.runtime<DestructionRuntime>();
    ASSERT_TRUE(rt.fracture(box, box.worldPosition()));
    EXPECT_EQ(rt.activeDebrisCount(), 6u);
    ASSERT_TRUE(h.runUntil([&] { return rt.chunksOf(box).empty(); }, 4.0));
    h.tick(); // deferred destruction flushed at the end of the frame
    for (auto [e, c] : h.world.view<DestructionChunkComponent>().each()) ADD_FAILURE() << "chunk entity left";
}

TEST(GameplayDestruction, StructuralCollapseWhenAnchorsAreGone) {
    GameplayHarness h;
    h.ground();
    Entity wall = destructibleBox(h, "Wall", {0, 1, 0}, {1.5f, 1.f, 0.15f}, 24);
    auto& d = wall.get<DestructibleComponent>();
    d.structural = true;
    d.anchor = DestructionAnchor::Bottom;
    d.breakRadius = 0.3f;
    u32 collapsed = 0, fractured = 0;
    h.start();
    h.tick();
    auto& rt = h.runtime<DestructionRuntime>();
    ScopedConnection c = rt.onDestruction.connect([&](const DestructionEvent& ev) {
        if (ev.type == DestructionEvent::Type::Collapsed) ++collapsed;
        if (ev.type == DestructionEvent::Type::Fractured) ++fractured;
    });
    ASSERT_TRUE(rt.fracture(wall, {0.f, 1.95f, 0.f}, {0.f, 0.f, 20.f}));
    EXPECT_EQ(fractured, 1u);
    auto prepared = rt.prepare(wall);
    ASSERT_TRUE(prepared);
    const u32 frozen = rt.frozenChunkCount(wall);
    ASSERT_GT(frozen, 0u);
    ASSERT_LT(frozen, 24u);
    // Every frozen chunk is held: connected to an anchored frozen chunk through frozen chunks.
    const auto chunks = rt.chunksOf(wall);
    std::vector<int> state(24, 0); // 0 gone/moving, 1 frozen, 2 frozen+anchored
    u32 anchoredCount = 0;
    std::vector<Entity> anchoredChunks;
    for (Entity ch : chunks) {
        const auto& m = ch.get<DestructionChunkComponent>();
        if (!m.frozen) continue;
        state[m.index] = m.anchored ? 2 : 1;
        if (m.anchored) {
            ++anchoredCount;
            anchoredChunks.push_back(ch);
        }
    }
    ASSERT_GT(anchoredCount, 0u);
    ASSERT_GT(frozen, anchoredCount); // some frozen chunks rest on others
    const auto adj = prepared->result->adjacency();
    std::vector<bool> held(24, false);
    std::vector<u32> stack;
    for (u32 i = 0; i < 24; ++i) {
        if (state[i] == 2) held[i] = true, stack.push_back(i);
    }
    while (!stack.empty()) {
        const u32 i = stack.back();
        stack.pop_back();
        for (u32 j : adj[i]) {
            if (state[j] != 0 && !held[j]) held[j] = true, stack.push_back(j);
        }
    }
    for (u32 i = 0; i < 24; ++i) {
        if (state[i] != 0) EXPECT_TRUE(held[i]) << "chunk " << i << " floats";
    }
    // Knock out every anchor: the rest of the wall loses its support and collapses.
    for (Entity a : anchoredChunks) rt.applyDamage(a, 100.f, a.worldPosition());
    EXPECT_EQ(rt.frozenChunkCount(wall), 0u);
    EXPECT_GE(collapsed, 1u);
    // Collapsed chunks fall onto the ground.
    h.run(2.0);
    for (Entity ch : rt.chunksOf(wall)) EXPECT_LT(ch.worldPosition().y, 2.f);
}

TEST(GameplayDestruction, ProjectileKnocksFrozenChunkOut) {
    GameplayHarness h;
    h.ground();
    Entity wall = destructibleBox(h, "Wall", {0, 1, 0}, {0.15f, 1.f, 1.5f}, 24);
    auto& d = wall.get<DestructibleComponent>();
    d.structural = true;
    d.breakRadius = 0.25f;
    d.impulseThreshold = 10.f;
    h.start();
    h.tick();
    auto& rt = h.runtime<DestructionRuntime>();
    ASSERT_TRUE(rt.fracture(wall, {0.f, 1.9f, 1.4f}));
    const u32 before = rt.frozenChunkCount(wall);
    ASSERT_GT(before, 2u);
    projectile(h, {-2.f, 1.f, 0.f}, 30.f, 2.f);
    h.run(0.5);
    EXPECT_LT(rt.frozenChunkCount(wall), before);
}

TEST(GameplayDestruction, ReplicatedBrokenFlagFractures) {
    GameplayHarness h;
    Entity box = destructibleBox(h, "Remote", {0, 1, 0}, glm::vec3(0.5f), 5);
    h.start();
    h.tick();
    auto& rt = h.runtime<DestructionRuntime>();
    EXPECT_TRUE(rt.chunksOf(box).empty());
    box.get<DestructibleComponent>().broken = true; // e.g. written by a snapshot on a client
    u32 replicated = 0;
    ScopedConnection c = h.services.get<EventBus>().subscribe<DestructionEvent>([&](const DestructionEvent& ev) {
        if (ev.cause == DestructionCause::Replicated) ++replicated;
    });
    h.tick();
    EXPECT_EQ(rt.chunksOf(box).size(), 5u);
    EXPECT_EQ(replicated, 1u);
}

TEST(GameplayDestruction, LuaAppliesDamage) {
    GameplayHarness h;
    h.assets.addScript("breaker", R"(
        function onStart(self)
            local w = scene.find("Wall")
            self.before = destruction.isBroken(w)
            self.weak = destruction.applyDamage(w, 10)
            self.health = destruction.health(w)
            self.broke = destruction.applyDamage(w, 500, vec3(0, 1, 0), vec3(30, 0, 0))
            self.after = destruction.isBroken(w)
            self.chunks = #destruction.chunks(w)
            self.debris = destruction.activeDebris()
        end
    )");
    h.assets.addScript("wall", R"(
        function onEvent(self, name, payload)
            if name == "broken" then self.brokenChunks = payload end
        end
    )");
    Entity wall = destructibleBox(h, "Wall", {0, 1, 0}, glm::vec3(0.5f), 9);
    wall.add<ScriptComponent>().script = "wall";
    Entity driver = h.world.create("Driver");
    driver.add<ScriptComponent>().script = "breaker";
    h.start();
    h.run(0.1);
    sol::table self = h.runtime<ScriptRuntime>().self(driver);
    ASSERT_TRUE(self.valid());
    EXPECT_FALSE(self.get_or("before", true));
    EXPECT_FALSE(self.get_or("weak", true));
    EXPECT_FLOAT_EQ(self.get_or("health", 0.f), 90.f);
    EXPECT_TRUE(self.get_or("broke", false));
    EXPECT_TRUE(self.get_or("after", false));
    EXPECT_EQ(self.get_or("chunks", 0), 9);
    EXPECT_EQ(self.get_or("debris", 0), 9);
    sol::table wallSelf = h.runtime<ScriptRuntime>().self(wall);
    ASSERT_TRUE(wallSelf.valid());
    EXPECT_DOUBLE_EQ(wallSelf.get_or("brokenChunks", 0.0), 9.0);
}

TEST(GameplayDestruction, BeamCollapsesWhenSupportsBreak) {
    GameplayHarness h;
    h.ground();
    Entity left = destructibleBox(h, "PillarL", {-2.f, 1.f, 0.f}, {0.3f, 1.f, 0.3f}, 6);
    Entity right = destructibleBox(h, "PillarR", {2.f, 1.f, 0.f}, {0.3f, 1.f, 0.3f}, 6);
    Entity beam = destructibleBox(h, "Beam", {0.f, 2.25f, 0.f}, {2.6f, 0.25f, 0.4f}, 20);
    auto& d = beam.get<DestructibleComponent>();
    d.structural = true;
    d.anchor = DestructionAnchor::Static;
    d.breakRadius = 1.2f;
    std::vector<DestructionEvent> events;
    h.start();
    auto& rt = h.runtime<DestructionRuntime>();
    ScopedConnection c = rt.onDestruction.connect([&](const DestructionEvent& ev) { events.push_back(ev); });
    h.run(0.2);
    EXPECT_FALSE(rt.isBroken(beam)); // resting on both pillars
    ASSERT_TRUE(rt.fracture(left, left.worldPosition() + glm::vec3(0.f, 0.f, -0.3f), {0.f, 0.f, 200.f}));
    h.run(0.2);
    ASSERT_TRUE(rt.isBroken(beam)) << "losing a support breaks the beam";
    bool collapseCause = false;
    for (const auto& ev : events) collapseCause |= ev.entity == beam && ev.cause == DestructionCause::Collapse;
    EXPECT_TRUE(collapseCause);
    const u32 frozen = rt.frozenChunkCount(beam);
    EXPECT_GT(frozen, 0u);   // the end over the right pillar still stands ...
    EXPECT_LT(frozen, 20u);  // ... the end over the broken pillar fell
    // Chunks left over the right pillar are anchored there.
    for (Entity ch : rt.chunksOf(beam)) {
        const auto& m = ch.get<DestructionChunkComponent>();
        if (m.frozen && m.anchored) EXPECT_GT(ch.worldPosition().x, 0.5f);
    }
    ASSERT_TRUE(rt.fracture(right, right.worldPosition() + glm::vec3(0.f, 0.f, -0.3f), {0.f, 0.f, 200.f}));
    h.run(0.2);
    EXPECT_EQ(rt.frozenChunkCount(beam), 0u); // nothing holds the rest any more
}

TEST(GameplayDestruction, FastCcdProjectileBreaksScaledColumn) {
    GameplayHarness h;
    h.ground();
    // Like the showcase: unit box collider on a scaled entity, a 40 kg cannonball at 34 m/s with CCD.
    Entity column = h.world.create("Column");
    column.setPosition({0.f, 1.6f, 0.f});
    column.setScale({0.9f, 3.2f, 0.9f});
    column.add<ColliderComponent>().halfExtents = glm::vec3(0.5f);
    auto& d = column.add<DestructibleComponent>();
    d.chunkCount = 12;
    d.impulseThreshold = 250.f;
    Entity ball = h.world.create("Ball");
    ball.setPosition({-8.f, 1.2f, 0.f});
    ball.setScale(glm::vec3(0.5f));
    auto& rb = ball.add<RigidBodyComponent>();
    rb.mass = 40.f;
    rb.ccd = true;
    rb.linearDamping = 0.f;
    rb.initialLinearVelocity = {34.f, 0.f, 0.f};
    auto& c = ball.add<ColliderComponent>();
    c.type = ColliderType::Sphere;
    c.radius = 0.5f;
    f32 maxImpulse = 0.f;
    h.start();
    ScopedConnection con = h.runtime<PhysicsRuntime>().onCollision.connect([&](const CollisionEvent& ev) {
        if (ev.phase == ContactPhase::Begin) maxImpulse = std::max(maxImpulse, ev.impulse);
    });
    h.run(1.0);
    EXPECT_GT(maxImpulse, 250.f);
    EXPECT_TRUE(column.get<DestructibleComponent>().broken);
}

TEST(GameplayDestruction, BlastAtTheFootCollapsesTower) {
    GameplayHarness h;
    h.ground();
    Entity tower = h.world.create("Tower");
    tower.setPosition({0.f, 3.f, 0.f});
    tower.setScale({1.5f, 6.f, 1.5f});
    tower.add<ColliderComponent>().halfExtents = glm::vec3(0.5f);
    auto& d = tower.add<DestructibleComponent>();
    d.chunkCount = 36;
    d.seed = 41;
    d.structural = true;
    d.breakRadius = 1.0f;
    u32 collapsed = 0;
    h.start();
    h.tick();
    auto& rt = h.runtime<DestructionRuntime>();
    ScopedConnection c = rt.onDestruction.connect([&](const DestructionEvent& ev) {
        collapsed += ev.type == DestructionEvent::Type::Collapsed ? 1u : 0u;
    });
    // The blast centre is 2.5 m below the entity origin, inside the tower's bounds.
    EXPECT_GE(rt.applyRadialDamage({0.f, 0.5f, 0.f}, 1.8f, 1000.f, 150.f), 1u);
    EXPECT_TRUE(rt.isBroken(tower));
    EXPECT_GE(collapsed, 1u);
    EXPECT_LT(rt.frozenChunkCount(tower), 8u);
}

TEST(GameplayDestruction, DynamicCrateBreaksWhenHit) {
    GameplayHarness h;
    h.ground();
    Entity crate = destructibleBox(h, "Crate", {0.f, 0.45f, 0.f}, glm::vec3(0.45f), 8, physics::MotionType::Dynamic);
    crate.get<RigidBodyComponent>().mass = 25.f;
    crate.get<DestructibleComponent>().impulseThreshold = 120.f;
    h.start();
    h.run(0.5); // settle (and sleep)
    EXPECT_FALSE(crate.get<DestructibleComponent>().broken) << "resting on the ground does not break it";
    Entity ball = h.world.create("Ball");
    ball.setPosition({-6.f, 0.45f, 0.f});
    ball.setScale(glm::vec3(0.5f));
    auto& rb = ball.add<RigidBodyComponent>();
    rb.mass = 40.f;
    rb.ccd = true;
    rb.initialLinearVelocity = {34.f, 0.f, 0.f};
    auto& col = ball.add<ColliderComponent>();
    col.type = ColliderType::Sphere;
    col.radius = 0.5f;
    f32 maxImpulse = 0.f;
    ScopedConnection con = h.runtime<PhysicsRuntime>().onCollision.connect([&](const CollisionEvent& ev) {
        if (ev.phase == ContactPhase::Begin && (ev.a == crate || ev.b == crate)) maxImpulse = std::max(maxImpulse, ev.impulse);
    });
    h.run(1.0);
    EXPECT_GT(maxImpulse, 120.f);
    EXPECT_TRUE(crate.get<DestructibleComponent>().broken);
}

TEST(GameplayDestruction, KnockedOutChunksFlyAlongTheShot) {
    GameplayHarness h;
    h.ground();
    Entity wall = h.world.create("Wall");
    wall.setPosition({0.f, 1.75f, 0.f});
    wall.setScale({0.45f, 3.5f, 7.f}); // thin along X, the shot travels +X
    wall.add<ColliderComponent>().halfExtents = glm::vec3(0.5f);
    auto& d = wall.add<DestructibleComponent>();
    d.chunkCount = 64;
    d.seed = 31;
    d.structural = true;
    d.breakRadius = 0.9f;
    d.impulseThreshold = 300.f;
    Entity ball = h.world.create("Ball");
    ball.setPosition({-8.f, 1.15f, -0.7f});
    ball.setScale(glm::vec3(0.5f));
    auto& rb = ball.add<RigidBodyComponent>();
    rb.mass = 40.f;
    rb.ccd = true;
    rb.initialLinearVelocity = {34.f, 0.f, 0.f};
    auto& col = ball.add<ColliderComponent>();
    col.type = ColliderType::Sphere;
    col.radius = 0.5f;
    h.start();
    auto& rt = h.runtime<DestructionRuntime>();
    ASSERT_TRUE(h.runUntil([&] { return rt.isBroken(wall); }, 1.0));
    std::vector<std::pair<Entity, f32>> start;
    for (Entity c : rt.chunksOf(wall)) {
        if (!c.get<DestructionChunkComponent>().frozen) start.emplace_back(c, c.worldPosition().x);
    }
    h.run(0.5);
    u32 moving = 0;
    f32 mean = 0.f;
    for (const auto& [c, x0] : start) {
        if (!c.valid()) continue;
        ++moving;
        // Prism chunks (planar seeding of the thin wall, in metres) are not keyed into their neighbours: they leave
        // backwards, along the shot, never towards the shooter.
        EXPECT_GT(c.worldPosition().x - x0, -0.01f) << c.name();
        mean += c.worldPosition().x - x0;
    }
    ASSERT_GT(moving, 0u);
    EXPECT_GT(mean / f32(moving), 0.05f);
}
