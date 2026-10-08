# core (`Oxwald::core`)

Foundation library: everything other modules share. Deps: glm, nlohmann-json (public), enkiTS (private),
Tracy (public, optional — only when `OX_ENABLE_TRACY=ON` *and* the package is installed).

## Basics
- **types.hpp / log.hpp / assert.hpp** — `u32`, `f32`…; `OX_LOG_INFO("cat", "fmt {}", x)`; `OX_ASSERT(cond, "fmt", ...)`.
- **result.hpp** — `ox::Result<T>` / `ox::Status` (= `Result<void>`), `ox::Error{message}`, `ox::makeError("fmt {}", x)`.
  Own type because the engine builds as C++20 (no `std::expected`).
  ```cpp
  ox::Result<Mesh> load(...) { if (!ok) return ox::makeError("bad file {}", path); return mesh; }
  ```
- **hash.hpp** — `fnv1a64` (constexpr), `"name"_hash`, `hashCombine`, `crc32` (IEEE).
- **uuid.hpp** — `ox::Uuid{hi, lo}`: `generate()` (v4), `fromName()`, `parse()`, `toString()`, `std::hash`.
- **services.hpp** — DI container: `add<I>(unique_ptr<Impl>)`, `emplace<T>(args)`, `addExternal<T>(ref)`, `get/tryGet/has/remove<T>`.
  Destroyed in reverse registration order.

