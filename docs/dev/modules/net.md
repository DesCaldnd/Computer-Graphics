# Module `net` (`Oxwald::net`, namespace `ox::net`)

Client/server networking: packet transports (ENet over UDP, a deterministic in-memory transport, a lag/loss
simulator), bit-level serialization, typed messages and RPCs, delta-compressed object replication with a per-client
bandwidth budget, snapshot interpolation, client-side prediction with reconciliation, clock sync and a headless
dedicated-server loop. It depends only on `core` (types/log/assert) and glm; ENet is a **private** dependency.
It is entity-agnostic: ECS replication plugs in through `NetObject` getters/setters (see "ECS integration" below).

Umbrella header: `#include <oxwald/net/net.hpp>`.

## Headers

| Header | Contents |
| --- | --- |
| `bit_stream.hpp` | `BitWriter` / `BitReader`: bits, varints (zig-zag), ranged ints, raw/quantised floats & vec3, smallest-three quaternions, strings, `rewind()` |
| `serialize.hpp` | `netHash` (FNV-1a), `NetCodec<T>` defaults (bool/ints/enums/f32/f64/string/glm vec/quat) and quantising codecs `QuantizedFloatCodec`, `QuantizedVec3Codec`, `CompressedQuatCodec`, `RangedIntCodec` |
| `transport.hpp` | `ITransport`, `PeerId`, `Channel`, `DisconnectReason`, `PeerStats`, `TransportEvent`, `makeEnetTransport()` |
| `memory_transport.hpp` | `MemoryNetwork` (+ its transports), `LinkConditions`, `SimulatedTransport` |
| `messages.hpp` | `NetMessage` concept, `MessageRegistry`, `RpcRegistry` |
| `replication.hpp` | `NetObject`, `ReplicatedProperty`, `Relevancy`, `ReplicationServer`, `ReplicationClient`, `NetObjectFactory` |
| `net_server.hpp` / `net_client.hpp` | `NetServer`, `NetClient`, `PacketKind`, `runDedicatedServer()` |
| `interpolation.hpp` | `InterpolationBuffer` (Hermite + slerp), `ClockSync` (NTP-style) |
| `prediction.hpp` | `ClientPrediction<State, Input>`, `ServerInputQueue<Input>` |

## Transports

```cpp
using namespace ox::net;
auto transport = makeEnetTransport({.timeoutMs = 5000});           // real UDP
MemoryNetwork net(/*seed*/ 1);                                       // in-process, deterministic
net.setConditions({.latency = 0.05, .jitter = 0.01, .loss = 0.1f}); // per datagram, both directions
auto a = net.createTransport(), b = net.createTransport();
auto laggy = std::make_unique<SimulatedTransport>(makeEnetTransport(), LinkConditions{.latency = 0.08});
```

* Channels: `ReliableOrdered` (ENet reliable), `UnreliableSequenced` (ENet unreliable: never older than latest),
  `Unreliable` (ENet unsequenced). `MemoryNetwork` implements the same semantics with its own tiny reliable-UDP layer
  (acks, RTO retransmission, reordering buffer), so loss/jitter/duplicates are real at datagram level.
* `poll(now, events)` takes the caller's clock. The memory transport runs entirely on it (tests step simulated time),
  ENet uses its own clock.
* Disconnect payload = `DisconnectReason` (`Timeout = 0` because ENet reports 0 on timeouts, `UserBase` + n for game
  reasons). Connect payload carries the protocol version.
* `PeerStats`: RTT and variance, packet loss, smoothed send/receive bandwidth, byte/packet counters.
* Testing loss on real ENet: `EnetTransportConfig::simulatedIncomingLoss` drops incoming UDP datagrams in ENet's
  intercept hook, so reliable channels genuinely retransmit. `SimulatedTransport` wraps any transport (outgoing
  latency/jitter; drops unreliable packets, models reliable loss as an extra 2×latency delay).

## Bit packing

```cpp
BitWriter w;
w.writeVarU32(entityCount);
w.writeRangedInt(team, 0, 3);                         // 2 bits
w.writeQuantizedFloat(health, 0.f, 100.f, 0.1f);       // 10 bits, error <= 0.05
w.writeQuantizedVec3(pos, -1024.f, 1024.f, 0.01f);     // 18 bits per axis
w.writeQuat(rotation, 10);                             // 32 bits (2 + 3*10), < ~0.0055 rad
BitReader r(w.data());
const u32 n = r.readVarU32(); ...
if (!r.ok()) { /* truncated or malformed: reads past the end return 0 and set the error flag */ }
```

