# 14. Сеть

> Модуль `net` (таргет `Oxwald::net`, пространство имён `ox::net`). Зависит от `core` и glm; ENet — приватная зависимость, в публичных заголовках её нет. Модуль не знает про ECS: объекты реплицируются через `NetObject` с геттерами и сеттерами. Общий заголовок: `#include <oxwald/net/net.hpp>`.

## Зачем

Сетевая игра — это авторитетный сервер и клиенты, которые:

1. **обмениваются сообщениями и RPC** — чат, «игрок выстрелил», «взрыв здесь»;
2. **получают состояние мира** — сервер рассылает снапшоты объектов, отправляя только изменения;
3. **не видят задержку** — чужие объекты плавно интерполируются в прошлом, свой персонаж предсказывается и мгновенно реагирует на ввод.

Модуль закрывает все три пункта: транспорт поверх UDP (ENet) и детерминированная in-memory сеть для тестов, побитовая сериализация, типизированные сообщения и RPC, дельта-репликация с бюджетом трафика и релевантностью, интерполяция, предсказание с согласованием, синхронизация часов и цикл выделенного сервера.

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| `ITransport` | Пакетный транспорт: `listen`, `connect`, `send`, `poll`. Реализации: ENet (`makeEnetTransport()`), `MemoryNetwork` (в процессе, детерминированно), `SimulatedTransport` (обёртка с задержкой/потерями). |
| `PeerId` | Локальный id удалённой стороны; `0` (`kInvalidPeer`) — невалидный. На клиенте отправитель всегда сервер. |
| `Channel` | `ReliableOrdered` (доставка и порядок: чат, RPC), `UnreliableSequenced` (может потеряться, но никогда старше последнего: ввод), `Unreliable` (снапшоты). |
| `NetServer` / `NetClient` | Сервер и клиент: подключения, сообщения, RPC, репликация, часы. Вызывайте `poll(now)` каждый кадр, на сервере ещё `tick(now)` с частотой тиков. |
| Сообщение (`NetMessage`) | Структура с `kNetName`, `serialize(BitWriter&)`, `deserialize(BitReader&)`. id на проводе — `netHash(kNetName)`. |
| RPC | Вызов по имени: `rpcs().bind("name", handler)`, `callServer(...)` / `callClient(...)` / `multicast(...)`. Типы аргументов выводятся из обработчика. |
| `BitWriter` / `BitReader` | Побитовая упаковка: varint, целые в диапазоне, квантованные float/vec3, сжатые кватернионы, строки. |
| `NetCodec<T>` | Как писать/читать/сравнивать `T` в сети. Есть для bool, целых, enum, f32/f64, string, glm vec/quat; специализируйте для своих типов. |
| `NetObject` | Реплицируемый объект: набор свойств (`replicate(...)`), владелец, релевантность, приоритет. |
| `NetId` | id объекта в сессии; никогда не переиспользуется. |
| `InterpolationBuffer` | История снапшотов объекта на клиенте, выборка в момент `renderTime()`. |
| `ClientPrediction` / `ServerInputQueue` | Предсказание своего персонажа на клиенте и очередь вводов на сервере. |
| `runDedicatedServer` | Headless-цикл: poll → симуляция → снапшоты с фиксированной частотой. |

### Кадр клиента и тик сервера

```
сервер, каждый кадр:   server.poll(now)                  // приём, подключения, сообщения, RPC
сервер, 1/tickRate:    симуляция мира -> server.tick(now) // снапшоты всем клиентам
клиент, каждый кадр:   client.poll(now)                  // приём, снапшоты, RPC, пинг часов
```

`now` — ваши часы в секундах. `MemoryNetwork` живёт целиком на них (тесты шагают симулированным временем), ENet использует собственные часы, но `now` нужен для синхронизации времени и снапшотов.

## Шаг 1. Сервер, клиент и подключение

Для тестов и локальной разработки удобна `MemoryNetwork`: сервер и клиент в одном процессе, никаких сокетов, полный детерминизм при заданном seed.

