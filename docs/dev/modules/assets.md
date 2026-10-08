# assets (`Oxwald::assets`, namespace `ox::assets`)

Asset database, importers, CPU-side asset data in GPU-ready layouts, async loading with dependencies and hot
reload, `.oxpak` packaging. The GPU side (uploads, GPU resource cache) belongs to the renderer: it consumes
`MeshData`/`TextureData`/`MaterialAsset` and subscribes to `AssetManager::onLoaded/onReloaded/onUnloaded`.

Deps: core (public), animation (public, optional — skeleton/clip assets when the module is configured, macro
`OX_ASSETS_HAS_ANIMATION`), scene (private — prefab creation), assimp, fastgltf, meshoptimizer, KTX (Basis
Universal encoder + transcoder), stb, tinyexr, zstd (private, optional — `OX_ASSETS_HAS_ZSTD`).
Umbrella header `<oxwald/assets/assets.hpp>`. Call `ox::assets::registerAssetTypes()` once (idempotent; the
registry/manager call it too).

Tools: `tools/oximport` (import one file, print stats), `tools/oxpack` (cook a project into a pak).

## Overview

| Header | Contents |
| --- | --- |
| `asset_types.hpp` | `AssetType` (Mesh, Texture, Material, Scene, Prefab, Script, Audio, AnimationClip, Skeleton, Font, NavMesh, Heightmap, Raw, `FirstCustom`), built-in UUIDs (`builtin::checkerTexture()`, `cubeMesh()`, `defaultMaterial()`, ...) |
| `asset_registry.hpp` | `AssetRegistry` — editor database for `<project>/Assets` (+ `.meta`, `.oxcache`) |
| `importer.hpp` | `IAssetImporter`, `ImportContext`, `ImporterRegistry`, `importStandalone` |
| `asset_manager.hpp`, `asset_handle.hpp` | `AssetManager` service, `AssetHandle<T>` |
| `asset_source.hpp` | `IAssetSource` (registry or paks), `AssetRecord` |
| `mesh.hpp`, `mesh_processing.hpp`, `model_import.hpp` | `MeshData` + `.oxmesh`, tangents/LODs/meshlets/bounds/collision/convex hull, glTF/assimp import |
| `image.hpp`, `texture.hpp`, `texture_import.hpp` | `Image`, mips, cubemaps, `TextureData` + `.oxtex`, BC7/BC5 encode/decode, KTX2 |
| `material.hpp` | `MaterialAsset` (reflected), JSON/binary I/O |
| `asset_data.hpp` | `SceneAsset`, `PrefabAsset`, `ScriptAsset`, blob assets (audio, font, navmesh, heightmap), `SkeletonAsset`, `AnimationClipAsset` |
| `pak.hpp`, `cook.hpp` | `.oxpak` writer/reader, `PakMountSource` (VFS), `PakAssetSource` (cooked runtime), `cookProject` |

## Asset database (`AssetRegistry`)

```
<project>/Assets/**                  sources, each with "<file>.meta"
<project>/.oxcache/artifacts/<uuid><ext>   cooked artifacts (.oxmesh, .oxtex, .oxmat, .oxprefab, ...)
<project>/.oxcache/imports/<uuid>.json      import records
<project>/pack.json (optional)              {"startupScenes": [...], "alwaysInclude": [...]} for oxpack
```

```cpp
ox::assets::AssetRegistry registry("MyGame");        // built-in importers registered
registry.scan();                                     // metas, UUID<->path, moves, duplicates
registry.importAll();                                // or lazily via record()/readArtifact()
ox::Uuid rock = *registry.uuidForPath("Textures/rock.png");
registry.setSettings(rock, {{"type", "Linear"}, {"maxSize", 1024}});   // rewrites the meta + reimports
registry.dependencies(materialUuid);  registry.dependents(textureUuid);
registry.startWatching();  /* each frame: */ registry.poll();         // hot reimport -> onReimported
```

- **Meta** (`asset_meta.hpp`): `{"formatVersion":1, "uuid":"…", "importer":"texture", "importerVersion":1,
  "settings":{…}}`. Settings are the importer's reflected settings struct as plain JSON (enums by name). New metas
  get defaults from `IAssetImporter::defaultSettingsFor(path)` (e.g. `*_normal.png` → Normal, `*_orm/_rough/_ao`
  → Linear, `.hdr/.exr` → HDR). A model importing a never-imported texture as normal/ORM map applies a settings
  hint to that texture's meta.
