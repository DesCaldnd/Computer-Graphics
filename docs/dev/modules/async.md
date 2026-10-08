# Module `async` (`Oxwald::async`, namespace `ox`)

C++20 coroutines for gameplay code: timers, waiting for animations/events, asset loading, network replies and
background computations written as straight-line code. Depends on `core` only. Optional extras:

* `Oxwald::async_net` (header-only, `engine/async/net/`) — request/response RPCs over `net` (`RpcCall`, `serveRequest`).
* Lua bridge in the `script` module (`engine/script/async/`, header `oxwald/script/async_bridge.hpp`), compiled
  automatically when `async` is configured (`OX_SCRIPT_HAS_ASYNC=1`).

Umbrella header: `#include <oxwald/async/async.hpp>`.

## Headers

| Header | Contents |
| --- | --- |
| `task.hpp` | `Task<T>` (lazy, move-only, symmetric transfer), `toTask(awaitable)` |
| `future.hpp` | `Promise<T>`, `Future<T>`, `AsyncError`, `BrokenPromise`, `runAsync`, `awaitCallback`, `makeReadyFuture`, `makeErrorFuture` |
| `scheduler.hpp` | `CoroutineScheduler`, `CoroutineHandle`, `SpawnOptions`, `CoroutineInfo`, awaitables (`nextFrame`, `frames`, `seconds`, `realSeconds`, `nextFixedUpdate`, `until`, `whileTrue`, `event`, `mainThread`, `backgroundThread`, `switchTo`, `named`, `currentCancellation`, `currentScheduler`, `spawnChild`) |
| `combinators.hpp` | `whenAll`, `whenAny` (variadic + vector), `timeout`, `realTimeout` |
| `cancellation.hpp` | `CancellationSource`, `CancellationToken`, `CancellationRegistration` |
| `executor.hpp` | `IExecutor`, `ThreadPoolExecutor`, `InlineExecutor`, `JobSystemExecutor` (core `JobSystem` adapter) |
| `generator.hpp` | `Generator<T>` (synchronous, range-for) |
| `detail/frame_pool.hpp` | `coroutineFrameStats()` (live frames, pool/heap allocation counters) |

## Core concepts

```cpp
ox::Task<int> countdown(int n) {
    for (int i = n; i > 0; --i) { co_await ox::seconds(1.0); }
    co_return n;
}

ox::CoroutineScheduler sched;                                      // constructing thread = scheduler (main) thread
ox::CoroutineHandle h = sched.spawn(countdown(3), {.name = "Countdown", .owner = entityId});
// every frame:
sched.tick(dt, frameIndex);       // dt = real seconds; game time = dt * timeScale unless paused
sched.fixedTick(fixedDt);         // per fixed step (resumes nextFixedUpdate())
h.status(); h.isDone(); h.cancel();
```

* **Task<T>** is lazy: nothing runs until awaited or spawned. `spawn` runs the task synchronously until its first
  suspension (Unity `StartCoroutine` semantics; `SpawnOptions::deferStart` starts it at the next tick).
  `spawn(lambda)` keeps the lambda (and its captures) alive in the frame — never write
  `sched.spawn([&]() -> Task<> {...}())` (the closure would die immediately).
* **Exceptions** propagate to the awaiter (`co_await` rethrows). A root task that throws ends as
  `CoroutineStatus::Failed` with `handle.error()` and an error log. Prefer returning `ox::Result<T>` for expected
  failures; exceptions stay inside coroutine code (no module boundary).
* **Awaitables**: `nextFrame()`, `frames(n)` (ticks), `seconds(s)` (game time: time scale + pause; always at least
  one tick), `realSeconds(s)`, `nextFixedUpdate()`, `until(pred)`, `whileTrue(pred)` (checked once per tick),
  `event(signal)` (next emission of an `ox::Signal`, returns its args), any `Future<T>` or `Task<T>`.
* **Futures**: `Promise<T>` completes from any thread; a coroutine that awaited on the scheduler thread is resumed by
  that scheduler during its next tick — never on the completing thread. Awaited from a background thread (or outside
  a scheduler) it resumes inline on the completing thread. A destroyed `Promise` fails the future with `BrokenPromise`.
* **Callback APIs in one line**:
  `Mesh m = co_await ox::awaitCallback<Mesh>([&](auto resume) { assets.loadAsync(path, resume); });`
  (`resume` is copyable/thread-safe; if all copies die uncalled the await fails with `BrokenPromise`).