## Reflection (`reflect.hpp`)
Process-wide `TypeRegistry`. Built-in support: bool, all ints, f32/f64, `std::string`, glm vec2/3/4, ivec2/3/4, quat,
mat4, `Uuid`, enums, `std::vector<T>`, `std::optional<T>`, `std::map/unordered_map<std::string, T>`, nested reflected
structs, base classes (`.base<B>()`), and custom leaves (`registerCustomLeaf<T>(name, tag, toValue, fromValue)` —
scene's `EntityRef` uses it). Attributes (`ox::attr::`): `DisplayName, Tooltip, Range{min,max}, Step, Color{hdr},
AssetRef{"Mesh"}, Hidden, ReadOnly, Category, SaveGame, Replicated, NoSerialize, FormerName{"old"}, Meta{key,value}`.

**Registration rule:** every module has an explicit `registerXxxTypes()` called from its init (static initialisers in
static libraries are dropped by the linker). Registration is idempotent.
```cpp
void registerPhysicsTypes() {
    OX_REFLECT_ENUM(BodyType, "BodyType").value("Static", BodyType::Static).value("Dynamic", BodyType::Dynamic);
    OX_REFLECT_TYPE(RigidBodyComponent, "RigidBody")
        .attributes(ox::attr::Category{"Physics"}, ox::attr::Meta{"icon", "cube"})
        .field("type", &RigidBodyComponent::type)
        .field("mass", &RigidBodyComponent::mass, ox::attr::Range{0.0, 10000.0}, ox::attr::SaveGame{})
        .field("velocity", &RigidBodyComponent::velocity, ox::attr::Replicated{}, ox::attr::NoSerialize{});
    ox::ComponentRegistry::instance().add<RigidBodyComponent>();   // scene module, see scene.md
}
```
Generic access for the editor/undo: `reflect::ValueRef` + `resolvePath(ValueRef::of(obj), "transform.position.x")`
(fields, former names, vector components x/y/z/w or r/g/b/a, `[i]` / `.i` array indices, map keys, optional `.value`),
then `ref.get()` / `ref.set(serial::Value)` / `ref.getAs<T>()` / `ref.setAs(v)` (numeric conversions are tolerant).
Core's own value types: `reflect::registerCoreReflection()` ("ox.Transform", "ox.AABB", "ox.Sphere").

## Serialization (`serial/`)
- `serial::Value` — typed, self-describing value tree (tags: bool, i8…u64, f32/f64, string, vecN/ivecN/quat/mat4, uuid,
  entity ref, enum, object, array, optional, map). Arrays of fixed-size elements are stored packed (raw blocks).
- `serial::toValue(obj)` / `fromValue(value, obj, options)` — reflection bridge. Reading is tolerant: numeric types
  convert, missing fields keep defaults, unknown fields are ignored, `attr::FormerName` resolves renames, enums accept
  names/numbers. `ConvertOptions::fieldFilter` selects fields (e.g. save-game only); `NoSerialize` is always skipped.
- `serial::Writer` / `serial::Reader` (archive.hpp) — reflected values plus a manual API:
  ```cpp
  serial::Writer w("save", /*version*/ 3);
  w.value("player", player);                 // reflected / built-in
  w.beginObject("quests"); w.value("active", ids); w.endObject();
  w.beginArray("log"); w.element(std::string("met npc")); w.endArray();
  w.blob("thumbnail", pngBytes);
  w.save("slot1.oxsave");                    // ".json" suffix => JSON

  auto r = serial::Reader::load("slot1.oxsave");
  r->value("player", player);                // false + unchanged when missing
  if (r->beginObject("quests")) { r->value("active", ids); r->endObject(); }
  if (auto n = r->beginArray("log")) { std::string s; r->value(0, s); r->endArray(); }
  ```
- Formats (format.hpp): **OXB1** binary (header, META/STRS/TYPE/DATA chunks with CRC32, schema table with type/field
  name hashes and descriptors, field records with byte sizes so unknown fields can be skipped, enums by name hash) and
  **JSON** (`{"format":"oxb1-json", "schema":{...}, "data":{...}}`, every object has `"$type"`). `binaryToJson` /
  `jsonToBinary` are lossless (byte-identical) without C++ types; `inspectBinary` reads header/chunks/schema;
  `loadDocument`/`saveDocument` (atomic temp + rename), `decodeAny` auto-detects. Hand-written JSON without a schema
  is accepted (types inferred, conversion happens when reading into C++ types).
- CLI: `tools/oxdump` — `oxdump file.oxscene` (pretty JSON), `-o out.json`, `--to-binary in.json out`, `--info`, `--check`.

## CVars & scalability (`cvar.hpp`, `scalability.hpp`)
```cpp
static ox::CVar<bool>  cvVsync("r.VSync", true, "Vertical sync", ox::CVarFlags::Persist);
static ox::CVar<float> cvGamma("r.Gamma", 2.2f, "Display gamma", 1.0f, 3.0f);          // clamped range
static ox::CVar<int>   cvUpscaler("r.Upscaler", 0, "Upscaler", ox::CVarEnum{"Off", "FSR1", "DLSS"});
static ox::CVar<int>   cvShadowRes("r.Shadows.Resolution", 2048, "Shadow map resolution",
                                   ox::Scalability::Shadows, {512, 1024, 2048, 4096});
static ox::ConsoleCommand cmdReload("r.ReloadShaders", "Recompile shaders",
                                    [](std::span<const std::string> args) { return std::string("ok"); });
cvShadowRes.onChanged([](int now, int before) { ... });
```
Declare cvars in a .cpp that is linked anyway (next to the code reading them). Types: bool, int (also enum-as-int),
float, std::string; values are atomics (string: mutex) so any thread may read. Flags: `ReadOnly` (code only),
`Persist`, `RequiresRestart` (`CVarRegistry::restartRequired()`), `Cheat` (console needs `setCheatsEnabled`).
`CVarSource` tracks who set a value (Code/Scalability/Config/Console).
`CVarRegistry::instance()`: `find`, `set(name, text)`, `execute("r.Gamma 1.8")`, `complete(prefix)`,
`saveOverrides()/loadOverrides(json)` (+ `…ToFile`), values for not-yet-registered cvars stay pending until they register.

Scalability groups `ViewDistance … RayTracing`, levels `Low/Medium/High/Ultra` (+`Custom`). Group level lives in the
persisted cvar `sg.<Group>`; `scalability::setOverall(level)`, `setGroup(g, level)`, `currentLevel(g)` (Custom when a
member cvar differs from its level value), `overallLevel()`, `savePreset()/loadPreset(json)` (groups + custom overrides).
Cvars registered after a level was chosen start at that level.

## Core utilities
- **math.hpp** — glm (RH, Y-up, -Z forward, depth [0,1]). `Transform {position, rotation(quat), scale}`:
  `toMatrix/fromMatrix/inverse`, `parent * child` (= `compose`), `transformPoint/Vector/Direction`, `forward/right/up`,
  `Transform::lerp` (slerp). `decompose(m, t, r, s)`. `lookRotation(fwd, up)` (-Z → fwd), `fromToRotation`,
  `perspectiveReversedZ`, `perspectiveInfiniteReversedZ`, `orthoReversedZ` (near→1, far→0).
  Volumes: `AABB` (default = empty; `expand`, `transformed(mat4)`), `Sphere`, `Plane` (dot(n,p)+d=0), `OBB`.
  `Frustum::fromViewProj(vp, reversedZ=true)` — inward normalised planes L,R,B,T,N,F; a degenerate infinite far plane
  always passes. Ray tests: `intersectRayAABB/Sphere/Plane`, `intersectRayTriangle` (`RayHit{t,normal,u,v,frontFace}`).
  `Random` = seeded xoshiro256**. Limit: TRS inverse/compose are exact only for uniform scale.
- **time.hpp** — `Clock::now()`, `Stopwatch`, `FixedTimestep(dt=1/60, maxSteps=8)`: `advance(frameDt)` returns the steps to
  run, `alpha()` for interpolation, excess time dropped (`droppedTime()`). `FrameTimer`.
- **memory.hpp** (not thread-safe) — `FrameAllocator` (bump, trivially destructible types only),
  `MultiFrameAllocator`/`FrameAllocatorRing<N>` (`beginFrame(i)`), `PoolAllocator`, `TypedPool<T>`, `Handle<Tag>` +
  `HandlePool<T,Tag>` (generational, free-list reuse).
- **jobs.hpp** — `JobSystem(threads=0)` over enkiTS: `submit` → `JobHandle`, `parallelFor(count, grain, fn(begin,end,thread))`
  (blocking) / `parallelForAsync`, `wait`, `waitAll`, `enqueueMainThread` + `runMainThreadQueue()`, `TaskGroup`.
  Waiting inside a job runs other jobs. Register it as a service.
- **events.hpp** — `Signal<Args...>` (+ `Connection`, RAII `ScopedConnection`), `EventBus` (`subscribe<E>`, `publish`
  immediate, `enqueue` from any thread + `dispatch()` FIFO).
- **profile.hpp** — `OX_PROFILE_ZONE()`, `OX_PROFILE_ZONE_N("name")`, `OX_PROFILE_FRAME()`, `OX_PROFILE_ALLOC/FREE`,
  `OX_PROFILE_THREAD_NAME`, `OX_PROFILE_PLOT`, `OX_PROFILE_MESSAGE` — Tracy when active (`OX_TRACY`), else no-ops.
- **debug_draw.hpp** — `DebugDraw` (a service): line, ray, aabb, box, obb, sphere, circle, capsule, cone, cylinder,
  frustum, arrow, axes, grid, point, text3D, each with color, duration and depth-test flag; thread-safe submission.
  Renderer: `flush(dt)` once per frame, then `depthTestedLines()`, `overlayLines()` (`std::span<const DebugVertex>`),
  `texts()`. Duration 0 = exactly one frame.
- **paths.hpp** — `paths::engineSourceDir()` (`$OXWALD_SOURCE_DIR` overrides), `executableDir()`, `userDataDir(app)`, `tempDir()`.
- **vfs.hpp** — `Vfs` service: `mount("engine"|"project"|"user", std::unique_ptr<IMountSource>, priority)`;
  `exists/readBytes/readText/writeBytes/writeText/resolveNative/list("project://levels", recursive)`. `DirectoryMount`
  provided; pak archives implement `IMountSource` (assets module). `..` escapes are rejected.
- **file_watcher.hpp** — polling `FileWatcher` (mtime + size, debounce): `watchFile`, `watchDirectory(dir, cb, recursive,
  {".glsl"})`, `poll()` on the calling thread or `start(interval)` background scanning.

**ThreadSanitizer**: enkiTS is a prebuilt, uninstrumented library, so `jobs.cpp` mirrors its hand-offs with
`__tsan_release/__tsan_acquire` (submit -> run -> completion/wait/waitAll/shutdown; compiled only under TSan).
TSan runs need no suppressions (`tools/sanitizers/tsan.supp` documents this).

## Polish additions (0.1.0)
- `CVarRegistry::addChangeListener(fn(ICVar&, CVarSource))` / `removeChangeListener(id)`: notified after any
  registered cvar changes (code, console line, `set()`, config, scalability), on the changing thread. The runtime uses
  it to re-apply graphics settings for `r.*`/`sg.*` changes with `CVarSource::Console`.
- Duplicate cvar/command names are detected case-insensitively (lookups are case-insensitive).
- `log::setStderrSinkEnabled(false)` disables the built-in stderr output (Fatal is always printed); `log::write` is
  unfiltered, `OX_LOG_*` / `print` honour `minLevel()`.
- `serial::decodeBinary(bytes, BinaryDecodeOptions{{"header"}})` decodes only the listed root fields (the rest are
  skipped by record size) — used for save-slot listings.

## Limits / TODO
- JSON cannot represent NaN payload bits (NaN/±inf are written as strings `"nan"`, `"inf"`); object keys `"$type"` are reserved.
- Arrays in the value tree are homogeneous; heterogeneous hand-written JSON arrays are rejected.
- `std::vector<bool>` and `std::array` are not reflectable (use `std::vector<u8>`).
- CVar change callbacks run on the thread that changed the value.