- **Moves/renames**: a meta moved with its file keeps the UUID (`onMoved`). A file moved without its meta is matched
  to the orphan meta whose import record has the same content hash. Copied file+meta pairs get a fresh UUID.
  Orphan metas of deleted files are removed (`Options::deleteOrphanMetas`) together with their cache files.
- **Reimport** when: no record, importer name/version changed, settings hash changed, source size/mtime changed
  *and* content hash differs, an extra source file (`.mtl`, `.bin`, cubemap faces, referenced textures) changed,
  or an artifact file is missing. Failed imports are not retried until the source changes.
- **Sub-assets**: one source can produce several artifacts. Their UUIDs are deterministic,
  `Uuid::fromName("<main uuid>/<name>")`, and paths are `"<source>#<name>"`, e.g. `Models/hero.glb#Mesh/0`.
- **Dependencies**: per artifact; filtered to known assets (scene/prefab documents list every UUID they contain,
  entity ids are dropped this way). `collectDependencies(roots)` = transitive closure (imports on demand).
- Thread-safe (one recursive mutex; imports serialise).
- `Options::assetsDir` (default `"Assets"`, relative to the project or absolute) selects the source directory; the
  runtime passes `ProjectSettings::assetDirs[0]`.

### Importers

| Importer | Extensions | Main artifact | Sub-assets |
| --- | --- | --- | --- |
| `texture` | png jpg jpeg tga bmp psd gif hdr exr ktx2 | Texture | – |
| `cubemap` | `.oxcube` (`{"faces": [+X,-X,+Y,-Y,+Z,-Z image paths]}`) | Texture (cube) | – |
| `model` | gltf glb (fastgltf) · obj fbx dae 3ds ply stl blend x lwo ms3d (assimp) | Prefab | `Mesh/<i>` (or `Mesh/merged`), `Material/<i>`, `Texture/<i>[_normal|_linear]` (embedded images), `Skeleton`, `Clip/<i>` |
| `material` | `.oxmat` (JSON, also the runtime format) | Material | – |
| `scene`, `prefab` | `.oxscene(.json)`, `.oxprefab(.json)` | Scene / Prefab (OXB1 binary) | – |
| `script` | `.lua` | Script (text) | – |
| `audio` | wav ogg mp3 flac | Audio (OXBL passthrough + sampleRate/channels/frames/duration) | – |
| `font`, `navmesh`, `heightmap` | ttf otf · oxnav navmesh · r8 r16 r32 raw | OXBL passthrough (heightmap: width/height/format/heightScale settings) | – |

Gameplay importers (`gameplay::registerGameplayImporters(registry.importers())`, gameplay module): `.oxbt`
behaviour trees and `.oxanimctrl` animator controllers (JSON) -> `AssetType::Raw` blobs with info
`{"kind": "BehaviorTree" | "AnimatorController"}` (controller clip UUIDs become dependencies).

Custom importers: derive `IAssetImporter` (`name`, `version`, `extensions`, `mainType`, `defaultSettings`,
`import(ImportContext&)`), then `registry.importers().add(std::make_unique<MyImporter>())` (later registrations
win). In `import`: `ctx.settings<T>()`, `ctx.readSource()`, `ctx.setMain(type, bytes)`, `ctx.addSubAsset(name, type,
bytes)`, `artifact.dependencies`, `ctx.resolveAsset(path, settingsHint)` (UUID of another source, meta created on
demand), `ctx.addSourceDependency(path)`. Register a loader for custom types on the `AssetManager`.

## Loading (`AssetManager`)

```cpp
ox::JobSystem jobs;
ox::assets::AssetManager assets(registry /* or a PakAssetSource */, &jobs, {.memoryBudget = 512u << 20});
services.addExternal(assets);

auto mat = assets.load<MaterialAsset>("Materials/rock.oxmat", ox::assets::kPriorityHigh);
mat.onLoaded([](bool ok) { /* main thread, from update(); textures are loaded too */ });
auto mesh = assets.loadSync<MeshData>("Models/rock.glb#Mesh/0");
const MeshData* m = mesh.getOrDefault();       // cube placeholder until loaded
std::shared_future<bool> f = mat.future();      // adapter point for the async/coroutine module

assets.onReloaded.connect([&](const ox::Uuid& id, AssetType type) { gpuCache.reupload(id); });
// every frame, main thread:
assets.update();   // callbacks, onLoaded/onFailed/onReloaded/onUnloaded, hot-reload swaps, LRU eviction
```