### Threading

```cpp
co_await ox::backgroundThread();     // scheduler's background executor (JobSystemExecutor in the runtime)
auto path = navmesh.findPath(a, b);  // heavy work off the game thread
co_await ox::mainThread();           // back on the scheduler thread, resumed during the next tick
Future<int> f = ox::runAsync(executor, [] { return work(); });   // plain function on an executor
co_await ox::switchTo(myExecutor);
```

Scheduler awaitables (`seconds`, `frames`, `event`, ...) assert when used off-thread: hop back first. A `whenAll`
started on a background thread joins its branches on the main thread.

### Combinators and timeouts

```cpp
auto [mesh, tex] = co_await ox::whenAll(loadMesh(a), loadTexture(b));     // void -> std::monostate
std::vector<Path> paths = co_await ox::whenAll(std::move(pathTasks));
auto r = co_await ox::whenAny(playerReachedExit(), ox::toTask(ox::seconds(30)));  // r.index, r.value (variant)
std::optional<Reply> reply = co_await ox::timeout(rpc(req), 2.0);         // nullopt on timeout (game time)
bool arrived = co_await ox::realTimeout(ox::toTask(ox::until(near)), 5.0);
```

`whenAll` rethrows the first branch exception once all branches finished. `whenAny` returns as soon as one branch
finishes; the **losers are cancelled and unwound before it returns** (RAII destructors run, timers unregistered,
future continuations detached). A loser running on a background thread is torn down when it returns to the main
thread — poll `co_await ox::currentCancellation()` in long background loops.

### Cancellation

* `handle.cancel()` (thread-safe), `scheduler.cancelOwner(owner)`, `scheduler.cancelAll()`,
  `SpawnOptions::token` (`CancellationSource`), `SpawnOptions::parent` / `co_await ox::spawnChild(task)` (children
  are cancelled with their parent and inherit its owner).
* A suspended task is unwound immediately: its frame chain is destroyed, so every local, awaited sub-task and
  whenAll/whenAny branch runs its destructors; waits are unregistered; futures detached. A task cancelling itself
  (or cancelled while running) is unwound at its next suspension point. Code after the cancelled `co_await` never
  runs.
* While any part of a task runs on a background thread, destruction is deferred until it comes back to the main
  thread (`co_await mainThread()` / branch completion); `currentCancellation().isCancelled()` turns true
  immediately so loops can bail out.

### Debugging / introspection

* Names: `SpawnOptions::name` or `co_await ox::named("OpenDoor")`.
* `scheduler.coroutines()` → `CoroutineInfo{id, name, owner, parentId, state (Pending/Suspended/Running/OffThread/
  CancelPending), waitingOn ("seconds(0.42 left)", "future", "event", "frames(2 left)", "background", several
  entries for whenAll branches), ageSeconds, ageRealSeconds, ageFrames}` — data for the editor "Coroutines" panel.
* Leak report: `shutdown()` (also run by the destructor) logs every still-running coroutine (`[warn] [async]`), then
  cancels them; parts stuck on background threads are waited for (≤ 2 s) and otherwise reported and leaked.
* `coroutineFrameStats()`: live frames, pooled vs heap allocations, pool chunks.

### Allocation

Coroutine frames and task records use `operator new` overloads on the promise backed by a thread-local
size-class pool (64-byte classes up to 2 KiB, 64 KiB chunks never returned to the OS). The pool is compiled out
under AddressSanitizer or with `-DOX_ASYNC_NO_FRAME_POOL` so ASan sees every frame.

Measured (AppleClang 17, RelWithDebInfo, M-series, `AsyncStress.HundredThousandTasksNoLeaks`): spawning 100k tasks
(each `worker` → `leaf` with `nextFrame` + `frames(n)`) takes ~14–16 ms (≈140–160 ns/task) with **0 heap
allocations** once the pool and scheduler vectors are warm; running them to completion takes ~35 ms over 3 ticks.
Cancelling 100k suspended `whenAny(future, seconds)` tasks by owner frees every frame (live-frame count back to
baseline). The whole suite also passes under ASan+UBSan and TSan.

## Gameplay examples

### Door opening sequence (animation wait + sound + timer)

