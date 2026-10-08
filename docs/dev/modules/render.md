# render (`Oxwald::render`)

Clustered forward+ renderer on top of `rhi`: GPU scene, extract, feature extension API, PBR + IBL, raster shadows
(CSM / spot atlas / point cubes, PCF + PCSS, caching), picking, editor overlays, debug views, stats, quality
auto-detection. Namespace `ox::render`. Umbrella header `<oxwald/render/render.hpp>`.

Deps: `rhi`, `scene`, `assets` (CPU mesh/texture/material structs; `asset_provider.cpp` glue when the library is
built), `runtime` (only for the `IRenderer` adapter, linked automatically when configured). Shaders live in
`engine/shaders/render/`.

**Feature authors: read §2 (API), §3 (resources), §6 (shader conventions, including the Metal shadow-sampler rule)
and §8 (golden tests).**

## 1. Frame architecture

Forward+ rather than deferred: MSAA and transparency stay possible, translucency reuses the same light clusters, and
the material model isn't constrained by G-buffer bandwidth. A thin prepass G-buffer (`Normals` + `Velocity`) still
feeds the screen-space effects (SSAO, SSR, TAA) before lighting.

```
 snapshot ──► SceneUpload (instances/materials/mesh table: staged copies, graphics queue)
              │
   PreDepth   │  [features: GPU culling, previous-frame HiZ occlusion]   (built-in: Environment → IBL/sky constants)
              ▼
         DepthPrepass ──► Depth (D32, reversed-Z) · Normals (RGBA16F: N, roughness) · Velocity (RG16F) · EntityID (editor)
              ▼
            HiZ (R32F min pyramid; culled when unused)
   AfterDepth │  [SSAO, SSR trace, contact shadows, decals]
              ▼
         LightCulling (compute, 16×9×24 log clusters) ──► LightClusters
   Shadows    │  [ShadowsRaster | ShadowsRT] ──► ShadowCascades · ShadowAtlas · PointShadows · ShadowMask
   Lighting   │  [AO, ReflectionsSpecular, IndirectDiffuse, VolumetricFog]
              ▼
         ForwardOpaque (depth EQUAL, clustered PBR + IBL) ──► SceneColorHDR (RGBA16F, pre-exposed)
   AfterOpaque│  (built-in: Sky at order -1000) [SSR composite, refraction copy]
   Translucency  [transparent / OIT / water / particles]
   BeforePostProcess [TAA]
   PostProcess   [ordered: auto exposure, bloom, DOF, ...]
   Upscale       single slot [FSR1 | DLSS]; default: bilinear Resample when render ≠ output resolution
   AfterUpscale  [output-resolution HDR effects]
              ▼
         Tonemap (ACES fitted / AgX / Neutral, exposure, sRGB OETF, dither) ──► SceneColorLDR (RGBA8, display-encoded)
   Overlay    │  (built-in: EditorOverlays grid + selection outline, DebugLines) [UI, gizmos]
   Debug      │  (built-in: DebugViews) 
              ▼
         Final (copy into the target; decodes for *_SRGB targets) ──► Output (swapchain image or offscreen texture)
         Pick (editor readback, side effect)
```

