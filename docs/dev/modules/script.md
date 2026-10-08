# Module `script` (`Oxwald::script`, namespace `ox::script`)

Lua 5.4 (vcpkg `lua` 5.4.8) + sol2 3.5 scripting: a sandboxed VM, script assets with declared editor properties,
a per-entity instance lifecycle, hot reload, glm math bindings, logging, coroutines/timers on script time, an event
bus bridged to C++ and a `bindApi` extension point for other modules. Depends on `core` (types/log/assert) + glm;
lua and sol2 are public dependencies (`bindApi` builders receive `sol::table`). Compile definitions
`SOL_ALL_SAFETIES_ON=1` are exported. No ECS dependency: the integration layer drives `ScriptInstance`.

## Headers

| Header | Contents |
| --- | --- |
| `oxwald/script/script_vm.hpp` | `ScriptVMConfig`, `ScriptVM`, `ScriptResult` |
| `oxwald/script/script_instance.hpp` | `ScriptAsset`, `ScriptInstance` |
| `oxwald/script/script_value.hpp` | `ScriptValue`, `ScriptPropertyType`, `ScriptPropertyDesc`, coercion helpers |
| `oxwald/script/script_events.hpp` | `ScriptEventBus`, `Scheduler` |
| `oxwald/script/sol_glm.hpp` | sol2 traits for glm types (included by `script_vm.hpp`; include it before moving glm values through sol) |

## VM and sandbox

```cpp
using namespace ox::script;
ScriptVM vm({.memoryLimit = 64 << 20, .instructionLimit = 10'000'000, .searchRoots = {"assets/scripts"}});
sol::environment env = vm.createEnvironment();
ScriptResult r = vm.runString("return vec3(1, 2, 3):length()", "console", &env);
if (r) { f32 len = r.value.as<f32>(); } else { /* already logged: r.error has chunk:line + traceback */ }
```

* Each script instance/module/console snippet gets its own environment: whitelisted base functions, read-only
  per-environment proxies of `math`, `string`, `table`, `utf8`, `coroutine`, `os` (`clock/time/date/difftime` only)
  and of every API table. Not available: `io` (unless `allowIo`; to be replaced by a VFS API), `os.execute/exit/
  remove/getenv…`, `load/loadfile/dofile`, `debug`, `package`, `collectgarbage`, `string.dump`; `getmetatable` returns
  nil for userdata and `false` for strings, so shared metatables cannot be modified. Binary chunks are never loaded.
* All C++ → Lua calls go through `ScriptVM::call()` / `run*()`: errors (including C++ exceptions thrown by bindings)
  are caught, logged to `OX_LOG_ERROR("script", "<context>: <chunk>:<line>: msg\nstack traceback…")`, counted
  (`errorCount()`, `lastError()`), never propagated.
* Memory limit: custom allocator; over the limit Lua raises "not enough memory" (caught as above).
* Instruction limit: count hook every `hookInterval` instructions, budget per top-level call (lifecycle call, event,
  timer, coroutine resume). Once tripped it stays tripped until the call returns — sandbox `pcall`, `xpcall` and
  `coroutine.resume` re-raise it, so scripts cannot swallow the abort.

## Script assets, properties, instances

```cpp
auto asset = vm.loadScript("assets/scripts/player.lua");   // cached by path
for (const ScriptPropertyDesc& p : asset->properties()) {  // editor inspector, no instance needed
    // p.name, p.type (Float/Int/Bool/String/Vec2/Vec3/Vec4/Color), p.defaultValue, p.min, p.max, p.tooltip, p.order
}
auto inst = vm.createInstance(asset, [&](ScriptInstance&, sol::table& self) { self["entity"] = entityHandle; });
inst->setProperty("speed", 12.0);       // per-instance override, coerced + clamped to min/max
inst->create();                          // runs the chunk, fills self from defaults/overrides, onCreate(self)
inst->update(dt);                        // onStart(self) once, then onUpdate(self, dt)
inst->fixedUpdate(fixedDt);
inst->sendEvent("hit", ScriptValue{25.0});
inst->destroy();                         // onDestroy(self); cancels its coroutines/timers/subscriptions
vm.update(dt);                           // script time, timers, coroutines, periodic hot-reload poll
```

