// Глава 14: репликация объектов (NetObject), дельта-снапшоты, релевантность по дистанции, владелец,
// интерполяция на клиенте (docs/guide/14-networking.md).
#include <oxwald/net/net.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ox;
using namespace ox::net;

namespace {

// Реплицируемый объект: один и тот же класс на сервере и на клиенте (создаётся фабрикой).
struct Crate : NetObject {
    glm::vec3 pos{0.f};
    glm::quat rot{1.f, 0.f, 0.f, 0.f};
    f32 health = 100.f;
    std::string label;
    InterpolationBuffer interp; // только на клиенте: история для плавного движения

    Crate() : NetObject("Crate") {
        // Порядок регистрации свойств должен совпадать на сервере и клиенте.
        replicate("pos", pos, QuantizedVec3Codec{-2048.f, 2048.f, 0.01f});
        replicate("rot", rot, CompressedQuatCodec{10});
        replicate("health", health, QuantizedFloatCodec{0.f, 100.f, 0.5f});
        replicate("label", label); // NetCodec<std::string> по умолчанию
        setPositionSource([this] { return pos; }); // для релевантности/приоритета по дистанции
    }
    void onSnapshotApplied(f64 serverTime) override { interp.push({.time = serverTime, .position = pos, .rotation = rot}); }
};

// «ECS-компонент», который хранится вне NetObject: реплицируем через getter/setter.
struct HealthComponent {
    f32 value = 100.f;
};

struct LocalSession {
    MemoryNetwork network{7};
    NetServer server{network.createTransport()};
    NetClient client{network.createTransport()};
    f64 now = 0.0, nextTick = 0.0;
    void step(f64 dt = 1.0 / 120.0) {
        now += dt;
        server.poll(now);
        if (now >= nextTick) {
            server.tick(now); // снапшоты уходят здесь
            nextTick += 1.0 / server.tickRate();
        }
        client.poll(now);
    }
    bool runUntil(const std::function<bool()>& done, f64 seconds = 3.0) {
        for (f64 end = now + seconds; now < end && !done();) step();
        return done();
    }
    void run(f64 seconds) {
        for (f64 end = now + seconds; now < end;) step();
    }
    bool connect() {
        return server.start() && client.connect("memory", server.port()) &&
               runUntil([&] { return client.connected(); });
    }
    PeerId firstClient() const { return server.clients().front(); }
};

} // namespace

TEST(GuideNetReplication, SpawnUpdateDespawn) {
    LocalSession s;
    ASSERT_TRUE(s.connect());
    // Клиент: как создавать объекты по имени типа + реакции на появление/исчезновение.
    s.client.replication().factory().registerType<Crate>("Crate");
    std::vector<std::string> events;
    s.client.replication().onSpawn = [&](NetObject& o) { events.push_back("spawn " + o.typeName()); };
    s.client.replication().onDespawn = [&](NetObject& o) { events.push_back("despawn " + o.typeName()); };

    // Сервер: создаём и регистрируем объект.
    auto crate = std::make_shared<Crate>();
    crate->pos = {5.f, 0.f, 3.f};
    crate->label = "ammo";
    const NetId id = s.server.replication().add(crate);

    ASSERT_TRUE(s.runUntil([&] { return s.client.replication().find(id) != nullptr; }));
    auto* mirror = static_cast<Crate*>(s.client.replication().find(id));
    EXPECT_EQ(mirror->label, "ammo");
    EXPECT_NEAR(mirror->pos.x, 5.f, 0.006f);

    // Меняем одно поле — уходит только оно (дельта к подтверждённому клиентом состоянию).
    s.run(0.3);
    crate->health = 40.f;
    ASSERT_TRUE(s.runUntil([&] { return mirror->health == 40.f; }));
    EXPECT_EQ(mirror->label, "ammo");

    s.server.replication().remove(id); // исчезает у всех клиентов
    ASSERT_TRUE(s.runUntil([&] { return s.client.replication().find(id) == nullptr; }));
    EXPECT_EQ(events, (std::vector<std::string>{"spawn Crate", "despawn Crate"}));
}