- Requests run on the `JobSystem` in priority order (synchronously without a job system or with `loadSync`).
- Dependencies (record + loader-reported, e.g. material → textures) load first; the asset becomes `Loaded` only
  after all of them finished (failed dependencies do not fail the parent — use `getOrDefault`). Dependencies stay
  referenced while the parent is loaded.
- `AssetHandle<T>` is ref-counted. Unreferenced assets stay cached up to `Options::keepUnreferenced` (LRU) or
  until `memoryBudget` is exceeded (oldest unreferenced evicted first); `unloadUnused()` evicts all now.
- `get()` pointers stay valid until the next `update()` after a reload/unload; use `share()` for longer access.
  `generation()` changes on every reload.
- Hot reload: the registry's change notification queues a background reload; `update()` swaps the data and emits
  `onReloaded`. Failed assets are retried on change.
- `stats()`: counts, bytes per type, budget. Built-ins (always resident): checker/white/flat-normal textures, cube
  mesh, default material; placeholders: checker (Texture), cube (Mesh), default material (Material).
- Type mismatch (`load<MeshData>` on a texture) fails with an error. Dependency cycles are not detected (would hang).

## Runtime data formats (for the renderer)

All little-endian. UUIDs are written as `u64 hi, u64 lo`.

### Mesh vertex streams (vertex pulling, `mesh.hpp`)

Non-interleaved position stream (depth/shadow/culling passes read 12 B per vertex) + one interleaved attribute
stream + optional skin stream. Indices are `u32`, triangle lists, **absolute** (already include the submesh vertex
offset). All LODs share the vertex streams. Engine conventions: right-handed, Y-up, meters, CCW front faces, UV
origin top-left (glTF; assimp sources are flipped on import).

| Stream | Stride | Layout |
| --- | --- | --- |
| `positions` | 12 | `0: f32 x,y,z` |
| `attributes` (`VertexAttributes`) | 48 | `0: f32x3 normal` · `12: f32x4 tangent (xyz, w = ±1, B = w·cross(N,T))` · `28: f32x2 uv0` · `36: f32x2 uv1` · `44: u32 color RGBA8 unorm (R = low byte), linear` |
| `skin` (`SkinVertex`, optional) | 16 | `0: u16x4 joints (skeleton joint indices)` · `8: u16x4 weights (unorm16, sum 65535)` |
| `indices` | 4 | `u32` |
| `meshlets` (`Meshlet`) | 64 | `0: f32x3 center` · `12: f32 radius` · `16: f32x3 coneAxis` · `28: f32 coneCutoff` · `32: f32x3 coneApex` · `44: u32 submesh` · `48: u32 vertexOffset` · `52: u32 triangleOffset` · `56: u32 vertexCount (≤64)` · `60: u32 triangleCount (≤124)` |
| `meshletVertices` | 4 | `u32` absolute vertex index; meshlet range `[vertexOffset, +vertexCount)` |
| `meshletTriangles` | 1 | `u8` local indices, 3 per triangle, from byte `triangleOffset`; each meshlet's block padded to 4 bytes |

```glsl
layout(buffer_reference, scalar) readonly buffer Positions { vec3 p[]; };
struct VertexAttributes { vec3 normal; vec4 tangent; vec2 uv0; vec2 uv1; uint color; };   // 48 B, scalar
layout(buffer_reference, scalar) readonly buffer Attributes { VertexAttributes a[]; };
struct Meshlet { vec3 center; float radius; vec3 coneAxis; float coneCutoff; vec3 coneApex; uint submesh;
                 uint vertexOffset; uint triangleOffset; uint vertexCount; uint triangleCount; };  // 64 B
// cone culling: reject if dot(normalize(center - cameraPos), coneAxis) >= coneCutoff
// triangle t of meshlet m: uint b = m.triangleOffset + 3*t; local indices = tri[b], tri[b+1], tri[b+2] (u8)
```