Every frame each view's graph is re-declared from scratch. `rhi::RenderGraph` caches the compiled plan by topology
hash, so **changing any cvar (or a feature's `isEnabled`) rebuilds the graph on the next frame without a restart**.
Unused passes (HiZ without consumers, for example) are culled automatically.

### Entry points

```cpp
auto renderer = ox::render::Renderer::create(device);            // + RendererDesc{.jobs = &jobs}
ViewId vp = renderer->createView({.name = "Main", .flags = {.editor = false}});
// per frame (the caller owns device.beginFrame/endFrame and swapchain acquire/present)
renderer->beginFrame(snapshot);                                   // settings snapshot, cache update, instances
renderer->renderView({.view = vp, .camera = cam, .target = {.swapchain = swapchain.get()}});
renderer->endFrame();                                             // RenderStats
```

* `RenderSnapshot` (`snapshot.hpp`): `extract(const World&, RenderSnapshot&, ExtractOptions)` runs on the game thread
  (cameras, mesh instances with current and previous `WorldTransform`, lights, environment, DebugDraw lines, skin
  palettes, per-entity pick id). The renderer never touches the World. `SnapshotBuffer` is a 2-slot helper.
  `addExtractHook(fn)` lets other modules append renderables (for example gameplay skinned meshes filling `palettes`).
* `CameraParams` (`render_view.hpp`): `fromComponent(CameraComponent, world)` or `lookAt(...)`.
  `RenderView` keeps per-view state: render vs output extent, Halton jitter, previous matrices, history textures,
  per-feature state, and the view's graph.
* `ViewRenderRequest::recordInto`: record into a caller-owned command list instead of submitting (editor).
* Picking: `requestPick(view, x, y, w, h)` (output pixels) → `takePickResult(id)` once ready (async, 1–2 frames).
  Ids are `encodeEntityId(entt)` = entt id + 1, and 0 means none.

### Integration

| Consumer | API |
| --- | --- |
| runtime | `#include <oxwald/render/runtime_renderer.hpp>`, `engine.setRenderer(ox::render::createRenderer())` (`OX_RENDER_HAS_RUNTIME`). Implements `ox::IRenderer`: device + swapchain on `RenderSurface::provider` (headless → offscreen target), extract into `ctx.slot` (flushes the `DebugDraw` service), render from the primary camera, `settingsChanged` updates the present mode from `r.VSync`. Uses the `assets::AssetManager` service for asset loading and hot reload. `apps/player` installs it. |
| editor | `EditorViewportRenderer` (`editor_viewport.hpp`, Qt-free): `render(frame, target, cmd)` records into the editor's command list, `pick(frame, size, pixel) → Uuid`, `renderToImage` (thumbnails). `editor_viewport_adapter.hpp` (header-only, include from editor code) gives `makeEditorViewportRenderer(device)` → `editor::IViewportRenderer` for `EditorServices::setViewportRendererFactory`. |
| assets | `resources().setProvider(makeAssetManagerProvider(assetManager), &jobs)` + `connectAssetHotReload(assetManager, resources())` (`asset_provider.hpp`). |

## 2. Feature extension API (`render_feature.hpp`)

```cpp
class IRenderFeature {
    virtual std::string_view name() const = 0;               // also the toggle cvar r.Feature.<Name>
    virtual InjectionMask injectionPoints() const = 0;       // maskOf(InjectionPoint::AfterDepth, InjectionPoint::AfterOpaque)
    virtual i32 order() const;                               // ascending within an injection point
    virtual std::string_view exclusiveGroup() const;         // "Shadows", "Sky", ... one active per group
    virtual i32 priority() const;                            // highest enabled wins in its group
    virtual std::vector<std::string_view> provides() const;  // published resource names (docs / UI)
    virtual std::vector<std::string> cvarNames() const;      // for settings UI grouping
    virtual bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const;  // evaluated every frame
    virtual bool initialize(FeatureInitContext&);            // lazily on first enable (pipelines, persistent textures)
    virtual void shutdown(rhi::Device&);
    virtual void prepareView(ViewSetup&);                    // before the graph: jitter, screen percentage, mip bias
    virtual void setup(FeatureContext&) = 0;                 // declare passes for ctx.point()
};
```

Injection points: `PreDepth, AfterDepth, Shadows, Lighting, AfterOpaque, Translucency, BeforePostProcess,
PostProcess, Upscale, AfterUpscale, Overlay, Debug` (see the diagram above). `Upscale` is a single slot: when
several features are enabled, only the highest priority runs.

Registration:
* `renderer.features().emplace<MyFeature>(args...)` at runtime, or
* `ox::render::registerFeatureFactory("MyFeature", [] { return std::make_unique<MyFeature>(); })` from your module's
  explicit `registerXxx()` init. Every Renderer created afterwards instantiates it.
* Every feature gets a process-wide bool cvar `r.Feature.<Name>` (default true).

Raster vs ray traced variants: put both in the same `exclusiveGroup`. The RT one has higher priority and
`isEnabled` returns `s.rayTracing && caps.rayTracingSupported()` (`RenderSettings::rayTracing` is already forced to
false on devices without ray queries). Both must publish the same resources (for example `ShadowMask`).

`FeatureContext` (valid during `setup`):

| | |
| --- | --- |
| `graph()`, `resources()` | the view's `rhi::RenderGraph` and the `FrameResources` blackboard (`texture(name)`, `setTexture(name, rg)`, buffers likewise) |
| `view()`, `renderExtent()`, `outputExtent()`, `settings()`, `snapshot()`, `device()`, `caps()`, `scene()` | context |
| `viewConstants()` | mutable `GpuViewConstants`, uploaded after all setups (for example the shadow feature writes the cascade matrices) |
| `viewAddress()`, `sceneAddress()` | BDA addresses for push constants (the first 16 bytes of every render push block) |
| `allocate(size)`, `upload(span)` | per-frame host-visible memory → device address (valid until the frame retires) |
| `drawLists()` | camera-culled `ViewDrawLists` (Opaque, Masked, Transparent (sorted back-to-front), Refractive) |
| `buildDrawList(DrawFilter)` | custom culled list (frustum / sphere / instance flags / buckets) for shadow views, captures, planar reflections |
| `drawBatches(cmd, list, pipelines[4], pc, size, instanceMultiplier)` | binds the index buffer, picks `pipelines[batch.variant]` (bit 0 alpha test, bit 1 double sided), one instanced `drawIndexed` per batch, counts stats |
| `history(name, desc)` | persistent ping-pong pair `{current, previous, previousValid}` (reallocated on resize, invalid after camera cuts) |
| `viewState<T>()` | persistent per (feature, view) state deriving from `IFeatureViewState` |
| `defaults()` | white / black / flat-normal / checker / black-cube textures + bindless indices |

Helpers: `fullscreenVertexShader()`, `createFullscreenPipeline(device, name, fragPath, formats, blend, defines)`,
`createComputePipeline(device, name, path, defines)`, `drawFullscreen(cmd, pipeline, pc, size)`. Pipelines created
from files hot-reload automatically.

### Complete example: a compute SSAO-style feature providing `AO`

```cpp
// my_ao.cpp
#include <oxwald/render/render.hpp>
#include <oxwald/rhi/device.hpp>

using namespace ox;
using namespace ox::render;

static CVar<float> cvAoRadius("r.AO.Radius", 0.5f, "AO radius (m)", Scalability::Effects, {0.3f, 0.5f, 0.5f, 0.8f});
static CVar<bool> cvAo("r.AO", true, "Screen-space ambient occlusion");

class MyAoFeature final : public IRenderFeature {
public:
    std::string_view name() const override { return "MyAO"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    std::string_view exclusiveGroup() const override { return "AO"; }       // an RT AO would join this group
    std::vector<std::string_view> provides() const override { return {res::kAO}; }
    std::vector<std::string> cvarNames() const override { return {"r.AO", "r.AO.Radius"}; }
    bool isEnabled(const RenderSettings&, const rhi::DeviceCaps&) const override { return cvAo; }

    bool initialize(FeatureInitContext& ctx) override {
        m_pipeline = createComputePipeline(ctx.device, "myao", "render/myao/ao.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override { d.destroy(m_pipeline); }

    void setup(FeatureContext& ctx) override {
        FrameResources& R = ctx.resources();
        const rhi::RGTexture depth = R.texture(res::kDepth), normals = R.texture(res::kNormals);
        const Extent2D e = ctx.renderExtent();
        rhi::TextureDesc td;
        td.format = formats::kAO;            // R8_UNORM, render resolution
        td.width = e.width;
        td.height = e.height;
        td.usage = rhi::TextureUsage::None;  // the graph derives Storage | Sampled
        td.name = "AO";
        const rhi::RGTexture ao = ctx.graph().createTexture(td);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        const float radius = cvAoRadius;
        ctx.graph().addPass("MyAO", rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .overwrite(ao, rhi::Access::StorageWriteCompute)
            .execute([=, this](rhi::PassContext& p) {
                struct { u64 view, scene; u32 depth, normals, out; float radius; } pc{
                    view, scene, p.sampledIndex(depth), p.sampledIndex(normals), p.storageIndex(ao), radius};
                p.cmd.bindPipeline(m_pipeline);
                p.cmd.pushConstants(pc);
                p.cmd.dispatchThreads(e.width, e.height);
            });
        R.setTexture(res::kAO, ao);          // ForwardOpaque declares the read and multiplies indirect light by it
    }

private:
    rhi::PipelineHandle m_pipeline;
};

void registerMyAo() { registerFeatureFactory("MyAO", [] { return std::make_unique<MyAoFeature>(); }); }
```

```glsl
// engine/shaders/render/myao/ao.comp
#version 460
#include <render/common/view.glsl>
layout(local_size_x = 8, local_size_y = 8) in;
OX_RENDER_PUSH(uint depth; uint normals; uint outAo; float radius;);
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, ivec2(VIEW.renderSize.xy)))) return;
    float d = OX_FETCH_2D(pc.depth, p, 0).r;
    vec3 n = OX_FETCH_2D(pc.normals, p, 0).xyz;
    vec3 ws = oxWorldPositionFromDepth(pc.view, (vec2(p) + 0.5) * VIEW.renderSize.zw, d);
    float ao = 1.0; /* ... sample around ws along n within pc.radius ... */
    OX_IMAGE_STORE_2D(r8, pc.outAo, p, vec4(ao));
}
```

`tests/gpu/renderer_tests.cpp` (`HalfAoFeature`, `MarkerFeature`) contains working versions of both patterns: a
compute input feature and a raster pass injected at `AfterOpaque`.

## 3. Resource contracts (`render_types.hpp`: `res::k*`, `formats::k*`)

| Name | Format | Size | Written by | Read by / default when absent |
| --- | --- | --- | --- | --- |
| `SceneColorHDR` | RGBA16F | render res until Upscale, output res after | ForwardOpaque, Sky, AfterOpaque … PostProcess (LOAD), upscaler (republishes) | Tonemap. **Pre-exposed**: radiance × `VIEW.preExposure` |
| `Depth` | D32 reversed-Z (near 1, far 0) | render | DepthPrepass | everything; sample with `OX_FETCH_2D`/`OX_SAMPLE_2D_LOD`, linearise with `oxLinearDepth` |
| `Normals` | RGBA16F: xyz world normal (normal-mapped), w perceptual roughness | render | DepthPrepass | SSR, SSAO, ShadowMask |
| `Velocity` | RG16F: `uvCurrent − uvPrevious` (unjittered, camera + object + skinning) | render | DepthPrepass | TAA, motion blur, upscalers: `prevUv = uv − velocity` |
| `EntityID` | R32_UINT (0 = none) | render | DepthPrepass (editor views) | picking, selection outline |
| `HiZ` | R32F, full mip chain, min (= farthest) | render | HiZ pass | occlusion culling, SSR |
| `LightClusters` | buffer (see `clusters.glsl`) | 16×9×24 × (1 + maxLights) u32 | LightCulling | ForwardOpaque, translucency, volumetrics |
| `ShadowMask` | R8 sun visibility | render | ShadowsRaster / ShadowsRT | ForwardOpaque (default: sample CSM / 1) |
| `ShadowCascades` / `ShadowAtlas` / `PointShadows` | D32 2D array / D32 2D / D32 2D array (6 layers per light) | settings | ShadowsRaster | lighting via `shadows.glsl` (indices in `VIEW`, `GpuShadow`) |
| `AO` | R8 | render | Lighting-stage feature | ForwardOpaque multiplies indirect diffuse + specular occlusion; default 1 |
| `ReflectionsSpecular` | RGBA16F: rgb radiance (not pre-exposed), a = weight | render | SSR / probes / RT reflections | replaces the prefiltered IBL by `mix(ibl, rgb, a)`; default IBL |
| `IndirectDiffuse` | RGBA16F: diffuse radiance of a white surface (irradiance / π) | render | GI features | replaces SH irradiance; default SH9 |
| `VolumetricFog` | RGBA16F 3D froxels: rgb in-scatter, a transmittance | froxel grid | volumetrics | translucency + composite (reserved name; height fog fallback in `lighting.glsl`) |
| `Exposure` | R32F 1×1: absolute exposure | — | auto exposure | Tonemap (default: camera / manual EV100) |
| `SceneColorLDR` | RGBA8 display-encoded (sRGB curve) | output | Tonemap | Overlay/Debug features (LOAD + blend), Final |
| `SelectionMask` | R8 | render | EditorOverlays | outline |
| `Output` | target format | output | Final | (imported view target) |

Rules: publish your outputs with `resources().setTexture(name, rg)`, and declare every read (`.read(t, Access::…)`)
so the graph inserts barriers. To modify `SceneColorHDR` in place, use `.color(hdr, LOAD)` or
`.write(hdr, Access::StorageWriteCompute)`. Lighting inputs must exist before `ForwardOpaque` is declared, so publish
them at `Lighting` or earlier.

## 4. GPU scene (`gpu_scene.hpp`, `gpu_types.hpp`, `common/scene.glsl`)

* Geometry arenas (shared by all meshes, growable): `positions` vec3 (12 B), `attributes` 48 B (normal, tangent+sign,
  uv0, uv1, RGBA8 colour = `assets::VertexAttributes`), u32 `indices` (mesh-relative; draws pass the mesh base as
  `vertexOffset`, so `gl_VertexIndex` indexes the arenas), `skin` (`assets::SkinVertex`), `meshlets` (u32 arena:
  `Meshlet`×n, vertices, triangles; `GpuMeshInfo::meshletOffset/Count` for GPU culling).
* Tables: `GpuMeshInfo` per submesh (LOD0 draw args, bounds, skin/meshlet offsets), `GpuMaterial` (bindless texture
  indices, PBR factors, blend mode, IOR, transmission, thickness, absorption, clearcoat, subsurface, uv transform),
  `GpuInstance` per (entity, submesh): world, prevWorld, world bounding sphere, mesh/material index, flags (cast /
  receive shadows, moved, skinned, selected), entity id, skin palette offsets. Stable slots, incremental updates
  (only changed elements are copied, from per-frame staging, by the `SceneUpload` pass on the graphics queue).
* Per view and frame: lights (`GpuLight`, directional first, intensity converted: lux; lumens / 4π → candela),
  shadows (`GpuShadow`), palettes, and the `GpuSceneHeader` with all addresses.
* `GpuResourceCache`: Uuid → mesh / texture / material. Loads through an `AssetProvider` on the JobSystem and uploads
  on the rhi transfer queue. While loading: meshes are skipped, materials use the default material, textures are
  ignored (factors only); missing albedo shows the checker. Hot reload through `invalidate` / `queueInvalidate`.
  `r.Textures.MaxSize` skips large mips; BC is decoded on the CPU on devices without BC. Procedural primitives
  (cube, sphere, plane, cylinder, capsule, cone, torus) are always resident under `primitiveUuid(Primitive)`.

## 5. Lighting & shadows

* PBR metallic-roughness: GGX, height-correlated Smith, Schlick; multi-scatter energy compensation (Fdez-Agüera /
  Filament) from the BRDF LUT (CPU generated, 128², RGBA16F). IBL: environment capture (sky or HDRI cube) → GGX
  prefiltered cube (filtered importance sampling, `r.IBL.Resolution`) + SH9 irradiance (GPU, buffer) + split sum.
  Regenerated when the environment hash changes (sky mode, HDRI, Preetham coefficients, quantised sun direction and
  colour, intensities).
* Units: directional lux, point/spot lumens, camera EV100 (`exposure = 1 / (1.2 · 2^EV100)`). Material emissive is
  display-relative (1 = white at the current exposure). Sky: procedural (zenith ≈ 10 % of the sun's lux, in cd/m²),
  HDRI cube × `skyIntensity`, or Preetham (`SnapshotEnvironment::preetham`, `world::PreethamSky::Gpu` layout).
* Clusters: 16×9×24 exponential slices from near to `min(far, r.Clusters.MaxDistance)`. Culling tests a sphere (the
  cone's bounding sphere for spots) against the cluster AABB, with 64 lights per shared-memory batch.
  `r.Clusters.MaxLightsPerCluster` (256) sets the list capacity.
* CSM: practical split (`r.Shadows.CSM.Lambda`), bounding-sphere cascades (rotation-invariant size), light-space
  origin snapped to texels (no shimmering), depth clamp pancaking, blend band (`r.Shadows.CSM.Blend`). The
  screen-space `ShadowMask` is produced from the depth buffer.
* Spot lights: atlas tiles sized by projected screen size × priority (power of two, `SpotMin/MaxResolution`). When the
  atlas is full, the largest / least important tiles are halved before anything is dropped.
* Point lights: 6 faces per light in a 2D array, guard-band FOV so the PCF kernel stays inside a face, rendered in one
  instanced draw per caster batch with `gl_Layer` from the vertex shader (`DeviceCaps::shaderOutputLayer`; 6 passes
  otherwise). Faces are sampled through hardware compare.
* Filtering: `sampler2DShadow` / `sampler2DArrayShadow` hardware compare, 16-tap Poisson PCF rotated per pixel
  (IGN), PCSS (blocker search → penumbra from `LightComponent::sourceRadius`: metres for local lights, degrees of
  angular radius for the sun). Bias: slope-scaled raster bias plus normal offset (`shadowNormalBias` 0.01 = 1 texel).
* Caching: local light shadows are re-rendered only when their allocation or light changed, or when a moving (or
  removed) instance's bounds, current or previous, intersect the light's range sphere (`r.Shadows.Caching`).
  Cascades render every frame.

## 6. Shader conventions (`engine/shaders/render/common/`)

| Include | Content |
| --- | --- |
| `view.glsl` | `ViewConstants` (mirrors `GpuViewConstants`), `ViewBuffer`, depth/position helpers taking `ViewBuffer` (`oxLinearDepth`, `oxWorldPositionFromDepth`, `oxViewRay`, `oxProjectToUv`), extra storage aliases `r8/rg16f/r16f`, debug view ids |
| `scene.glsl` | `Instance/MeshInfo/Material/Light/Shadow/VertexAttributes`, `SceneBuffer`, `OX_RENDER_PUSH(...)`, `OX_RENDER_DRAW_PUSH(...)`, `VIEW`/`SCENE` macros, `oxFetchVertex` (vertex pulling + skinning + previous position) |
| `material.glsl` | `oxSampleMaterial` (albedo, ORM, normal map, emissive, double-sided), `oxMaterialAlpha` |
| `pbr.glsl` | `OxSurface`, `oxBrdfDirect`, GGX/Smith/Schlick, attenuation, `oxEvalSH9`, specular occlusion, GGX importance sampling |
| `lighting.glsl` | `oxEvaluateLighting(view, scene, surface, pixel, viewDepth, inputs)`: directional + clustered lights, shadows, IBL, optional screen-space inputs; `oxApplyHeightFog`. Use it for translucency/water with `inputs.screenSpace = false` |
| `shadows.glsl` | `oxSunShadow`, `oxSpotShadow`, `oxPointShadow`, PCF/PCSS primitives, cube face tables |
| `clusters.glsl` | `oxClusterIndex`, `oxClusterLightCount`, `oxClusterLight` |
| `color.glsl` | sRGB transfer, `oxTonemap` (ACES fitted, AgX, Neutral), heat map |
| `sky.glsl` | `oxSkyRadiance` (procedural / HDRI / Preetham) |
| `math.glsl` | constants, IGN, Hammersley, Poisson disk, octahedral encoding, `oxCubeDirection` |
| `fullscreen.vert` | fullscreen triangle, `uv` top-left origin, depth 0 |

* Push constants (≤ 128 B, scalar): render passes start with `ViewBuffer view; SceneBuffer scene;` (16 B), and
  geometry passes add `InstanceIdBuffer drawIds` (`OX_RENDER_DRAW_PUSH`, used with `drawBatches`). Macro field lists
  must not contain commas: write one declaration per statement.
* Helpers take the 8-byte `ViewBuffer` / `SceneBuffer` references, never `ViewConstants` by value (that would load
  ~1.7 KB per call).
* Matrices in `VIEW` include the Vulkan Y flip (NDC y down, `uv = ndc.xy * 0.5 + 0.5`, front faces CCW with
  `VK_FRONT_FACE_COUNTER_CLOCKWISE`) and reversed-Z. Shadow matrices have no flip, and their uv is also
  `ndc * 0.5 + 0.5`.
* `SceneColorHDR` is pre-exposed: write `radiance * VIEW.preExposure`.
* **Metal rule: depth textures sampled with comparison samplers must go through `oxShadowTextures2D[]` /
  `oxShadowTextures2DArray[]`** (`OX_SAMPLE_SHADOW`, `OX_SAMPLE_SHADOW_ARRAY` in `common/bindless.glsl`). On MoltenVK,
  SPIRV-Cross types an array used with a shadow sampler as `depth2d`, and every other texture read through the same
  array in that shader then returns its red channel replicated (this broke the BRDF LUT until it was found).
* Use `invariant gl_Position` and the shared `passes/mesh.vert` for anything that depth-tests EQUAL against the
  prepass.

## 7. CVars & scalability

All cvars are snapshotted into `RenderSettings::fromCVars()` once per frame. Values per level are Low / Medium /
High / Ultra.

| CVar | Default | Group: levels |
| --- | --- | --- |
| `r.ScreenPercentage` | 100 (25–200) | — (upscalers override via `ViewSetup`) |
| `r.AntiAliasing` | 0 (None/FXAA/TAA, read by features) | AntiAliasing: 0, 1, 2, 2 |
| `r.Upscaler` / `r.Upscaler.Quality` | Off / Quality | — (owned here, set by the runtime) |
| `r.RayTracing` | false (forced off without ray queries) | — |
| `r.Tonemapper` | ACES (ACES, AgX, Neutral, Linear) | — |
| `r.Exposure.Mode` / `.EV100` / `.Compensation` | Camera / 10 / 0 | — |
| `r.IBL`, `r.IBL.Intensity` | true, 1 | — |
| `r.IBL.Resolution` | 128 | Shading: 64, 128, 128, 256 |
| `r.Shading.MultiScatter` | true | Shading: off, on, on, on |
| `r.Lights.Max`, `r.Clusters.MaxLightsPerCluster`, `r.Clusters.MaxDistance` | 4096, 256, 500 | — |
| `r.Sky` | true | — |
| `r.Shadows` | true | — |
| `r.Shadows.CSM.Resolution` | 2048 | Shadows: 1024, 2048, 2048, 4096 |
| `r.Shadows.CSM.Cascades` | 4 | Shadows: 2, 3, 4, 4 |
| `r.Shadows.CSM.Distance` | 120 | Shadows: 60, 100, 150, 250 |
| `r.Shadows.CSM.Lambda` / `.Blend` | 0.75 / 0.1 | — |
| `r.Shadows.AtlasSize` | 4096 | Shadows: 2048, 4096, 4096, 8192 |
| `r.Shadows.PointResolution` | 512 | Shadows: 256, 512, 512, 1024 |
| `r.Shadows.MaxShadowedLights` | 16 | Shadows: 4, 8, 16, 32 |
| `r.Shadows.MaxPointShadows` | 8 | Shadows: 2, 4, 8, 12 |
| `r.Shadows.PCFTaps` | 16 | Shadows: 0, 8, 16, 16 |
| `r.Shadows.PCSS` | true | Shadows: off, off, on, on |
| `r.Shadows.FilterRadius`, `.SpotMinResolution`, `.Caching` | 1.5, 128, true | — |
| `r.Shadows.SpotMaxResolution` | 1024 | Shadows: 512, 1024, 1024, 2048 |
| `r.Textures.Anisotropy` | 8 | Textures: 2, 4, 8, 16 |
| `r.Textures.MipBias` | 0 | Textures: 1, 0.5, 0, 0 |
| `r.Textures.MaxSize` | 8192 | Textures: 1024, 2048, 4096, 8192 |
| `r.ViewDistance.DrawDistance` | 0 (= camera far) | ViewDistance: 400, 1000, 2500, 0 |
| `r.ViewDistance.LODBias` | 0 | ViewDistance: 1, 0.5, 0, -0.5 |
| `r.DebugView` | None (Albedo, Normals, Roughness, Metallic, AO, Emissive, LightComplexity, Overdraw, ShadowCascades, Wireframe, Velocity, Depth, ShadowMask) | — |
| `r.Wireframe`, `r.GpuTimings`, `r.FrustumCulling` | false, true, true | — |
| `r.Feature.<Name>` | true | per feature, auto-registered |

Not declared here (runtime owns them): `r.VSync r.WindowMode r.ResolutionX/Y r.Monitor t.MaxFPS g.FOV
a.MasterVolume`.

Quality auto-detect: `autoDetectQuality(device)` runs blended RGBA16F fill, FP32 FMA and copy bandwidth tests, one
render graph frame each (~30 ms, headless OK). It returns a score (100 = GTX 1060 class), and `applyQuality(result)`
sets every scalability group. Thresholds: <35 Low, <80 Medium, <160 High, else Ultra; GI/Volumetrics are one step
lower below 250; RayTracing is Low without support. M4 Pro: about 35 GPix/s, 3.7 TFLOPS, 227 GB/s → score ≈ 112 →
High.

## 8. Tests & golden images

* `ox_render_tests` (CPU): cluster math (slices, point-in-cluster), CSM split scheme, sub-texel snapping stability,
  rotation invariance, atlas quadtree + importance budget, point face mapping vs shader, Halton, camera conventions,
  extract, feature registry (exclusive groups, ordering, Upscale slot, toggles, factories), cvar/scalability
  snapshot, primitives, range allocator.
* `ox_render_gpu_tests` (labels `render;gpu`, skip without Vulkan, fail on validation errors). Goldens: PBR grid,
  CSM, spot, point cubes, 256 clustered lights, IBL only, ACES vs AgX, 50 % render scale, debug lines, editor grid
  + outline. Plus picking (also at half scale), AfterOpaque feature injection + toggle cvar, compute lighting input
  (AO), cvar toggles rebuild without leaks, shadow caching, shader hot reload, benchmark, editor viewport (external
  command list, Uuid pick, hidden), 1080p perf report.
* `cmake --build build/<you> --target ox_render_tests` builds both executables; `ctest -L render`.

Writing a golden test:

```cpp
#include "render_fixture.hpp"            // RenderTest: device, renderer, World, helpers
TEST_F(RenderTest, MyEffect) {
    mesh(Primitive::Sphere, material({0.8f, 0.2f, 0.2f, 1}, 0.0f, 0.4f), {0, 0, 0});
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    environment();
    CVarScope on("r.MyEffect", "true");  // restores on scope exit
    Image img = render(camera({0, 1, 4}, {0, 0, 0}, /*EV100*/ 12.5f));
    GoldenResult g = compareGolden("my_effect", img);  // mean error ≤ 2, ≤ 1 % pixels off by > 24
    EXPECT_TRUE(g.matched) << g.message;
}
```

Run once with `OX_UPDATE_GOLDEN=1` to write `engine/render/tests/data/golden/my_effect.png`. **Look at the PNG before
committing it.** Every run also writes the actual image to `<temp>/oxwald_render_out/<name>.png`. Keep scenes
deterministic (no time-dependent inputs; TAA tests need fixed frame counts).

## 9. Performance (Apple M4 Pro, 1080p, `PerfReport1080p`)

400 instances (548 draws, 1.08 M triangles), sun + 4 cascades, 64 point lights (8 shadowed, caching off: 52 maps
per frame), IBL: **GPU 3.4 ms**. Per pass: DepthPrepass 0.14, LightCulling 0.02, Shadow.Cascades 0.18,
Shadow.Points 0.14, ShadowMask 0.19, ForwardOpaque 2.63, Sky 0.06, Tonemap 0.06, Final 0.02. CPU (renderer) about
1.2 ms.

## 10. Known limits / TODO

* No GPU-driven culling yet: CPU frustum culling + instanced batches. The data the GPU culling team needs is already
  there (mesh table with LOD0 args + meshlets, instance bounds, `drawIds` indirection compatible with indirect draws;
  no `drawIndirectCount` on MoltenVK).
* LODs: only LOD 0 is drawn (`r.ViewDistance.LODBias` reserved). Mesh hot reload waits for the GPU (`waitIdle`).
* Equirectangular HDRIs are not converted to cubes (only cube textures are used as the sky). Area lights not
  implemented (`AreaRect` ignored). Static cascade caching not implemented.
* Shadow caching state is shared between views: several views re-render local light shadows each frame.
* Point faces render all casters within range into all 6 faces (no per-face culling).
* Translucent/refractive buckets are built and sorted but not drawn yet (translucency team).
* Auto exposure must also drive `preExposure` (CPU readback hook TBD) when it lands; until then SceneColorHDR is
  pre-exposed with the camera exposure.
* MoltenVK prints a harmless "Blending is enabled for attachment with format VK_FORMAT_R32_UINT" warning when
  creating the editor prepass pipelines, although blending is disabled for that attachment.