```cpp
#include <oxwald/net/net.hpp>
using namespace ox::net;

struct LocalSession {
    MemoryNetwork network{42};
    NetServer server{network.createTransport(), NetServerConfig{.tickRate = 30.f}};
    NetClient client{network.createTransport()};
    f64 now = 0.0, nextTick = 0.0;

    void step(f64 dt = 1.0 / 120.0) {   // один «кадр»
        now += dt;
        server.poll(now);
        if (now >= nextTick) {
            server.tick(now);            // снапшоты уходят здесь
            nextTick += 1.0 / server.tickRate();
        }
        client.poll(now);
    }
};

LocalSession s;
s.server.onClientConnected = [&](PeerId id) { /* заспавнить игрока */ };
s.server.start();
s.client.connect("memory", s.server.port());   // имя хоста MemoryNetwork игнорирует
while (!s.client.connected()) s.step();
```

С настоящей сетью меняется только транспорт:

```cpp
NetServer server(makeEnetTransport(), {.listen = {"0.0.0.0", 27015}, .tickRate = 30.f});
NetClient client(makeEnetTransport({.timeoutMs = 5000}));
client.connect("127.0.0.1", 27015);
```

Важные настройки:

| Где | Поле | Смысл |
| --- | --- | --- |
| `NetServerConfig` | `listen` | Адрес и порт (`"0.0.0.0"` — все интерфейсы, порт `0` — любой свободный, узнать через `server.port()`). |
| | `protocolVersion` | Клиент с другой версией отклоняется (`DisconnectReason::VersionMismatch`). |
| | `maxClients`, `tickRate` | Лимит игроков; частота снапшотов. |
| | `replication` | Бюджет и параметры репликации (`ReplicationConfig`). |
| `NetClientConfig` | `interpolationDelay` | Насколько в прошлом рисовать чужие объекты (по умолчанию 0.1 с ≈ 3 снапшота при 30 Гц). |
| `EnetTransportConfig` | `timeoutMs`, ограничения полосы | Таймаут ENet; `simulatedIncomingLoss` — искусственные потери (только для тестов). |

Отключение: `client.disconnect()`, `server.kick(peer, reason)`. Причина приходит в `onDisconnected(u32 reason)` / `onClientDisconnected(peer, reason)`. Свои причины нумеруйте от `DisconnectReason::UserBase`:

```cpp
constexpr u32 kCheating = static_cast<u32>(DisconnectReason::UserBase) + 1;
s.server.kick(s.server.clients().front(), kCheating);
```

Полный пример: `samples/guide_examples/14-networking/messages_rpc.cpp`.

## Шаг 2. Сообщения и RPC

**Сообщение** — структура с именем и сериализацией:

```cpp
struct ChatMessage {
    static constexpr std::string_view kNetName = "game.chat";
    std::string text;
    u8 team = 0;
    void serialize(BitWriter& w) const {
        w.writeString(text);
        w.writeRangedInt(team, 0, 3);
    }
    void deserialize(BitReader& r) {
        text = r.readString(256);   // ограничение длины — защита от «враждебных» пакетов
        team = static_cast<u8>(r.readRangedInt(0, 3));
    }
};

// сервер: принимает и пересылает всем
s.server.messages().on<ChatMessage>([&](PeerId from, const ChatMessage& m) {
    s.server.broadcast(ChatMessage{"[" + std::to_string(from) + "] " + m.text, m.team});
});
// клиент
s.client.messages().on<ChatMessage>([&](PeerId, const ChatMessage& m) { chat.push_back(m.text); });
s.client.send(ChatMessage{"привет", 1});        // ReliableOrdered по умолчанию
```

**RPC** — вызов по имени; типы аргументов берутся из сигнатуры обработчика, первый параметр всегда `PeerId` отправителя:

```cpp
s.server.rpcs().bind("player.fire", [&](PeerId from, glm::vec3 origin, glm::vec3 dir) {
    if (glm::length(dir) < 0.5f) return;                           // всегда валидируйте ввод клиента
    s.server.multicast("fx.explosion", origin + dir * 10.f, 5.f);   // всем клиентам
});
s.client.rpcs().bind("fx.explosion", [&](PeerId, glm::vec3 at, f32 radius) { spawnExplosion(at, radius); });

s.client.callServer("player.fire", glm::vec3(0.f), glm::vec3(1.f, 0.f, 0.f));
```

