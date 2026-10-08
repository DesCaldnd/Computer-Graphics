// Глава 14: выделенный (headless) сервер — runDedicatedServer: poll -> симуляция -> снапшоты с фиксированной
// частотой (docs/guide/14-networking.md). В тесте — симулированное время и MemoryNetwork вместо ENet.
#include <oxwald/net/net.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <memory>

using namespace ox;
using namespace ox::net;

namespace {

struct Platform : NetObject { // движущаяся платформа — часть «мира» сервера
    glm::vec3 pos{0.f};
    Platform() : NetObject("Platform") { replicate("pos", pos, QuantizedVec3Codec{-100.f, 100.f, 0.01f}); }
};

} // namespace

TEST(GuideNetDedicated, HeadlessServerLoop) {
    MemoryNetwork network;
    std::unique_ptr<NetClient> client; // клиент «на той стороне» — только для проверки
    auto platform = std::make_shared<Platform>();
    std::atomic<bool> stop{false};     // в настоящем сервере его выставляет обработчик SIGINT/SIGTERM

    DedicatedServerConfig cfg;
    cfg.server.tickRate = 60.f;
    cfg.server.maxClients = 16;
    cfg.transportFactory = [&] { return network.createTransport(); }; // по умолчанию — ENet
    cfg.realtime = false;  // без sleep, время = tick / tickRate (тесты, soak-прогоны)
    cfg.maxTicks = 600;    // страховка; 0 = до stopFlag
    cfg.stopFlag = &stop;

    const int rc = runDedicatedServer(
        cfg,
        // Каждый тик: симуляция мира. Снапшоты сервер отправит сам после этой функции.
        [&](NetServer& server, u32 tick, f64 dt) {
            platform->pos.x = static_cast<f32>(std::sin(tick * dt) * 10.0);
            client->poll(server.now()); // только для теста: клиент в том же процессе
            auto* mirror = static_cast<Platform*>(client->replication().find(platform->netId()));
            if (mirror && tick > 120 && std::abs(mirror->pos.x - platform->pos.x) < 1.f) stop = true;
        },
        // После старта: регистрируем объекты мира (и, в тесте, подключаем клиента).
        [&](NetServer& server) {
            server.replication().add(platform);
            client = std::make_unique<NetClient>(network.createTransport());
            client->replication().factory().registerType<Platform>("Platform");
            client->connect("memory", server.port());
        });

    EXPECT_EQ(rc, 0); // 0 = чистое завершение
    EXPECT_TRUE(stop.load());
    ASSERT_TRUE(client->connected());
    EXPECT_NE(client->replication().find(platform->netId()), nullptr);
}