## Server, client, messages, RPCs

```cpp
struct Chat {
    static constexpr std::string_view kNetName = "game.chat";   // id = netHash(kNetName)
    std::string text;
    void serialize(BitWriter& w) const { w.writeString(text); }
    void deserialize(BitReader& r) { text = r.readString(256); }
};

NetServer server(makeEnetTransport(), {.listen = {"0.0.0.0", 27015}, .tickRate = 30.f});
server.start();
server.messages().on<Chat>([&](PeerId from, const Chat& c) { server.broadcast(c); });
server.rpcs().bind("player.fire", [&](PeerId from, glm::vec3 origin, glm::vec3 dir) { /* validate! */ });
server.onClientConnected = [&](PeerId id) { ... };

NetClient client(makeEnetTransport());
client.connect("127.0.0.1", 27015);
client.rpcs().bind("fx.explosion", [](PeerId, glm::vec3 at, f32 radius) { ... });
client.callServer("player.fire", origin, dir);
server.multicast("fx.explosion", at, 5.f);              // all clients
server.multicastRelevant(netId, "fx.explosion", at, 5.f); // only clients that have `netId` spawned

// frame loop (both sides)
server.poll(now);  if (tickDue) server.tick(now);       // tick() sends snapshots
client.poll(now);
```

RPC argument types are deduced from the handler (`PeerId` first); arguments use `NetCodec<T>` — specialise it for
custom types. Unknown/malformed messages are logged and dropped. `PacketKind::User` and above are routed to
`onUserPacket` for custom protocols (e.g. batched inputs).

## Replication

```cpp
struct Crate : NetObject {
    glm::vec3 pos{0}; glm::quat rot{1, 0, 0, 0}; f32 health = 100; std::string label;
    InterpolationBuffer interp;
    Crate() : NetObject("Crate") {
        replicate("pos", pos, QuantizedVec3Codec{-2048, 2048, 0.01f});
        replicate("rot", rot, CompressedQuatCodec{10});
        replicate("health", health, QuantizedFloatCodec{0, 100, 0.5f});
        replicate("label", label);
        setPositionSource([this] { return pos; });
        setRelevancy(Relevancy::Distance, 150.f);
    }
    void onSnapshotApplied(f64 serverTime) override { interp.push({serverTime, pos, rot}); }
};

// server
auto crate = std::make_shared<Crate>();
crate->setOwner(clientId);                       // optional, kServerOwner by default
NetId id = server.replication().add(crate);
server.replication().setViewerPosition(clientId, cameraPos);   // for distance relevancy/priority
server.replication().remove(id);                  // despawn everywhere

// client
client.replication().factory().registerType<Crate>("Crate");
client.replication().onSpawn = [](NetObject& o) { ... };
auto t = static_cast<Crate*>(client.replication().find(id))->interp.sample(client.renderTime());
```

How it works:

* Every tick the server captures each object's state once (`NetState` = immutable shared values, shared by all
  clients). Per client it remembers which state was **acked** (client acks newest snapshot seq + 32-bit history).
* Snapshot entry per object: `netId`, spawn flag (+ type hash + owner), baseline seq, then either the full state or
  a changed-bit mask + changed values against the acked baseline. Equality goes through the codec, so a quantised
  property that moved less than its resolution is not resent. Baselines older than `maxBaselineAge` → full state.
  The client keeps a 64-entry state history per object and decodes deltas against the exact baseline the server
  used (robust to loss/reordering/A→B→A changes).