```cpp
ox::Task<> openDoor(DoorCtx& ctx, ox::Entity door) {
    co_await ox::named("OpenDoor");
    ctx.animator.play(door, "Open");
    ctx.audio.play3D("sfx/door_creak", ctx.positionOf(door));
    co_await ox::event(ctx.animator.onFinished(door));          // Signal<std::string_view clip>
    ctx.physics.setCollisionEnabled(door, false);
    co_await ox::seconds(5.0);                                   // stays open 5 s of game time (pause-aware)
    co_await ox::until([&] { return !ctx.triggers.isOccupied(door); });
    ctx.physics.setCollisionEnabled(door, true);
    ctx.animator.play(door, "Close");
    co_await ox::event(ctx.animator.onFinished(door));
}
sched.spawn(openDoor(ctx, door), {.owner = door.id()});         // destroyed door -> cancelOwner(door.id())
```

### Async asset load with progress

```cpp
ox::Task<> loadLevel(Game& g, std::string level) {
    ox::CancellationCheck c = co_await ox::currentCancellation();
    std::vector<ox::Task<MeshHandle>> loads;
    for (const auto& path : g.manifest(level)) {
        loads.push_back(ox::toTask(ox::awaitCallback<MeshHandle>([&, path](auto resume) {
            g.assets.loadAsync(path, resume);                    // completes on an IO thread
        })));
    }
    const usize total = loads.size();
    usize done = 0;
    auto progress = [&]() -> ox::Task<> {
        while (done < total) { g.ui.setProgress(f32(done) / f32(total)); co_await ox::nextFrame(); }
    };
    auto counted = [&](ox::Task<MeshHandle> t) -> ox::Task<MeshHandle> { auto m = co_await std::move(t); ++done; co_return m; };
    std::vector<ox::Task<MeshHandle>> wrapped;
    for (auto& t : loads) wrapped.push_back(counted(std::move(t)));
    auto [meshes, _] = co_await ox::whenAll(ox::whenAll(std::move(wrapped)), progress());
    g.world.instantiate(level, meshes);
}
```

### NPC dialogue awaiting the player's choice

```cpp
ox::Task<> talk(Npc& npc, DialogueUi& ui) {
    co_await ox::named("Dialogue:" + npc.name);
    ui.show(npc.line("greeting"), {"Trade", "Quest", "Bye"});
    auto r = co_await ox::whenAny(ox::toTask(ox::event(ui.onChoice)),          // Signal<int>
                                  ox::toTask(ox::realSeconds(30.0)));         // idle timeout
    const int choice = r.index == 0 ? std::get<0>(r.value) : 2;
    if (choice == 1) {
        ui.show(npc.line("quest"), {"Accept", "Decline"});
        if (co_await ox::event(ui.onChoice) == 0) npc.giveQuest();
    }
    ui.hide();
}
```

### Network request with timeout

```cpp
// server (once)
ox::net::serveRequest<Inventory, u32>(server, "inv.fetch", [&](ox::net::PeerId from, u32 slot) { return db.load(from, slot); });
// client: member RpcCall<Inventory, u32> m_fetch{client, "inv.fetch"};
ox::Task<> openInventory(Hud& hud, ox::net::RpcCall<Inventory, u32>& fetch) {
    hud.spinner(true);
    std::optional<Inventory> inv;
    try {
        inv = co_await ox::realTimeout(fetch(0), 3.0);
    } catch (const ox::AsyncError& e) { hud.toast(e.what()); }   // server-side exception or not connected
    hud.spinner(false);
    if (inv) hud.showInventory(*inv); else hud.toast("Server did not answer");
}
```

### Background pathfinding, then apply on the main thread

```cpp
ox::Task<> repath(AiAgent& agent, const NavMesh& nav, glm::vec3 goal) {
    const glm::vec3 from = agent.position();                     // read game state on the main thread
    co_await ox::backgroundThread();
    std::vector<glm::vec3> path = nav.findPath(from, goal);      // heavy, thread-safe query
    auto cancel = co_await ox::currentCancellation();
    if (!cancel.isCancelled()) path = smooth(path);
    co_await ox::mainThread();                                   // resumed during the next tick
    agent.setPath(std::move(path));                              // safe: back on the game thread
}
```

## Lua bridge (`script` module)