Per submesh: `vertexOffset/vertexCount`, `materialSlot`, `bounds`, `lods[l] = {indexOffset, indexCount,
meshletOffset, meshletCount, error}`. LOD 0 is full detail; `error` is the simplification error in mesh units
(use with the bounding sphere for screen-space LOD selection). Submeshes may have fewer LODs than others
(simplification stops when it stalls) — clamp the LOD index per submesh. Mesh-level `bounds` (AABB), `boundingSphere`,
`materials[slot] = {name, material uuid}`, `skeleton` uuid, `collision` (`MeshCollisionData`: convex hull points +
triangles, simplified welded triangle mesh — physics input).

### `.oxmesh` file

```
0   char magic[4] "OXMS"   u32 version (1)   u32 chunkCount   u32 reserved
16  chunkCount × { char id[4]; u32 crc32(payload); u64 offset; u64 size }
    payloads, each 16-byte aligned
INFO  string name (u32 len + UTF-8), f32x3 aabbMin, f32x3 aabbMax, f32x3 sphereCenter, f32 sphereRadius,
      uuid skeleton, u32 vertexCount
SUBM  u32 count; per submesh: string name, u32 materialSlot, u32 vertexOffset, u32 vertexCount, f32x3 min, f32x3 max,
      u32 lodCount, lodCount × {u32 indexOffset, u32 indexCount, u32 meshletOffset, u32 meshletCount, f32 error}
MATS  u32 count; per slot: string name, uuid material
POSN, ATTR, SKIN (optional), INDX, MSHL, MVTX, MTRI   raw streams (layouts above)
COLH, COLI, COLV, COLX (optional)   hull vertices f32x3, hull indices u32, collision vertices f32x3, indices u32
```
`deserializeMesh` validates CRCs, stream lengths and every index/meshlet range.

### Textures

Formats (`TextureFormat` → `toVkFormat`): `RGBA8Srgb` (43), `RGBA8Unorm` (37), `BC7Srgb` (146), `BC7Unorm` (145),
`BC5Unorm` (141), `RGBA16Float` (97), `RGBA32Float` (109), `R8` (9), `RG8` (16), `R16` (70), `R32F` (100);
`BC6HUfloat` (143) is reserved.

| Import type | Default format | Uncompressed (`compression: Uncompressed`) |
| --- | --- | --- |
| Color / UI (sRGB) | BC7 sRGB | RGBA8 sRGB |
| Linear (ORM, masks) | BC7 unorm | RGBA8 unorm |
| Normal | BC5 (RG = XY, reconstruct Z in the shader) | RGBA8 unorm |
| HDR (.hdr/.exr or type HDR) | RGBA16F | RGBA16F |

- **BC7** = Basis Universal UASTC (libktx encoder, level `compressionQuality`, default 1) transcoded to BC7 at
  import time. Trade-off: we chose import-time transcoding over shipping KTX2/UASTC+zstd with runtime
  transcoding: zero load-time cost, per-mip byte ranges for streaming, BC is supported by every desktop target
  incl. MoltenVK on Apple Silicon. Cost: larger cooked size than RDO-UASTC+zstd and no ASTC/ETC path. For devices
  without BC the renderer can call `decompressToRGBA8(texture)` (4× memory). KTX2 sources (incl. Basis
  ETC1S/UASTC) are imported as-is or transcoded to BC7 (BC5 for normal maps).
- **BC5**: own encoder (two BC4 blocks: min/max, local endpoint search, least-squares refinement).
- **BC6H**: no encoder in the dependency set → HDR is stored as RGBA16F (8 B/texel). TODO when an encoder is added.
- Mips: Kaiser (windowed sinc, radius 3, α = 4) or box; sRGB data is filtered in linear space; normal maps are
  renormalised per level; optional alpha-coverage preservation (binary-searched alpha scale per level).
- Cubemaps: from equirect (`cubemap: FromEquirect`, 2×2 supersampled; `u = atan2(d.x, −d.z)/2π + 0.5`,
  `v = acos(d.y)/π`, so −Z is the image centre) or from 6 faces (`.oxcube`). Face order +X, −X, +Y, −Y, +Z, −Z,
  Vulkan cube addressing (`cubeFaceDirection`).
