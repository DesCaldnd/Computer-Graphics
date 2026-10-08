// Глава 14: сервер и клиент, подключение, типизированные сообщения, RPC; MemoryNetwork для тестов и
// ENet для реальной сети (docs/guide/14-networking.md).
#include <oxwald/net/net.hpp>

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace ox;
using namespace ox::net;

namespace {

// Типизированное сообщение: имя -> id (netHash), сериализация вручную.
struct ChatMessage {
    static constexpr std::string_view kNetName = "game.chat";
    std::string text;
    u8 team = 0;
    void serialize(BitWriter& w) const {
        w.writeString(text);
        w.writeRangedInt(team, 0, 3);
    }
    void deserialize(BitReader& r) {
        text = r.readString(256); // ограничение длины — защита от «враждебных» пакетов
        team = static_cast<u8>(r.readRangedInt(0, 3));
    }
};

// Сервер и клиент в одном процессе поверх детерминированной MemoryNetwork.
struct LocalSession {
    MemoryNetwork network{42};
    NetServer server{network.createTransport(), NetServerConfig{.tickRate = 30.f}};
    NetClient client{network.createTransport()};
    f64 now = 0.0, nextTick = 0.0;

    void step(f64 dt = 1.0 / 120.0) { // один «кадр»: poll каждый кадр, tick с частотой сервера
        now += dt;
        server.poll(now);
        if (now >= nextTick) {
            server.tick(now);
            nextTick += 1.0 / server.tickRate();
        }
        client.poll(now);
    }
    bool runUntil(const std::function<bool()>& done, f64 seconds = 3.0) {
        for (f64 end = now + seconds; now < end && !done();) step();
        return done();
    }
};

} // namespace

TEST(GuideNetMessages, ConnectChatAndRpcs) {
    LocalSession s;
    std::vector<std::string> serverLog, clientChat;
    s.server.onClientConnected = [&](PeerId id) { serverLog.push_back("join " + std::to_string(id)); };

    // Сервер: принимает чат и пересылает всем; RPC "player.fire" с проверкой аргументов.
    s.server.messages().on<ChatMessage>([&](PeerId from, const ChatMessage& m) {
        s.server.broadcast(ChatMessage{"[" + std::to_string(from) + "] " + m.text, m.team});
    });
    glm::vec3 shotDir{0.f};
    s.server.rpcs().bind("player.fire", [&](PeerId from, glm::vec3 origin, glm::vec3 dir) {
        if (glm::length(dir) < 0.5f) return; // всегда валидируйте ввод клиента
        shotDir = dir;
        s.server.multicast("fx.explosion", origin + dir * 10.f, 5.f); // всем клиентам
    });

    // Клиент: подписки на чат и RPC от сервера (первый параметр — всегда PeerId отправителя).
    s.client.messages().on<ChatMessage>([&](PeerId, const ChatMessage& m) { clientChat.push_back(m.text); });
    glm::vec3 boomAt{0.f};
    f32 boomRadius = 0.f;
    s.client.rpcs().bind("fx.explosion", [&](PeerId, glm::vec3 at, f32 radius) {
        boomAt = at;
        boomRadius = radius;
    });

    ASSERT_TRUE(s.server.start());
    ASSERT_TRUE(s.client.connect("memory", s.server.port())); // имя хоста MemoryNetwork игнорирует
    ASSERT_TRUE(s.runUntil([&] { return s.client.connected(); }));
    EXPECT_EQ(serverLog.size(), 1u);

    s.client.send(ChatMessage{"привет", 1});
    s.client.callServer("player.fire", glm::vec3(0.f), glm::vec3(1.f, 0.f, 0.f));
    ASSERT_TRUE(s.runUntil([&] { return !clientChat.empty() && boomRadius > 0.f; }));
    EXPECT_EQ(clientChat[0], "[" + std::to_string(s.client.clientId()) + "] привет");
    EXPECT_EQ(shotDir, glm::vec3(1.f, 0.f, 0.f));
    EXPECT_EQ(boomAt, glm::vec3(10.f, 0.f, 0.f));
}

TEST(GuideNetMessages, KickWithGameReason) {
    LocalSession s;
    constexpr u32 kCheating = static_cast<u32>(DisconnectReason::UserBase) + 1; // свои причины — от UserBase
    u32 reason = 0;
    s.client.onDisconnected = [&](u32 r) { reason = r; };
    ASSERT_TRUE(s.server.start());
    ASSERT_TRUE(s.client.connect("memory", s.server.port()));
    ASSERT_TRUE(s.runUntil([&] { return s.client.connected(); }));
    s.server.kick(s.server.clients().front(), kCheating);
    ASSERT_TRUE(s.runUntil([&] { return !s.client.connected(); }));
    EXPECT_EQ(reason, kCheating);
}

TEST(GuideNetMessages, LossyNetworkStillDeliversReliableMessagesInOrder) {
    LocalSession s;
    s.network.setConditions({.latency = 0.05, .jitter = 0.02, .loss = 0.2f, .duplicate = 0.05f}); // «плохой Wi-Fi»
    std::vector<std::string> got;
    s.server.messages().on<ChatMessage>([&](PeerId, const ChatMessage& m) { got.push_back(m.text); });
    ASSERT_TRUE(s.server.start());
    ASSERT_TRUE(s.client.connect("memory", s.server.port()));
    ASSERT_TRUE(s.runUntil([&] { return s.client.connected(); }, 10.0));
    for (int i = 0; i < 20; ++i) s.client.send(ChatMessage{std::to_string(i), 0}); // ReliableOrdered по умолчанию
    ASSERT_TRUE(s.runUntil([&] { return got.size() == 20; }, 10.0));
    for (int i = 0; i < 20; ++i) EXPECT_EQ(got[static_cast<usize>(i)], std::to_string(i));
    EXPECT_GT(s.network.datagramsDropped(), 0u);
}

TEST(GuideNetMessages, RealEnetOnLocalhost) {
    // Тот же код поверх UDP: меняется только транспорт.
    NetServer server(makeEnetTransport(), {.listen = {"127.0.0.1", 0}}); // порт 0 = любой свободный
    NetClient client(makeEnetTransport({.timeoutMs = 2000}));
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(client.connect("127.0.0.1", server.port()));
    std::string received;
    server.messages().on<ChatMessage>([&](PeerId, const ChatMessage& m) { received = m.text; });
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count(); };
    bool sent = false;
    while (received.empty() && elapsed() < 5.0) {
        server.poll(elapsed()); // ENet живёт по своим часам, now нужен для синхронизации времени
        client.poll(elapsed());
        if (client.connected() && !sent) {
            client.send(ChatMessage{"по UDP", 0});
            sent = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(received, "по UDP");
    client.disconnect();
}