```cpp
ox::script::AsyncBridge bridge(vm);                            // registers `await`, `async.await`, Future usertype
vm.bindApi("assets", [&](sol::state_view, sol::table& api) {
    api["load"] = [&](std::string path) { return bridge.wrap(assetDb.loadAsync(path)); };   // Future<T> -> Lua
});
// per frame, main thread:
bridge.update();   // wakes script coroutines whose futures completed (any thread)
vm.update(dt);
```

```lua
spawn(function()
    local mesh = await(assets.load("props/door.glb"))   -- parks the coroutine (no polling); errors raise
    local ok, err = pcall(function() return await(assets.load("missing")) end)
end)
-- Future methods: f:isReady(), f:hasError(), f:error(), f:get()
```

C++ awaiting Lua: `sol::main_object r = co_await bridge.call(fn, args...)`,
`co_await bridge.invoke(instance, "openDoor", 45.0)` (runs `openDoor(self, 45)` as a script coroutine owned by the
instance — it may `wait()`/`await()`), `ScriptValue v = co_await bridge.callValue(fn)`. Lua errors fail the future
with `ox::AsyncError`. Supported value types for `wrap`: void, bool, numbers, `std::string`, glm vec2/3/4/quat,
`ScriptValue`, `sol::object` (tables) and anything sol2 can push. sol objects must only be touched on the main
thread (await these futures from the scheduler thread).

## Integration (runtime / ECS / gameplay)

1. **Ownership**: the runtime owns one `CoroutineScheduler` (game thread) and registers it in `ox::Services`
   (`services.add<ox::CoroutineScheduler>(...)`). Construct it on the game thread (or call `bindToCurrentThread()`).
   Pass `new JobSystemExecutor(jobs)` as background executor (keep the executor alive as long as the scheduler).
2. **Ticking** (game loop): after input/event dispatch and before `Update` systems run gameplay —
   `PreUpdate`: `bridge.update(); vm.update(dt); scheduler.tick(dt, frameIndex);`; inside the fixed-step loop, after
   the physics step: `scheduler.fixedTick(fixedDt)`. Pausing the game → `scheduler.setPaused(true)`; slow motion →
   `setTimeScale(s)` (real-time waits keep running).
3. **Entity destruction**: call `scheduler.cancelOwner(entity.id())` (and `vm.releaseOwner(...)` for scripts) from
   the entity-destroy hook / `World` on-destroy signal, *before* components are freed, so coroutine destructors may
   still touch the entity. Owner ids are any non-zero `u64` (entity id or packed handle).
4. **Scene unload / shutdown**: `scheduler.cancelAll()` on scene change; on exit destroy the scheduler before the job
   system (it logs leaked coroutines).
5. **C++ gameplay components**: give `ox::ISystem`s / components a helper
   `CoroutineHandle startCoroutine(Entity e, Task<> t, std::string name)` →
   `scheduler.spawn(std::move(t), {.name = name, .owner = e.id()})`; `stopCoroutine(handle)` → `handle.cancel()`;
   `stopAllCoroutines(e)` → `cancelOwner(e.id())`.
6. **ScriptComponent**: create one `AsyncBridge` per `ScriptVM`. Scripts already have `spawn`/`wait`/`await`;
   `startCoroutine` from C++ into a script = `bridge.invoke(*instance, "fn", args...)` (owned by the instance, so
   `instance->destroy()` stops it). Expose engine async APIs to Lua by returning `bridge.wrap(future)` from
   `bindApi` functions.
7. **Editor**: the "Coroutines" panel polls `scheduler.coroutines()` each frame (name, owner → entity name,
   state, waitingOn, age) and offers a Cancel button (`CoroutineHandle::cancel` via `handlesForOwner`).

## Known limits / TODO

* `whenAny` outside a scheduler cannot destroy losers (they may be running on another thread); it waits for them.
* Lua `await` is resumed one script update after the C++ future completes (bridge wake → next `vm.update`).
  A script coroutine cancelled while parked leaves a no-op continuation on the future until it completes.
* `bridge.invoke` on a destroyed instance: the future fails with `BrokenPromise` only when Lua collects the
  resolver closures (next GC cycle), not immediately.
* `RpcCall` keeps timed-out requests until a reply or `failAll()`; call `failAll` on disconnect.
* No per-owner time scale (e.g. a slowed-down character) — would need a per-owner clock in `WaitNode`.
* Scheduler awaitables cannot be used from background threads (assert); there is no implicit hop.