- Import settings: `type, srgb, maxSize, compression, generateMips, mipFilter, preserveAlphaCoverage, alphaCutoff,
  flipY, wrapU, wrapV, filter, streamingPriority, cubemap, cubeFaceSize, compressionQuality`.

### `.oxtex` file

```
0   TexHeader (64 B): char magic[4] "OXTX", u32 version (1), u16 format (TextureFormat), u16 flags (1 = cube,
    2 = normal map), u32 width, u32 height, u32 layers (6 for cubes), u32 mipCount, u8 wrapU, u8 wrapV, u8 filter,
    u8 streamingPriority, u32 headerSize (= 64 + 32·mipCount), u32 tableCrc (CRC32 of the level table), u8[24] 0
64  mipCount × TexLevel (32 B): u64 offset, u64 size, u32 width, u32 height, u32 crc32(data), u32 0
    level data, 16-byte aligned, SMALLEST LEVEL FIRST (header + whole mip tail = one contiguous prefix)
    level = layers × faceSize, layer-major; faceSize = w·h·bpp or ceil(w/4)·ceil(h/4)·16 for BC
```
Streaming: read `kTextureMinHeaderRead` (4 KiB) → `readTextureInfo` → `readMipRange(reader, info, first, last)`
with a `RangeReader` (file: `fileRangeReader`, pak/registry: `IAssetSource::readArtifactRange`).
`deserializeTexture(bytes, firstMip)` skips the largest levels.

### Materials (`MaterialAsset`, reflected "Material")

`shadingModel` (Lit, Unlit, Subsurface, Foliage), `blendMode` (Opaque, AlphaTest, Transparent, Refractive),
`baseColor` (linear RGBA), `metallic`, `roughness`, `emissive` + `emissiveStrength`, `normalStrength`,
`occlusionStrength`, `heightScale`, textures `albedoTexture`, `normalTexture`, `ormTexture` (R occlusion, G
roughness, B metallic — glTF layout), `emissiveTexture`, `heightTexture`, `alphaCutoff`, `doubleSided`, `ior`,
`transmission`, `thickness`, `absorptionColor` + `absorptionDistance` (Beer–Lambert, 0 = off), `clearcoat`,
`clearcoatRoughness`, `subsurface`, `subsurfaceColor`, `uvTiling`, `uvOffset`, `renderQueueOffset`.
`.oxmat` = plain JSON (`{"oxmat": 1, "shadingModel": "Lit", "albedoTexture": "uuid", ...}`, missing fields keep
defaults); the cache keeps an OXB1 binary (`materialToBinary`). Materials imported from glTF/FBX/MTL: factors,
alpha mode, KHR transmission/volume/ior/clearcoat/emissive_strength/unlit/texture_transform; when the ORM texture
has no packed occlusion, `occlusionStrength` is 0.

### Other artifacts

- Scene/Prefab: OXB1 documents (see core/scene docs); `SceneAsset::document` → `ox::deserializeWorld`,
  `PrefabAsset::document` → `ox::instantiatePrefab`. Model prefabs: root entity (file name) → node entities with
  `Transform` and `MeshRenderer{mesh, materials per slot}`; deterministic entity UUIDs; prefab id = model UUID.
- `OXBL` blob (audio, fonts, navmesh, heightmaps, skeleton/clip wrappers): `"OXBL" u32 version, u16 AssetType,
  u16 0, string infoJson, u64 size, u32 crc32, pad to 16, payload`.
- Skeleton/AnimationClip: OXBL around `ox::anim::serialize` (clip info carries the skeleton UUID).

### `.oxpak`

```
0   PakHeader (64 B): "OXPK", u32 version (1), u32 flags, u32 entryCount, u64 tocOffset, u64 stringsOffset,
    u64 stringsSize, u32 alignment, u32 tocCrc (CRC32 of TOC + strings), u64 dataSize, u8[8]
64  entry data, each entry aligned to `alignment` (default 16)
    TOC: entryCount × PakTocEntry (64 B), sorted by pathHash: u64 pathHash (fnv1a64), u64 uuidHi, u64 uuidLo,
         u64 offset, u64 storedSize, u64 size, u32 crc32 (uncompressed), u16 compression (0 none, 1 zstd), u16 flags,
         u32 pathOffset, u32 pathSize
    string table (UTF-8 paths)
```
Cooked layout: `assets/<uuid><ext>` per artifact + the project's `<Name>.oxproj` file(s) at the root (the runtime
reads it when started with only `--pak`) + `catalog.oxcat` (JSON: uuid, type, source path, artifact entry,
dependencies). zstd per entry when it saves > 1/16; textures are stored uncompressed (range reads for mip
streaming). Readers mmap the file on POSIX (`PakReader::view` gives zero-copy spans), verify the TOC CRC on open
and entry CRCs on read.