Property declaration forms: full table (`type`, `default`, `min`, `max`, `tooltip`, `order`) or shorthand
(`godMode = false`, `speed = 5` → float). Vector/color defaults accept `vec3(…)` or `{1, 0.5, 0, 1}`. Properties are
sorted by `(order, name)`. Property values live in `self` — the per-instance state table.

## Hot reload

`vm.pollHotReload()` (also called from `update()` every `hotReloadInterval` s) checks file mtimes. On change the new
source is compiled first — a syntax error logs and keeps the old version. Otherwise every instance gets a fresh
environment with the new chunk, keeps its `self` table (all fields persist), receives defaults for newly declared
properties and `on_reload(self)` (or `onReload`) is called. A changed `require`d module invalidates the module cache
and reloads all file scripts. Coroutines/timers/subscriptions started by the old code keep their old closures.

`ScriptVM::reloadScript(asset, source)` reloads a string-backed script (asset database, network) the same way
(compile check first, then every live instance reloads in place); gameplay uses it for asset hot reload.

## Lua API

| Global | |
| --- | --- |
| `vec2`, `vec3`, `vec4` | `vec3(x,y,z)`, `vec3(s)`, `.x/.y/.z`, `+ - * / unary-`, `==`, `tostring`, `:length() :lengthSquared() :normalize() :dot(b) :distance(b) :lerp(b,t) :min/max/abs() :clone() :unpack()`, `vec3:cross :reflect`, constants `vec3.zero/one/up/right/forward` (−Z) |
| `quat` | `quat()` identity, `quat(w,x,y,z)`, `quat.fromEuler(vec3|p,y,r)`, `fromEulerDegrees`, `:toEuler()`, `angleAxis(rad, axis)`, `lookRotation(fwd[,up])`, `fromTo(a,b)`, `q*q`, `q*v`, `:normalize :inverse :conjugate :dot :slerp :lerp :angle :axis :rotate :forward :up :right` |
| `mat4` | `mat4()`, `identity()`, `translation(v)`, `rotation(q)`, `scaling(v)`, `trs(t,r,s)`, `lookAt`, `perspective`, `m*m`, `m*vec4`, `:inverse :transpose :transformPoint :transformVector :getTranslation :get(c,r) :set(c,r,v)` |
| `math` | Lua math + `clamp lerp inverseLerp remap smoothstep sign round approximately` |
| `log` | `log.trace/debug/info/warn/error(...)` → engine log, prefixed with `chunk:line:`; `print` = `log.info` |
| `spawn(fn, ...)` / `wait(s)` / `time()` | coroutine scheduler on script time; `wait` only inside `spawn`ed coroutines |
| `timer` | `timer.after(s, fn)`, `timer.every(s, fn)` → id, `timer.cancel(id)` |
| `events` | `events.subscribe(name, fn(name, payload))` → id, `events.unsubscribe(id)`, `events.publish(name, payload)` |
| `require(name)` | `a.b` → `<root>/a/b.lua` or `<root>/a/b/init.lua`; modules run in their own sandbox, results cached |

## Sample script

`engine/script/tests/data/sample_player.lua` (exercised by the tests):

```lua
local util = require("util.mathx")

properties = {
    speed     = { type = "float", default = 5, min = 0, max = 20, tooltip = "Units per second", order = 0 },
    maxHealth = { type = "int", default = 100, min = 1, max = 1000, order = 1 },
    tint      = { type = "color", default = { 1, 0.5, 0, 1 } },
    spawnAt   = { type = "vec3", default = vec3(0, 1, 0) },
    godMode   = false,
}

function onCreate(self)
    self.health = self.maxHealth
    self.position = self.spawnAt:clone()
    self.ticks = 0
    events.subscribe("player.damage", function(name, payload)
        if not self.godMode then self.health = math.max(0, self.health - payload.amount) end
    end)
end

function onStart(self)
    spawn(function()
        wait(1.0)
        self.warmedUp = true
        log.info("player warmed up at", time())
    end)
end

function onUpdate(self, dt)
    self.ticks = self.ticks + 1
    self.position = self.position + vec3.forward * (self.speed * dt)
    self.distance = util.round2(self.position:distance(self.spawnAt))
end

function onEvent(self, name, payload)
    if name == "heal" then self.health = math.min(self.maxHealth, self.health + payload) end
end

function onDestroy(self) events.publish("player.died", { ticks = self.ticks }) end

function on_reload(self) log.info("reloaded, health kept:", self.health) end
```

## Events from C++