* Spawns/despawns are re-sent until a packet containing them is acked; NetIds are never reused.
* Relevancy: `Always`, `Distance` (radius from the client's viewer position), `OwnerOnly`, `Custom` filter; the owner
  always receives its objects. Becoming irrelevant despawns on that client; relevant again respawns with full state.
* Budget: `ReplicationConfig::bytesPerTick` per client. Dirty objects add `priority × distanceFactor` (×4 for pending
  spawns) to a per-client accumulator, are written in accumulator order until the budget is hit (written objects
  reset to 0, skipped ones keep accumulating → no starvation). `NetServer::lastSnapshotStats(client)` reports bytes,
  objects written/deferred, spawns/despawns.

Measured in tests: an object with 10 properties costs ~50 B to spawn, an unchanged object 0 B, one changed float
≤ 10 B including id/baseline/mask.

## Interpolation, prediction, clock

* `NetClient::serverTime()` = local time + `ClockSync` offset (lowest-RTT sample of the last 16 pings, slewed).
  `renderTime() = serverTime() - interpolationDelay` (default 100 ms ≈ 3 snapshots at 30 Hz).
* `InterpolationBuffer::sample(renderTime)`: cubic Hermite for positions (replicated velocity as tangent, else
  central differences), slerp for rotations, bounded extrapolation (`maxExtrapolation`, 250 ms).
* Prediction for the locally owned object:

```cpp
ClientPrediction<MoveState, MoveInput> predict(simulate, nearlyEqual);
u32 seq = predict.applyInput(input, dt);       // simulate now, keep (seq, input, predicted state)
sendInputs(predict.pending());                 // unacked inputs, redundantly, Channel::UnreliableSequenced
// on server state for this object: (lastProcessedInputSeq, authoritative state)
predict.reconcile(lastSeq, authoritative);     // mismatch -> rewind to authoritative + replay pending inputs

ServerInputQueue<MoveInput> queue;             // server, per client: dedupes, orders, tracks lastProcessed()
queue.receive(seq, input, dt);
queue.process([&](const MoveInput& in, f32 dt) { simulate(state, in, dt); });
```

## Dedicated server

```cpp
std::atomic<bool> stop{false};   // set from SIGINT handler
DedicatedServerConfig cfg{.server = {.listen = {"0.0.0.0", 27015}, .tickRate = 60.f}, .stopFlag = &stop};
return runDedicatedServer(cfg, [&](NetServer& server, u32 tick, f64 dt) { world.fixedUpdate(dt); });
```

No window, no renderer: poll → `tickFn` → snapshots, sleeping to the tick rate (`realtime = false` runs on simulated
time for soak tests; `transportFactory` swaps ENet for a `MemoryNetwork`).

## ECS integration plan (for the `gameplay` integration agent)

* `NetworkIdentityComponent { NetId netId; PeerId owner; std::string netType; Relevancy relevancy; f32 relevancyRadius;
  f32 priority; bool serverOnly; std::shared_ptr<NetObject> object; }` — a system creates a plain `NetObject(netType)`
  per entity on the server and calls `ReplicationServer::add`; on clients `NetObjectFactory::registerType(netType, …)`
  creates the entity (prefab by type) and returns the `NetObject`.
* Component replication: for every reflected component marked `attr::Replicated` (field-level), register
  `object.replicate<T>(component.field, getter, setter)` where getter/setter read/write the component in the
  `World`. Field attributes map to codecs (Range + step → `QuantizedFloatCodec`, quat → `CompressedQuatCodec`).
  Properties must be registered in a deterministic order (sorted by component name, then field order).
* `NetworkTransformComponent { bool interpolate = true; bool predicted; f32 positionResolution = 0.01f;
  u32 rotationBits = 10; bool syncScale = false; InterpolationBuffer buffer; }` — server replicates Transform
  position/rotation (+velocity if a physics body exists); remote clients push into `buffer` from
  `onSnapshotApplied` and a `PreUpdate` system writes `buffer.sample(client.renderTime())` into the Transform.
  Owned + `predicted` entities run `ClientPrediction` with the character controller as `simulate`.
* Systems: `NetServerSystem` (poll every frame, `tick` in `FixedUpdate` at the server tick rate, viewer positions
  from player cameras), `NetClientSystem` (poll, send inputs, apply interpolation), both registered as services.

`oxwald/net/net_harness.hpp`: `ox::net::MemoryHarness` (server + client over a deterministic `MemoryNetwork` with
simulated time: `startAndConnect()`, `step()`, `run(seconds)`, `runUntil(pred, seconds)`) for game/gameplay tests;
`pumpReal()` for ENet tests in real time.

Containers replicate with the default codecs `NetCodec<std::vector<T>>`, `NetCodec<std::map<std::string, T>>` and
`NetCodec<std::unordered_map<std::string, T>>` (count + elements, sorted keys, max 65536 elements per container); a
change resends the whole container. Gameplay replicates reflected `attr::Replicated` arrays/maps/optionals as one
OXB1 blob per field (entity references inside containers keep their UUIDs).

## Known limits / TODO

* Ownership transfer after spawn and per-property conditions (owner-only / skip-owner) are not implemented yet.
* Messages are one per transport packet (ENet coalesces datagrams; no custom packing/fragmentation).
* `MemoryNetwork` endpoints share one clock (the clock-offset path is tested in `ClockSync` unit tests).
* No encryption/authentication; the version check is the only handshake. No NAT punch-through / lobby.
* `SimulatedTransport` delays only outgoing traffic; wrap both ends for symmetric conditions.
