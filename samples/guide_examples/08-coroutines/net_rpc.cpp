// Глава 08: сетевой запрос с тайм-аутом — RpcCall/serveRequest (цель Oxwald::async_net) поверх
// in-memory сети (docs/guide/08-coroutines.md, docs/guide/14-networking.md).
#include <oxwald/async/async.hpp>
#include <oxwald/async/net_rpc.hpp>
#include <oxwald/net/net.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ox;

// Тип ответа: нужен NetCodec (запись/чтение/сравнение) и конструктор по умолчанию.
struct Inventory {
    std::vector<std::string> items;
};

template <>
struct ox::net::NetCodec<Inventory> {
    void write(BitWriter& w, const Inventory& v) const {
        w.writeVarU32(static_cast<u32>(v.items.size()));
        for (const auto& s : v.items) w.writeString(s);
    }
    void read(BitReader& r, Inventory& v) const {
        const u32 n = r.readVarU32();
        v.items.clear();
        for (u32 i = 0; i < n && r.ok() && i < 256; ++i) v.items.push_back(r.readString(64));
    }
    bool equal(const Inventory& a, const Inventory& b) const { return a.items == b.items; }
};

namespace {

struct Hud {
    bool spinner = false;
    std::vector<std::string> toasts;
    std::optional<Inventory> shown;
};

Task<> openInventory(Hud& hud, net::RpcCall<Inventory, u32>& fetch, u32 slot) {
    hud.spinner = true;
    std::optional<Inventory> inv;
    try {
        inv = co_await realTimeout(fetch(slot), 3.0); // сеть живёт в реальном времени, даже на паузе
    } catch (const AsyncError& e) {
        hud.toasts.push_back(e.what()); // исключение на сервере или нет соединения
    }
    hud.spinner = false;
    if (inv) hud.shown = std::move(*inv);
    else if (hud.toasts.empty()) hud.toasts.push_back("Сервер не ответил");
}

// Сервер + клиент в одном процессе: детерминированная MemoryNetwork и симулированное время.
struct Session {
    net::MemoryNetwork network{1};
    net::NetServer server{network.createTransport()};
    net::NetClient client{network.createTransport()};
    CoroutineScheduler sched;
    f64 now = 0.0;

    void frame() { // один кадр игры: сеть -> корутины
        now += 1.0 / 60.0;
        server.poll(now);
        client.poll(now); // ответы RPC приходят здесь, на главном потоке
        sched.tick(1.0 / 60.0);
    }
    bool connect() {
        if (!server.start() || !client.connect("memory", server.port())) return false;
        for (int i = 0; i < 300 && !client.connected(); ++i) frame();
        return client.connected();
    }
};

} // namespace

TEST(GuideCoroNetRpc, RequestReplyAndServerError) {
    Session s;
    // Сервер: обработчик возвращает ответ; исключение уйдёт клиенту как AsyncError.
    net::serveRequest<Inventory, u32>(s.server, "inv.fetch", [](net::PeerId from, u32 slot) {
        if (slot > 3) throw std::runtime_error("no such slot");
        return Inventory{{"sword", "potion"}};
    });
    ASSERT_TRUE(s.connect());

    net::RpcCall<Inventory, u32> fetch(s.client, "inv.fetch"); // держите как член класса
    Hud hud;
    auto ok = s.sched.spawn(openInventory(hud, fetch, 0));
    EXPECT_TRUE(hud.spinner);
    for (int i = 0; i < 300 && ok.isRunning(); ++i) s.frame();
    ASSERT_TRUE(hud.shown.has_value());
    EXPECT_EQ(hud.shown->items, (std::vector<std::string>{"sword", "potion"}));
    EXPECT_FALSE(hud.spinner);

    Hud hud2;
    auto bad = s.sched.spawn(openInventory(hud2, fetch, 9));
    for (int i = 0; i < 300 && bad.isRunning(); ++i) s.frame();
    ASSERT_EQ(hud2.toasts.size(), 1u);
    EXPECT_EQ(hud2.toasts[0], "no such slot");
}

TEST(GuideCoroNetRpc, TimeoutWhenServerIsSilent) {
    Session s; // сервер ничего не отвечает на "inv.fetch"
    s.server.rpcs().bind("inv.fetch", [](net::PeerId, u32 /*requestId*/, u32 /*slot*/) {});
    ASSERT_TRUE(s.connect());
    net::RpcCall<Inventory, u32> fetch(s.client, "inv.fetch");
    Hud hud;
    auto h = s.sched.spawn(openInventory(hud, fetch, 0));
    for (int i = 0; i < 60 * 4 && h.isRunning(); ++i) s.frame(); // 4 с
    EXPECT_TRUE(h.isDone());
    EXPECT_EQ(hud.toasts, std::vector<std::string>{"Сервер не ответил"});
    EXPECT_EQ(fetch.pending(), 1u); // запрос всё ещё учтён...
    fetch.failAll("disconnected");  // ...до failAll() (вызывайте при разрыве соединения)
    EXPECT_EQ(fetch.pending(), 0u);
}

TEST(GuideCoroNetRpc, NotConnectedFailsImmediately) {
    Session s; // connect() не вызывался
    net::RpcCall<Inventory, u32> fetch(s.client, "inv.fetch");
    Hud hud;
    auto h = s.sched.spawn(openInventory(hud, fetch, 0));
    s.sched.tick(0.016);
    EXPECT_TRUE(h.isDone());
    ASSERT_EQ(hud.toasts.size(), 1u);
    EXPECT_NE(hud.toasts[0].find("not connected"), std::string::npos);
}