TEST(GuideNetReplication, DistanceRelevancyAndOwnership) {
    LocalSession s;
    ASSERT_TRUE(s.connect());
    s.client.replication().factory().registerType<Crate>("Crate");

    auto far = std::make_shared<Crate>();
    far->pos = {500.f, 0.f, 0.f};
    far->setRelevancy(Relevancy::Distance, 150.f); // виден в радиусе 150 м от камеры клиента
    auto mine = std::make_shared<Crate>();
    mine->setOwner(s.firstClient()); // владелец получает объект всегда
    mine->setRelevancy(Relevancy::OwnerOnly);
    const NetId farId = s.server.replication().add(far);
    const NetId mineId = s.server.replication().add(mine);
    s.server.replication().setViewerPosition(s.firstClient(), glm::vec3(0.f)); // позиция камеры игрока

    s.run(0.5);
    EXPECT_EQ(s.client.replication().find(farId), nullptr);
    ASSERT_NE(s.client.replication().find(mineId), nullptr);
    EXPECT_TRUE(s.client.replication().isOwned(*s.client.replication().find(mineId)));

    s.server.replication().setViewerPosition(s.firstClient(), glm::vec3(450.f, 0.f, 0.f)); // игрок подошёл
    ASSERT_TRUE(s.runUntil([&] { return s.client.replication().find(farId) != nullptr; }));
}

TEST(GuideNetReplication, ExternalComponentViaGetterSetter) {
    LocalSession s;
    ASSERT_TRUE(s.connect());
    HealthComponent serverHp, clientHp; // «компоненты» на двух сторонах
    s.client.replication().factory().registerType("Barrel", [&](NetTypeId, NetId, PeerId) {
        auto o = std::make_shared<NetObject>("Barrel");
        o->replicate<f32>("hp", [&] { return clientHp.value; }, [&](const f32& v) { clientHp.value = v; });
        return o;
    });
    auto barrel = std::make_shared<NetObject>("Barrel");
    barrel->replicate<f32>("hp", [&] { return serverHp.value; }, [&](const f32& v) { serverHp.value = v; });
    s.server.replication().add(barrel);

    serverHp.value = 12.5f;
    ASSERT_TRUE(s.runUntil([&] { return clientHp.value == 12.5f; }));
}

TEST(GuideNetReplication, ClientInterpolatesRemoteObjects) {
    LocalSession s;
    s.network.setConditions({.latency = 0.03});
    ASSERT_TRUE(s.connect());
    s.client.replication().factory().registerType<Crate>("Crate");
    auto crate = std::make_shared<Crate>();
    const NetId id = s.server.replication().add(crate);
    for (int i = 0; i < 240; ++i) { // 2 с: ящик едет со скоростью 6 м/с
        crate->pos.x = static_cast<f32>(s.now * 6.0);
        s.step();
    }
    auto* mirror = static_cast<Crate*>(s.client.replication().find(id));
    ASSERT_NE(mirror, nullptr);
    // Рисуем в прошлом: renderTime = serverTime - interpolationDelay (по умолчанию 100 мс).
    const f64 renderTime = s.client.renderTime();
    std::optional<InterpolatedTransform> t = mirror->interp.sample(renderTime);
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(t->extrapolated);
    EXPECT_NEAR(t->position.x, renderTime * 6.0, 0.1);
}

TEST(GuideNetReplication, BandwidthBudget) {
    LocalSession s;
    ASSERT_TRUE(s.connect());
    s.client.replication().factory().registerType<Crate>("Crate");
    ReplicationConfig cfg = s.server.replication().config();
    cfg.bytesPerTick = 200; // бюджет снапшота на клиента за тик
    s.server.replication().setConfig(cfg);
    std::vector<std::shared_ptr<Crate>> crates;
    for (int i = 0; i < 30; ++i) {
        crates.push_back(std::make_shared<Crate>());
        crates.back()->label = "crate #" + std::to_string(i);
        s.server.replication().add(crates.back());
    }
    u32 maxBytes = 0;
    for (int i = 0; i < 30; ++i) {
        s.step(1.0 / 30.0);
        maxBytes = std::max(maxBytes, s.server.lastSnapshotStats(s.firstClient()).bytes);
    }
    EXPECT_LE(maxBytes, cfg.bytesPerTick); // лишнее откладывается на следующие тики, никто не «голодает»
    ASSERT_TRUE(s.runUntil([&] { return s.client.replication().objects().size() == 30; }));
}