```cpp
vm.events().subscribe("player.died", [](std::string_view, const sol::object& payload) {
    auto ticks = payload.as<sol::table>()["ticks"].get<i64>();
});
vm.events().publish("player.damage", ScriptValue{25.0});          // plain values
sol::table t = vm.lua().create_table(); t["amount"] = 30;
vm.events().publish("player.damage", t);                           // tables
```

Bridge to the engine-wide event system: subscribe in C++ to the engine event and `publish` here (and vice versa).

## Extension point: `bindApi`

```cpp
vm.bindApi("physics", [&world](sol::state_view lua, sol::table& api) {
    api["raycast"] = [&world](glm::vec3 from, glm::vec3 dir, f32 maxDist) -> sol::optional<glm::vec3> { ... };
    api["gravity"] = -9.81;
});
// Lua: local hit = physics.raycast(pos, vec3(0, -1, 0), 100)
```

The table is built once in the main state; every sandbox (also ones created earlier) sees a read-only proxy.
Throwing from a binding is safe (sol2 turns it into a Lua error, which is then logged with a traceback).

## ECS integration plan (for the `gameplay` integration agent)

* `ScriptComponent { AssetRef<ScriptAsset> script; std::unordered_map<std::string, ScriptValue> overrides
  (reflected, editor-editable from asset->properties()); bool enabled = true; std::unique_ptr<ScriptInstance>
  instance (runtime only); }`. Several scripts per entity → `ScriptListComponent` with a vector of these.
* `ScriptSystem`: on component add → `vm.createInstance(asset, init)` where `init` puts `self.entity` (Entity
  userdata) into `self`, apply overrides, `create()`; `PreUpdate`: `vm.update(dt)`; `Update`: `instance->update(dt)`;
  `FixedUpdate`: `fixedUpdate`; on remove/scene unload → `destroy()`. Play-in-editor: instances only exist in play mode.
* Lua entity API (`bindApi("scene", …)` + an `Entity` usertype): `self.entity:get("Transform")` returns a reflected
  component proxy (field get/set through `ox::reflect`, glm fields as vec3/quat), `entity:has/add/remove(name)`,
  `entity.transform.position/rotation/scale`, `entity:children()`, `entity:destroy()`, `scene.find(name)`,
  `scene.spawn(prefab, pos, rot)`, `entity:sendEvent(name, payload)` → `ScriptInstance::sendEvent` on all scripts.
  Generic component access driven by reflection means no per-component binding code.
* Other modules: `physics.raycast/overlap`, `audio.play`, `input.isDown/axis`, `net.callServer` through `bindApi`.

## Built-in `cvar` API and `bindApi` reach
`cvar.get(name)` (typed: bool/number/string, nil when unknown), `cvar.getString`, `cvar.set(name, value)` (bool,
number or string; `CVarSource::Console` permissions), `cvar.exists`, `cvar.reset`, `cvar.description`. `log.*` and
`print` honour `log::minLevel()`. `bindApi` also reaches sandboxes created before the call (the VM keeps a weak-keyed
set of environments); a script global with the same name wins unless the API is being re-bound.

## Known limits / TODO

* `io` is either fully off or fully on; the VFS-restricted file API is pending (core VFS).
* Instruction budget is per top-level call, not per frame/instance; memory limit is per VM, not per script.
* Module hot reload reloads every file script (no dependency tracking yet).
* Coroutines yield across `pcall` fine, but not across C++ callbacks (e.g. inside an `events` handler).
* No debugger/profiler hooks yet (a `debug` library for the editor build could be added behind a config flag).

## Async bridge (optional, with the `async` module)

When `async` is configured, `engine/script/async/async_bridge.cpp` is compiled into `ox_script` (which then links
`Oxwald::async`, `OX_SCRIPT_HAS_ASYNC=1`). `ox::script::AsyncBridge` (`oxwald/script/async_bridge.hpp`) exposes
`ox::Future<T>` to Lua (`local v = await(f)` inside `spawn`ed coroutines, `Future:isReady/hasError/error/get`) and lets
C++ coroutines `co_await` script functions (`bridge.call(fn, ...)`, `bridge.invoke(instance, "fn", ...)`). Call
`bridge.update()` before `vm.update(dt)`. To park coroutines without polling, `Scheduler` gained `currentTaskId()` and
`wake(id)` (a task yielding `math.huge` sleeps until woken). Details and examples: `docs/dev/modules/async.md`.