| Отправка | Кому |
| --- | --- |
| `client.send(msg, channel)` / `client.callServer(rpc, args...)` | Серверу |
| `server.send(peer, msg)` / `server.callClient(peer, rpc, args...)` | Одному клиенту |
| `server.broadcast(msg)` / `server.multicast(rpc, args...)` | Всем клиентам |
| `server.multicastRelevant(netId, rpc, args...)` | Только клиентам, у которых объект `netId` заспавнен |

RPC всегда идут по `ReliableOrdered`. Тип аргумента должен иметь `NetCodec` (шаг 3). Неизвестные и битые пакеты логируются и отбрасываются. Нужен запрос с ответом и тайм-аутом — это `RpcCall` из [главы 08](08-coroutines.md#шаг-10-сетевой-запрос-с-тайм-аутом).

Даже при 20 % потерь, джиттере и дубликатах надёжный канал доставляет всё и по порядку — это легко проверить на `MemoryNetwork`:

```cpp
s.network.setConditions({.latency = 0.05, .jitter = 0.02, .loss = 0.2f, .duplicate = 0.05f});   // «плохой Wi-Fi»
```

Полный пример: `samples/guide_examples/14-networking/messages_rpc.cpp`.

## Шаг 3. Сериализация: BitWriter, квантование, NetCodec

Пакеты пишутся побитово. Вместо «как есть» (40+ байт) состояние игрока занимает 13 байт:

```cpp
void writePlayer(BitWriter& w, const PlayerState& p) {
    w.writeRangedInt(p.team, 0, 3);                            // 2 бита
    w.writeQuantizedFloat(p.health, 0.f, 100.f, 0.1f);        // 10 бит, ошибка <= 0.05
    w.writeQuantizedVec3(p.position, -1024.f, 1024.f, 0.01f); // 18 бит на ось
    w.writeQuat(p.rotation, 10);                               // 32 бита («smallest three»)
    w.writeBool(p.crouching);                                  // 1 бит
}

BitReader r(w.data());
PlayerState out = readPlayer(r);
if (!r.ok()) { /* пакет обрезан или испорчен */ }
```

| Метод | Размер | Когда |
| --- | --- | --- |
| `writeBool`, `writeBits(v, n)` | 1 / n бит | Флаги, малые поля |
| `writeVarU32/U64`, `writeVarI32/I64` | 1–10 байт | Счётчики, id; знаковые — zig-zag, малые отрицательные тоже короткие |
| `writeRangedInt(v, min, max)` | ровно `bitsRequired(max-min)` | Команды, состояния, перечисления |
| `writeQuantizedFloat/Vec3(v, min, max, step)` | по числу шагов | Позиции, здоровье, углы |
| `writeQuat(q, bits)` | 2 + 3·bits | Повороты |
| `writeF32/F64`, `writeVec3` | 32/64 бита на число | Когда точность важнее размера |
| `writeString`, `writeBytes` | длина + данные | Строки (при чтении задавайте `maxLength`) |

Чтение за концом не падает: возвращаются нули и выставляется флаг ошибки. Проверяйте `r.ok()` один раз после пачки чтений.

**Свой тип** в RPC и репликации — специализация `NetCodec` с тремя методами:

```cpp
struct ItemStack { u16 itemId = 0; u8 count = 0; };

template <>
struct ox::net::NetCodec<ItemStack> {
    void write(BitWriter& w, const ItemStack& v) const {
        w.writeVarU32(v.itemId);
        w.writeRangedInt(v.count, 0, 99);
    }
    void read(BitReader& r, ItemStack& v) const {
        v.itemId = static_cast<u16>(r.readVarU32());
        v.count = static_cast<u8>(r.readRangedInt(0, 99));
    }
    // «Одинаково ли на проводе» — репликация не пересылает равные значения.
    bool equal(const ItemStack& a, const ItemStack& b) const { return a.itemId == b.itemId && a.count == b.count; }
};
```

Готовые квантующие кодеки для `replicate`: `QuantizedFloatCodec{min, max, step}`, `QuantizedVec3Codec{min, max, step}`, `CompressedQuatCodec{bits}`, `RangedIntCodec{min, max}`. Их `equal` сравнивает квантованные значения, поэтому дрожание меньше шага квантования не вызывает повторной отправки.

Полный пример: `samples/guide_examples/14-networking/bit_stream.cpp`.

## Шаг 4. Репликация объектов

`NetObject` — объект, состояние которого сервер рассылает клиентам. Обычно это подкласс, и тот же класс создаётся на клиенте фабрикой:

```cpp
struct Crate : NetObject {
    glm::vec3 pos{0.f};
    glm::quat rot{1.f, 0.f, 0.f, 0.f};
    f32 health = 100.f;
    std::string label;
    InterpolationBuffer interp;     // только на клиенте

    Crate() : NetObject("Crate") {
        // Порядок регистрации свойств должен совпадать на сервере и клиенте.
        replicate("pos", pos, QuantizedVec3Codec{-2048.f, 2048.f, 0.01f});
        replicate("rot", rot, CompressedQuatCodec{10});
        replicate("health", health, QuantizedFloatCodec{0.f, 100.f, 0.5f});
        replicate("label", label);                    // NetCodec<std::string> по умолчанию
        setPositionSource([this] { return pos; });    // для релевантности и приоритета по дистанции
    }
    void onSnapshotApplied(f64 serverTime) override {
        interp.push({.time = serverTime, .position = pos, .rotation = rot});
    }
};

// клиент: как создавать объекты и что делать при появлении/исчезновении
s.client.replication().factory().registerType<Crate>("Crate");
s.client.replication().onSpawn = [&](NetObject& o) { /* создать визуал */ };
s.client.replication().onDespawn = [&](NetObject& o) { /* удалить визуал */ };

// сервер
auto crate = std::make_shared<Crate>();
NetId id = s.server.replication().add(crate);   // появится у всех релевантных клиентов
crate->health = 40.f;                            // уйдёт в ближайшем тике (только это поле)
s.server.replication().remove(id);               // исчезнет у всех

auto* mirror = static_cast<Crate*>(s.client.replication().find(id));   // копия на клиенте
```

Как это работает:

- каждый тик сервер один раз снимает состояние всех объектов, а для каждого клиента пишет **дельту** к последнему подтверждённому этим клиентом состоянию: маска изменённых полей + сами значения. Неизменившийся объект стоит 0 байт, одно изменённое число — около 10 байт;
- снапшоты идут по ненадёжному каналу; клиент подтверждает их, потери и переупорядочивание не ломают дельты;
- спавн/деспавн пересылаются, пока клиент их не подтвердит.

**Без подкласса** — например, для ECS-компонентов, которые живут в `World`, — свойства привязываются геттером и сеттером:

```cpp
auto barrel = std::make_shared<NetObject>("Barrel");
barrel->replicate<f32>("hp", [&] { return hp.value; }, [&](const f32& v) { hp.value = v; });

s.client.replication().factory().registerType("Barrel", [&](NetTypeId, NetId, PeerId) {
    auto o = std::make_shared<NetObject>("Barrel");
    o->replicate<f32>("hp", [&] { return clientHp.value; }, [&](const f32& v) { clientHp.value = v; });
    return o;
});
```

Геттер и сеттер должны быть симметричны: `set(get())` ничего не меняет.

### Релевантность, владелец, приоритет и бюджет

| Настройка | Что делает |
| --- | --- |
| `setRelevancy(Relevancy::Always)` | Видят все (по умолчанию). |
| `setRelevancy(Relevancy::Distance, radius)` | Видят клиенты, чья точка обзора ближе `radius`. Точку задаёт сервер: `replication().setViewerPosition(peer, cameraPos)`. |
| `setRelevancy(Relevancy::OwnerOnly)` | Только владелец (инвентарь, личные квесты). |
| `setRelevancy(Relevancy::Custom)` + `setRelevancyFilter(fn)` | Решает ваша функция `(PeerId, const glm::vec3* viewer) -> bool`. |
| `setOwner(peer)` | Владелец (до `add`). Владелец получает объект всегда; на клиенте `replication().isOwned(obj)`. |
| `setPriority(p)` | Чем выше, тем чаще объект попадает в снапшот при нехватке бюджета. |
| `ReplicationConfig::bytesPerTick` | Бюджет снапшота на клиента за тик (по умолчанию 1200 байт). Не влезшее откладывается, «голодания» нет. |

Объект, ставший нерелевантным, исчезает у клиента (`onDespawn`), а вновь релевантный появляется с полным состоянием. Статистика последнего снапшота: `server.lastSnapshotStats(peer)` — байты, записанные и отложенные объекты, спавны.

Полный пример: `samples/guide_examples/14-networking/replication.cpp`.

## Шаг 5. Интерполяция чужих объектов

Снапшоты приходят 30 раз в секунду с джиттером, а кадр рисуется 60–144 раза. Поэтому чужие объекты рисуют **в прошлом** — на `interpolationDelay` позади серверного времени, — между двумя известными снапшотами:

```cpp
// при каждом снапшоте (NetObject::onSnapshotApplied):
interp.push({.time = serverTime, .position = pos, .rotation = rot});

// при отрисовке:
const f64 renderTime = client.renderTime();          // serverTime() - interpolationDelay
if (auto t = mirror->interp.sample(renderTime)) {
    transform.position = t->position;                 // кубический Эрмит
    transform.rotation = t->rotation;                 // slerp
    // t->extrapolated == true: новых данных нет, идёт ограниченная экстраполяция (до 250 мс)
}
```

- `client.serverTime()` — локальное время плюс смещение `ClockSync` (оценка в стиле NTP по пингам с минимальным RTT). `client.clock().rtt()` — текущий RTT.
- Если реплицируется скорость, передайте её в `TransformSample::velocity` — она станет касательной сплайна Эрмита, движение будет точнее.
- Задержку можно менять на лету: `client.setInterpolationDelay(0.15)` при плохой сети.

Полный пример: `samples/guide_examples/14-networking/replication.cpp` (тест `ClientInterpolatesRemoteObjects`).

## Шаг 6. Предсказание своего персонажа

Своего персонажа ждать с сервера нельзя — управление будет «ватным». Клиент сразу применяет ввод локально, отправляет его серверу и, получив авторитетное состояние, сверяется:

```cpp
void simulate(MoveState& s, const MoveInput& in, f32 dt) { s.x += in.axis * 5.f * dt; }   // одинаково на обеих сторонах
bool nearlyEqual(const MoveState& a, const MoveState& b) { return std::abs(a.x - b.x) < 1e-3f; }

ClientPrediction<MoveState, MoveInput> predict(simulate, nearlyEqual);
for (int i = 0; i < 10; ++i) predict.applyInput({1.f}, 0.1f);   // мгновенный отклик: x == 5

// Сервер обработал 4 ввода, но его x на 1 больше (толчок от взрыва):
predict.reconcile(4, MoveState{2.f + 1.f});   // откат к серверному состоянию + повтор 6 вводов -> x == 6
```

Через сеть схема такая:

1. **Клиент, каждый тик:** `predict.applyInput(input, dt)`, затем отправить **все** неподтверждённые вводы (`predict.pending()`) одним пакетом по `UnreliableSequenced`. Избыточность дешевле повторной отправки.
2. **Сервер:** `ServerInputQueue::receive(seq, input, dt)` (дубликаты и старьё отбрасываются), затем `process(...)` исполняет вводы по порядку; `lastProcessed()` — до какого `seq` досчитали.
3. **Сервер → клиент:** `(lastProcessed, авторитетное состояние)`.
4. **Клиент:** `predict.reconcile(lastSeq, state)`. Совпало — ничего не происходит; нет — откат и повтор неподтверждённых вводов (`corrections()` растёт).

Свой формат пакета — через `PacketKind::User` и выше, он попадает в `onUserPacket`:

```cpp
constexpr auto kInputPacket = static_cast<PacketKind>(static_cast<u8>(PacketKind::User) + 0);

// клиент
BitWriter w;
w.writeU8(static_cast<u8>(kInputPacket));
w.writeVarU32(static_cast<u32>(predict.pending().size()));
for (const auto& p : predict.pending()) {
    w.writeVarU32(p.seq);
    w.writeF32(p.input.axis);
    w.writeF32(p.dt);
}
client.sendRaw(w.data(), Channel::UnreliableSequenced);

// сервер
server.onUserPacket = [&](PeerId, PacketKind kind, BitReader& r) {
    if (kind != kInputPacket) return;
    const u32 count = r.readVarU32();
    for (u32 i = 0; i < count && i < 64 && r.ok(); ++i) {
        const u32 seq = r.readVarU32();
        const MoveInput in{r.readF32()};
        const f32 dt = r.readF32();
        if (r.ok()) queue.receive(seq, in, dt);
    }
};
// ...после обработки вводов:
server.callClient(peer, "move.ack", queue.lastProcessed(), authoritative.x);
```

Полный пример: `samples/guide_examples/14-networking/prediction.cpp`.

## Шаг 7. Выделенный сервер

`runDedicatedServer` — готовый headless-цикл без окна и рендера: `poll` → ваша функция тика → снапшоты, со сном до следующего тика.

```cpp
std::atomic<bool> stop{false};   // выставьте из обработчика SIGINT/SIGTERM

DedicatedServerConfig cfg;
cfg.server.listen = {"0.0.0.0", 27015};
cfg.server.tickRate = 60.f;
cfg.server.maxClients = 16;
cfg.stopFlag = &stop;

return runDedicatedServer(
    cfg,
    [&](NetServer& server, u32 tick, f64 dt) { world.fixedUpdate(dt); },   // симуляция
    [&](NetServer& server) { server.replication().add(platform); });       // после старта
```

| Поле `DedicatedServerConfig` | Смысл |
| --- | --- |
| `server` | Обычный `NetServerConfig`. |
| `transportFactory` | По умолчанию ENet. Подставьте `MemoryNetwork` для тестов. |
| `maxTicks` | Остановиться после N тиков (0 — до `stopFlag`). |
| `stopFlag` | Атомарный флаг остановки из другого потока или обработчика сигнала. |
| `realtime` | `false` — симулированное время без сна: тесты и «прогон суток за минуту». |

Функция возвращает 0 при чистом завершении — удобно отдавать её результат из `main`.

Полный пример: `samples/guide_examples/14-networking/dedicated_server.cpp`.

## Шаг 8. Тестирование плохой сети

| Инструмент | Что делает |
| --- | --- |
| `MemoryNetwork::setConditions({latency, jitter, loss, duplicate})` | Задержка, джиттер (может переупорядочивать), потери и дубликаты на уровне датаграмм, в обе стороны. Свой мини-протокол надёжности, поэтому семантика каналов как у ENet. |
| `SimulatedTransport(inner, LinkConditions)` | Обёртка над любым транспортом (в том числе ENet): задерживает исходящие пакеты, теряет ненадёжные. Оборачивайте оба конца для симметрии. |
| `EnetTransportConfig::simulatedIncomingLoss` | Настоящий ENet теряет входящие UDP-датаграммы — надёжные каналы реально переотправляют. |
| `server.stats(peer)` / `client.stats()` | RTT, разброс, потери, полоса, счётчики байт и пакетов (`PeerStats`). |

## Типичные ошибки и подводные камни

- **Свойства зарегистрированы в разном порядке** на сервере и клиенте — значения «перепутаются». Регистрируйте в конструкторе, в одном и том же порядке.
- **Доверие клиенту.** RPC и сообщения от клиента — это пожелания. Проверяйте дистанции, перезарядку, права (`from` — кто прислал).
- **Нет ограничения длины** в `readString()` и счётчиков в своих пакетах — злоумышленник пришлёт «строку на 4 ГБ». Передавайте `maxLength`, ограничивайте циклы.
- **`r.ok()` не проверен** — обрезанный пакет прочитается как нули.
- **Позиция за пределами диапазона квантования** (`QuantizedVec3Codec{-1024, 1024, ...}`) зажимается к краю. Подбирайте диапазон под размер мира.
- **`server.tick()` вызывается каждый кадр**, а не с частотой `tickRate`, — трафик вырастет пропорционально FPS.
- **Ввод по `ReliableOrdered`.** Одна потеря задерживает все следующие пакеты. Для ввода — `UnreliableSequenced` с избыточностью.
- **Своего персонажа интерполируют**, а не предсказывают, — управление отстаёт на RTT + `interpolationDelay`.
- **Недетерминированная `simulate`** (разный порядок операций, `rand()` без общего seed) — бесконечные коррекции.
- **`setOwner` после `add`.** Передача владения после спавна пока не поддерживается.
- **`MemoryNetwork` в двух потоках.** Транспорты не потокобезопасны: опрашивайте их из одного потока.
- **ENet и `now`.** ENet живёт по своим часам, но `poll(now)` всё равно нужен корректный монотонный `now` — от него зависят часы и снапшоты.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`net.hpp`](../../engine/net/include/oxwald/net/net.hpp) | Общий заголовок модуля |
| [`transport.hpp`](../../engine/net/include/oxwald/net/transport.hpp) | `ITransport`, `PeerId`, `Channel`, `DisconnectReason`, `PeerStats`, `TransportEvent`, `ListenConfig`, `EnetTransportConfig`, `makeEnetTransport` |
| [`memory_transport.hpp`](../../engine/net/include/oxwald/net/memory_transport.hpp) | `MemoryNetwork`, `MemoryTransportConfig`, `LinkConditions`, `SimulatedTransport` |
| [`bit_stream.hpp`](../../engine/net/include/oxwald/net/bit_stream.hpp) | `BitWriter`, `BitReader`, `bitsRequired`, функции квантования |
| [`serialize.hpp`](../../engine/net/include/oxwald/net/serialize.hpp) | `netHash`, `NetCodec<T>`, `QuantizedFloatCodec`, `QuantizedVec3Codec`, `CompressedQuatCodec`, `RangedIntCodec`, `netWrite`/`netRead` |
| [`messages.hpp`](../../engine/net/include/oxwald/net/messages.hpp) | Концепт `NetMessage`, `MessageRegistry`, `RpcRegistry` |
| [`net_server.hpp`](../../engine/net/include/oxwald/net/net_server.hpp) | `NetServer`, `NetServerConfig`, `PacketKind`, `DedicatedServerConfig`, `runDedicatedServer` |
| [`net_client.hpp`](../../engine/net/include/oxwald/net/net_client.hpp) | `NetClient`, `NetClientConfig`, `ConnectionState` |
| [`replication.hpp`](../../engine/net/include/oxwald/net/replication.hpp) | `NetObject`, `Relevancy`, `ReplicationServer`, `ReplicationClient`, `ReplicationConfig`, `SnapshotStats`, `NetObjectFactory` |
| [`interpolation.hpp`](../../engine/net/include/oxwald/net/interpolation.hpp) | `InterpolationBuffer`, `TransformSample`, `InterpolatedTransform`, `ClockSync` |
| [`prediction.hpp`](../../engine/net/include/oxwald/net/prediction.hpp) | `ClientPrediction<State, Input>`, `ServerInputQueue<Input>` |
| [`net_rpc.hpp`](../../engine/async/net/include/oxwald/async/net_rpc.hpp) | `RpcCall`, `serveRequest` — запрос/ответ с `co_await` (таргет `Oxwald::async_net`) |

Заметки для разработчиков модуля (формат снапшота, замеры, план интеграции с ECS, ограничения): [`docs/dev/modules/net.md`](../dev/modules/net.md).

## Что дальше

- [08. Корутины](08-coroutines.md) — `RpcCall`: запрос к серверу с тайм-аутом через `co_await`.
- [09. Физика](09-physics.md) — контроллер персонажа как функция `simulate` для предсказания.
- [03. ECS и сцены](03-ecs-scene.md) — сущности, чьи компоненты реплицируются через геттеры/сеттеры.
- [05. Runtime и игровой цикл](05-runtime.md) — фиксированный шаг, в котором удобно тикать сервер.
- [15. Скрипты на Lua](15-scripting-lua.md) — как открыть сетевые вызовы скриптам через `bindApi`.
- [Оглавление](README.md).