```cpp
// cooked game: paks only, no importers
ox::assets::PakAssetSource source;
source.addPak("Game.oxpak");                       // later paks override earlier ones (patches)
ox::assets::AssetManager assets(source, &jobs);
vfs.mount("game", std::make_unique<ox::assets::PakMountSource>(*ox::assets::PakReader::open("Game.oxpak")));
```

## Engine and gameplay integration

The runtime registers `AssetRegistry` (dev/editor) or `PakAssetSource` (cooked, `EngineConfig::pakPath`),
`IAssetSource` and `AssetManager` as services, calls `update()` every frame on the game thread and serves
`project://<assetDir>/<path>` from the pak in cooked games (runtime.md). Gameplay implements its provider
interfaces on top of `AssetManager` (`gameplay::AssetProviders`, gameplay.md) and turns `onReloaded` into hot
reloads of running scripts, behaviour trees, prefab instances, animators and mesh colliders.

## Tools

```sh
oximport Assets/hero.glb [--settings '{"generateCollision": true}'] [--out dir] [--json]
#   Prefab main ... / Mesh Mesh/0  2876 vertices, triangles/LOD [968,482,400], 78 meshlets / Material ...
oxpack MyGame -o Game.oxpak [--scene Levels/start.oxscene] [--include UI/] [--all] [--no-compress] [--verify]
#   cooks startup scenes + their dependency closure + always-include list (pack.json merged), prints sizes
```
With neither scenes nor includes (and no `pack.json`), everything is packed.

## Tests (`ox_assets_tests`, label `assets`)

Registry (meta creation, stable UUIDs across rescans/instances, move with/without meta, copied metas, removal,
settings-change reimport, model sub-assets + dependency graph, hot reimport, passthrough importers), meshes (glTF
hierarchy/materials/tangents, axis/unit conversion, OBJ via assimp incl. textured MTL, LOD reduction, meshlet limits
and bounds, `.oxmesh` round trip + corruption, convex hull, collision data, cube), textures (mip sizes, sRGB-correct
downsampling, normal renormalisation, alpha coverage, BC7 PSNR > 38 dB, BC5 PSNR > 40 dB, formats, `.oxtex` round
trip + mip ranges, equirect → cube, KTX2/UASTC import, HDR load), materials (JSON/binary round trip, hand-written
JSON, `.oxmat` in a project), manager (async dependency order, sync load, ref-count/LRU unload, memory budget,
placeholders/failures, priorities, hot-reload signal), pak (write/read/mount/integrity, cook + cooked runtime
loading + mip range reads); CLI tests `oximport.*`, `oxpack.*`. Fixtures are generated at test time; the legacy
sample assets are read only.

## Limits / TODO

- No BC6H encoder (HDR = RGBA16F); no ASTC/ETC cooking for mobile; BC7 relies on UASTC quality (~43–48 dB),
  not a dedicated BC7 encoder. BC7 decoding uses Basis' `unpack_bc7` symbol from libktx (no public header).
- Tangents: angle-weighted MikkTSpace-equivalent, not bit-identical to the reference implementation.
- Skinned import: joints are remapped by name to the animation module's skeleton; not exercised by tests with a
  real skinned file yet. Morph targets, glTF cameras/lights, KHR_draco/meshopt compression are not imported.
- Scene/prefab dependency lists contain every known asset UUID referenced; loading a `SceneAsset` therefore loads
  the referenced assets too (intended for level loads).
- Dependency cycles between assets hang the loader; the async/coroutine module should wrap `onLoaded`/`future()`.
- Registry operations serialise on one mutex (an import blocks other lookups); the file watcher is polling.
- Material extraction to editable `.oxmat` files (instead of model sub-assets) is not implemented.
