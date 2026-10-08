// Глава 14: предсказание на клиенте и согласование с сервером (ClientPrediction / ServerInputQueue),
// ввод отправляется своим пакетом PacketKind::User (docs/guide/14-networking.md).
#include <oxwald/net/net.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <functional>

using namespace ox;
using namespace ox::net;

namespace {

struct MoveState {
    f32 x = 0.f;
};
struct MoveInput {
    f32 axis = 0.f; // -1..1
};

// Одна и та же детерминированная симуляция на сервере и клиенте.
void simulate(MoveState& s, const MoveInput& in, f32 dt) { s.x += in.axis * 5.f * dt; }
bool nearlyEqual(const MoveState& a, const MoveState& b) { return std::abs(a.x - b.x) < 1e-3f; }

constexpr auto kInputPacket = static_cast<PacketKind>(static_cast<u8>(PacketKind::User) + 0);

} // namespace

TEST(GuideNetPrediction, OfflineReconcile) {
    ClientPrediction<MoveState, MoveInput> predict(simulate, nearlyEqual);
    for (int i = 0; i < 10; ++i) predict.applyInput({1.f}, 0.1f); // игрок жмёт «вправо» 1 с
    EXPECT_NEAR(predict.state().x, 5.f, 1e-4f);                    // мгновенный отклик без ожидания сервера

    // Сервер обработал 4 ввода, но его x на 1 больше (например, толчок от взрыва).
    EXPECT_TRUE(predict.reconcile(4, MoveState{2.f + 1.f})); // откат к серверному состоянию + повтор 6 вводов
    EXPECT_NEAR(predict.state().x, 6.f, 1e-4f);
    EXPECT_EQ(predict.pending().size(), 6u);
    EXPECT_FALSE(predict.reconcile(2, MoveState{})); // устаревший ответ игнорируется
}

TEST(GuideNetPrediction, OverTheNetworkWithLatency) {
    MemoryNetwork network{3};
    network.setConditions({.latency = 0.06, .jitter = 0.01, .loss = 0.05f});
    NetServer server(network.createTransport(), {.tickRate = 60.f});
    NetClient client(network.createTransport());

    // --- Сервер: очередь вводов на клиента, авторитетное состояние, ответ (seq, x) каждый тик.
    ServerInputQueue<MoveInput> queue;
    MoveState authoritative;
    server.onUserPacket = [&](PeerId, PacketKind kind, BitReader& r) {
        if (kind != kInputPacket) return;
        const u32 count = r.readVarU32();
        for (u32 i = 0; i < count && i < 64 && r.ok(); ++i) {
            const u32 seq = r.readVarU32();
            const MoveInput in{r.readF32()};
            const f32 dt = r.readF32();
            if (r.ok()) queue.receive(seq, in, dt); // дубликаты и старые seq отбрасываются
        }
    };

    // --- Клиент: предсказание + согласование по ответу сервера.
    ClientPrediction<MoveState, MoveInput> predict(simulate, nearlyEqual);
    client.rpcs().bind("move.ack", [&](PeerId, u32 lastSeq, f32 x) { predict.reconcile(lastSeq, MoveState{x}); });

    ASSERT_TRUE(server.start());
    ASSERT_TRUE(client.connect("memory", server.port()));
    f64 now = 0.0;
    constexpr f32 dt = 1.f / 60.f;
    for (int i = 0; i < 300 && !client.connected(); ++i) {
        now += dt;
        server.poll(now);
        client.poll(now);
    }
    ASSERT_TRUE(client.connected());

    for (int tick = 0; tick < 240; ++tick) {
        now += dt;
        // Клиент: применяем ввод сразу и шлём все неподтверждённые вводы (избыточно, ненадёжный канал).
        if (tick < 180) predict.applyInput({tick % 60 < 40 ? 1.f : -1.f}, dt);
        if (!predict.pending().empty()) {
            BitWriter w;
            w.writeU8(static_cast<u8>(kInputPacket));
            w.writeVarU32(static_cast<u32>(predict.pending().size()));
            for (const auto& p : predict.pending()) {
                w.writeVarU32(p.seq);
                w.writeF32(p.input.axis);
                w.writeF32(p.dt);
            }
            client.sendRaw(w.data(), Channel::UnreliableSequenced);
        }
        server.poll(now);
        // Сервер: исполняем пришедшие вводы по порядку и сообщаем, до какого seq досчитали.
        queue.process([&](const MoveInput& in, f32 d) { simulate(authoritative, in, d); });
        if (tick == 90) authoritative.x += 2.f; // событие, о котором клиент не знал -> расхождение
        for (PeerId c : server.clients()) server.callClient(c, "move.ack", queue.lastProcessed(), authoritative.x);
        server.tick(now);
        client.poll(now);
    }
    EXPECT_GE(predict.corrections(), 1u);                     // был откат и повтор
    EXPECT_EQ(predict.lastAcked(), predict.lastSequence());   // все вводы подтверждены
    EXPECT_NEAR(predict.state().x, authoritative.x, 1e-3f);   // клиент сошёлся с сервером
}
