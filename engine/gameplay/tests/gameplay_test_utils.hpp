#pragma once

#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <memory>

namespace ox::gameplay::test {

inline GameplayConfig testConfig() {
    GameplayConfig c;
    c.physicsWorld.workerThreads = 0; // deterministic and cheap for tests
    c.physicsWorld.maxBodies = 1024;
    c.physicsWorld.maxBodyPairs = 1024;
    c.physicsWorld.maxContactConstraints = 1024;
    c.physicsWorld.tempAllocatorBytes = 8u << 20;
    c.scriptVM.hotReloadInterval = 0.0;
    return c;
}

// World + services + scheduler with every gameplay system. Member order matters: the scheduler detaches first,
// then services (runtimes) die, then the world; the asset registry outlives all of them.
class GameplayHarness {
public:
    explicit GameplayHarness(GameplayConfig config = testConfig(), std::function<void(Services&)> preServices = {}) {
        registerGameplayTypes();
        assets.registerIn(services);
        if (preServices) preServices(services);
        addGameplaySystems(scheduler, services, config);
    }
    ~GameplayHarness() { scheduler.detach(); }

    void start(bool playing = true) {
        scheduler.attach(world, services);
        scheduler.setPlaying(playing);
    }
    void tick(f64 dt = 1.0 / 60.0) { scheduler.tick(*active, services, dt); }
    void run(f64 seconds, f64 dt = 1.0 / 60.0) {
        const int frames = static_cast<int>(seconds / dt + 0.5);
        for (int i = 0; i < frames; ++i) tick(dt);
    }
    bool runUntil(const std::function<bool()>& pred, f64 maxSeconds, f64 dt = 1.0 / 60.0) {
        const int frames = static_cast<int>(maxSeconds / dt + 0.5);
        for (int i = 0; i < frames; ++i) {
            if (pred()) return true;
            tick(dt);
        }
        return pred();
    }

    template <class T>
    T& runtime() {
        return services.get<T>();
    }

    Entity ground(glm::vec3 halfExtents = {20.f, 0.5f, 20.f}) {
        Entity g = world.create("Ground");
        g.setPosition({0.f, -halfExtents.y, 0.f});
        auto& c = g.add<ColliderComponent>();
        c.type = ColliderType::Box;
        c.halfExtents = halfExtents;
        return g;
    }
    Entity box(std::string_view name, glm::vec3 position, glm::vec3 halfExtents = glm::vec3(0.5f),
               physics::MotionType motion = physics::MotionType::Dynamic) {
        Entity b = world.create(name);
        b.setPosition(position);
        auto& rb = b.add<RigidBodyComponent>();
        rb.motionType = motion;
        auto& c = b.add<ColliderComponent>();
        c.halfExtents = halfExtents;
        return b;
    }

    GameplayAssetRegistry assets;
    World world;
    World* active = &world;
    Services services;
    SystemScheduler scheduler;
};

} // namespace ox::gameplay::test
