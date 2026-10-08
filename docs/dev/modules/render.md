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
| `VolumetricFog` | RGBA16F 3D froxels: rgb in-scatter × preExposure, a transmittance | froxel grid (`VIEW.volumetricFogGrid`) | Volumetrics (Lighting) | composite, translucency via `render/volumetrics/fog_sample.glsl`; see [render_volumetrics.md](render_volumetrics.md) |
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
| `r.Upscaler` / `r.Upscaler.Quality` | Off / Quality (Off, FSR1, DLSS, TAAU) | — (owned here, set by the runtime; implemented by postprocess-upscalers, see `render_postprocess.md`) |
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

* GPU-driven culling, LODs, meshlets, streaming, parallel recording: see §13 and `docs/dev/perf.md`. Mesh hot reload
  waits for the GPU (`waitIdle`).
* Equirectangular HDRIs are not converted to cubes (only cube textures are used as the sky). Area lights not
  implemented (`AreaRect` ignored). Static cascade caching not implemented.
* Shadow caching state is shared between views: several views re-render local light shadows each frame.
* Point faces render all casters within range into all 6 faces (no per-face culling).
* Translucent/refractive buckets are drawn by the Translucency feature (§12).
* Auto exposure drives `preExposure` (3-frame readback, see `render_postprocess.md`); without it SceneColorHDR is
  pre-exposed with the camera exposure. Temporal features rescale history by the preExposure ratio.
* MoltenVK prints a harmless "Blending is enabled for attachment with format VK_FORMAT_R32_UINT" warning when
  creating the editor prepass pipelines, although blending is disabled for that attachment.

## 11. Ray tracing

"RTX mode": the r.RayTracing checkbox swaps raster effects for ray traced ones. Code: `src/features/raytracing/`,
public headers `features/raytracing/` (`raytracing.hpp` settings/status/registration, `rt_scene.hpp` CPU AS logic,
`rt_api.hpp` integration API, `denoiser.hpp`, `ddgi.hpp`), shaders `engine/shaders/render/raytracing/`.

### Gating

A ray traced feature runs only when `RenderSettings::rayTracing` (r.RayTracing, forced off by the renderer without
ray queries) **and** `DeviceCaps::rayTracingSupported()` **and** its own cvar are set (`rt::rayTracingActive`). Each
effect sits in the exclusive group of its raster counterpart with priority 100, so toggling at runtime just
resolves different features on the next frame (graph re-declared, no restart). On MoltenVK nothing changes: the RT
features never initialize (no RT pipeline is ever created) and `RayTracingCheckboxIsInertWithoutSupport` checks the
graph and image are identical. `rt::rayTracingStatus(caps)` gives per-effect availability with human-readable
reasons (`DeviceCaps::whyRayTracingUnavailable()`, plus the RT pipeline for path tracer mode 1) for the UI.

| Feature | Group / point | Publishes | Notes |
| --- | --- | --- | --- |
| `RayTracingScene` | — / AfterDepth -1000 | `RtScene` (buffer token) | BLAS/TLAS, `RtSceneHeader`; enabled when any RT effect or the path tracer is |
| `ShadowsRT` | Shadows / Shadows | `ShadowMask` (RGBA8!), `RtLocalLightShadows` | sun: cone sampling by angular radius; local lights: 4 explicit channels or ReSTIR DI |
| `AmbientOcclusionRT` | AO / Lighting | `AO` | cosine rays, length r.RayTracing.AO.Radius |
| `GlobalIlluminationRT` | IndirectDiffuse / Lighting -100 | `IndirectDiffuse`, `RtDdgiIrradiance/Depth` | DDGI probe volume around the camera |
| `ReflectionsRT` | Reflections / Lighting 10 | `ReflectionsSpecular` | GGX VNDF, 1 bounce, hit lighting, fade to probes/IBL above MaxRoughness |
| `VolumetricsRT` | — / Lighting `kFogOrder-50` | `VolumetricFogVisibility` | the volumetrics team's documented hook (r sun, g local ratio, b sky) |
| `TranslucencyRT` | — / Translucency 1000 | `SceneColorHDR` (in place) | replaces pixels whose nearest translucent surface is refractive |
| `PathTracer` | — / BeforePostProcess -10000 | `SceneColorHDR` | r.PathTracing; progressive, ray query or RT pipeline |

### Acceleration structures (`rt_scene.hpp`, `rt_scene_gpu.cpp`)

* **BLAS per unique (submesh, LOD)**, key = `blasKey(meshInfo, index range, vertex range, deformable)`. LOD policy
  `selectBlasLod(lodCount, r.RayTracing.BLAS.LOD)`: coarsest LOD by default (RT effects tolerate simplification), Ultra
  uses LOD 0. Index ranges come from `GpuScene::meshLods`.
* **`BlasScheduler`** drives an `IBlasBackend` (mockable): per-frame build budget (count + triangles, the first build
  always runs, highest screen coverage first), compaction of static meshes (build → size query → compact copy →
  swap; the original is traced until the copy completes, then destroyed), refit of deformables every deformed frame
  with a full rebuild every `maxRefitsBeforeRebuild` refits (old BLAS traced meanwhile), eviction of BLASes unused for
  `evictAfterFrames`. The rhi backend builds synchronously (`Device::createBlas` compacts internally →
  `compactsInternally()`), the unit tests use an asynchronous mock with latencies.
* **Skinned / deformed meshes**: `RayTracingSceneApi::find(registry)->setDeformedGeometryProvider(fn)`: `fn(gpuInstance)`
  returns `DeformedGeometry{buffer, offset, vertexCount, stride, version}` (object-space positions, buffer needs
  `AccelStructInput`). Such instances get their own update-capable BLAS refitted every frame from the provided
  buffer (`CommandList::refitBlas(blas, geometry)` overload), and hit shading reads the compute-skinning output
  (`SceneHeader.skinnedVertices`, `kInstanceSkinnedOutput`). For the world-skinning arena: buffer = arena,
  offset = `paletteOffset * 40`, stride 40. Without a provider skinned meshes are traced in bind pose.
* **TLAS** (`TlasInstanceTable`): dense, sorted by GpuInstance slot (stable custom indices), rebuilt once per frame in
  the first view. Change classification: transforms / masks / SBT offsets only → update (refit); instance added or
  removed, BLAS swapped (built, compacted, rebuilt) or opacity flags changed → rebuild; plus a rebuild every
  120 updates. Grows by power of two (min 1024 instances). Instance flags: cull disabled, front = CCW,
  FORCE_OPAQUE for opaque materials, FORCE_NO_OPAQUE for alpha-tested / translucent (candidate hits).
* **Masks** (8 bit): `Opaque 1, AlphaTested 2, Translucent 4, ShadowOpaque 8, ShadowTranslucent 16`. Shadow rays use 8
  (+16 for coloured transmittance), AO/GI use 1|2, reflections/refraction/path tracing 1|2|4.
* **SBT layout** (RT pipeline): hit groups `[Opaque, AlphaTested, Translucent] × [radiance, shadow]`, instance SBT
  offset = `hitGroup * 2`, trace with offset = ray type, stride 2; miss 0 radiance, miss 1 shadow.
* **Lookup tables for hit shading via bindless** (`rt_common.glsl`): custom index → `RtInstance{gpuInstance,
  meshInfo, material, firstIndex, vertexOffset, flags, mask}` → indices (`RtSceneHeader.indices` = index arena BDA)
  → positions/attributes arenas, `SceneHeader.materials[material]` → bindless textures (explicit LOD from a ray
  cone estimate). `RtSceneHeader` (128 B, frame memory): TLAS address, instance table, index arena, instance count,
  frame, `DdgiVolume`. Pass `rt` (its address) in push constants and `.read(resources().buffer("RtScene"), …)` for
  ordering; the TLAS build pass emits the AS barriers itself (WAR against last frame's traversal included).
* Async compute builds: not used. AS storage buffers are exclusive-sharing and the render graph does not track AS
  ownership, so builds stay on the graphics queue (BLAS builds are immediate submits outside the frame graph).

### Effects

* **Shadows**: `spp` rays per pixel, sun direction sampled in the cone of `sunAngularRadius`, local lights as spheres of
  `sourceRadius`; alpha test through the candidate loop (any-hit emulation); coloured transmission through
  translucent casters (`oxRtTransmittance`: transparent `(1-a)·tint`, refractive `baseColor·transmission·Beer-Lambert`).
  The ShadowMask becomes **RGBA8 with coloured sun visibility in rgb**, signalled by view flag 256
  (`OX_VIEW_RT_SHADOW_MASK_RGB`, lighting.glsl). Local lights: `GpuShadow.kind = 2` (screen-space mask, bindless
  index in `cubeLayer`, channel in `atlasRect.x`) of a persistent per-view RGBA8 texture, also published as
  `ShadowAtlas` so the forward pass declares the read. Non screen-space consumers (translucency) see kind 2 as lit.
* **ReSTIR DI** (r.RayTracing.Shadows.ReSTIR): reservoirs over every shadow-casting local light with p̂ = unshadowed
  luminance contribution (grey dielectric surface from the G-buffer normal/roughness): 32 initial candidates,
  temporal reuse (reprojection, depth/normal validation, light entity check, M clamped to 20×), spatial reuse (4
  neighbours, 16 px). The selected light's visibility estimates the shadowed/unshadowed ratio applied to every
  shadowed local light (channel 0) — consistent, denoised.
* **Reflections**: VNDF-sampled GGX lobe, 1 bounce with `oxRtShadeHit` (sun + one random local light with shadow rays,
  emissive, DDGI or SH diffuse, prefiltered specular), sky on miss, firefly clamp; weight fades to 0 between 75 % and
  100 % of MaxRoughness (forward keeps probes / IBL). Denoised with a short history (12) because surface-motion
  reprojection smears mirror reflections.
* **AO**: cosine rays of `Radius`, occlusion softened with hit distance, denoised into R8.
* **GI (DDGI)**: layout in `ddgi.hpp` (the reflections-ao team's baked volumes use a different SH L1 layout). Probe
  window `ProbesXZ × ProbesY × ProbesXZ` with `ProbeSpacing` scrolls with the camera (modular slots; slots that
  change world coordinate restart without hysteresis, tracked in the probe state buffer). Rays: rotated spherical
  Fibonacci, hit radiance = direct + emissive + albedo × last frame's DDGI (infinite bounces); irradiance atlas 8²
  (+border, stored with gamma 1/5), depth moments 16² (Chebyshev visibility), hysteresis 0.97. Applied per pixel into
  IndirectDiffuse (value = irradiance/π); other RT passes read it through `RtSceneHeader.ddgi`.
* **Translucency**: primary ray to the opaque depth with the translucent mask; refractive hits get Fresnel-weighted
  reflection (sees off-screen objects) + refraction path (Snell, internal bounces up to MaxBounces, TIR, Beer-Lambert,
  exit refraction, hit lighting with coloured shadows) + sun highlight. Deterministic (no noise, no denoiser).
  `rt::rayTracedRefractionActive()` lets the raster Translucency feature skip refractive draws.
* **Volumetric visibility**: per froxel of the Volumetrics grid (`froxelGridFromCVars`), r = sun shadow ray, g = local
  light ratio (lights of the froxel's cluster sampled ∝ unshadowed contribution), b = sky visibility (cosine rays,
  32 m). Shadows from any TLAS object, on or off screen.
* **Path tracer** (r.PathTracing 1): GGX (VNDF) + Lambert, smooth dielectric for refractive/transparent, Beer-Lambert,
  sun as a spherical cap with NEE + BSDF sampling combined by the power heuristic, local lights NEE (not geometry),
  emissive surfaces and sky via BSDF sampling, Russian roulette from bounce 4, firefly clamp on indirect paths.
  Mode 0 ray query megakernel, mode 1 RT pipeline (`path_trace.rgen/.rchit/.rahit/.rmiss`, `path_shadow.rmiss`;
  opaque shadows only). RGBA32F accumulation per view; `AccumulationTracker` restarts on camera, scene structure,
  moving objects, lights, environment or settings changes; stops at MaxSamples.

### Denoiser (`denoiser.hpp`, works on every device)

SVGF-style: temporal (Velocity reprojection, 2×2 bilinear taps validated by previous linear depth/normal, 3×3
fallback, alpha = max(1/len, 1/maxHistory), luminance moments) → variance (temporal, or 7×7 spatial below 4 frames)
→ À-trous (5×5 B3, steps 1..16, edge stops on depth gradient / normal / variance-guided luminance; iteration 0 feeds
the history) → optional joint-bilateral upsample (half resolution) → RGBA16F / RGBA8 / R8 output. `lumaWeights`
selects the guiding signal (colour, scalar AO, 4 shadow channels).

### CVars (Scalability::RayTracing, Low / Medium / High / Ultra)

| CVar | Default | Levels |
| --- | --- | --- |
| `r.RayTracing.Shadows` / `.Reflections` / `.AO` | on | on,on,on,on / off,on,on,on / off,on,on,on |
| `r.RayTracing.GI` / `.Translucency` / `.Volumetrics` | on | off,off,on,on / off,on,on,on / off,off,on,on |
| `r.RayTracing.Shadows.SamplesPerPixel` / `.MaxLocalLights` | 1 / 4 | 1,1,1,2 / 1,2,4,4 |
| `r.RayTracing.Shadows.ReSTIR` / `.Colored` | off / on | off,off,off,on / off,on,on,on |
| `r.RayTracing.Shadows.ResolutionScale` | 100 | 50,100,100,100 (50 = half res + upsample) |
| `r.RayTracing.Reflections.MaxRoughness` / `.SamplesPerPixel` / `.ResolutionScale` | 0.6 / 1 / 100 | 0.3,0.4,0.6,0.8 / 1,1,1,2 / 50,50,100,100 |
| `r.RayTracing.AO.SamplesPerPixel` / `.ResolutionScale` | 1 / 100 | 1,1,2,4 / 50,50,100,100 |
| `r.RayTracing.GI.RaysPerProbe` / `.ProbesXZ` / `.ProbesY` / `.ResolutionScale` | 128 / 24 / 8 / 100 | 64,96,128,256 / 12,16,24,32 / 6,8,8,12 / 50,50,100,100 |
| `r.RayTracing.Translucency.MaxBounces` | 4 | 2,3,4,6 |
| `r.RayTracing.Volumetrics.LocalSamples` / `.SkyRays` | 2 / 1 | 1,1,2,4 / 0,1,1,2 |
| `r.RayTracing.Denoiser.Iterations` | 4 | 3,4,4,5 |
| `r.RayTracing.BLAS.LOD` | -1 (coarsest) | -1,-1,-1,0 |
| no scalability | | `.Shadows/.Reflections/.AO.Denoiser` (on), `AO.Radius` 1, `GI.ProbeSpacing` 2, `GI.Hysteresis` 0.97, `Denoiser.MaxHistory` 32, `BLAS.BuildsPerFrame` 16, `r.PathTracing` off, `.Mode` RayQuery/Pipeline, `.MaxBounces` 8, `.SamplesPerFrame` 1, `.MaxSamples` 4096 |

### What is verified where

Verified on the Mac (MoltenVK, no ray tracing), `ctest -L render`:
* every RT shader and define variant compiles to optimized SPIR-V (`RtShaders.*`, plus the rhi all-shaders test):
  ray query compute shaders and all RT pipeline stages, push blocks ≤ 128 B;
* CPU logic: TLAS masks / SBT offsets / transform packing / update-vs-rebuild classification, BLAS scheduling
  (budget, priority, compaction lifecycle, refit/rebuild of deformables, eviction) against an asynchronous mock,
  LOD policy, accumulation resets, DDGI addressing / scrolling / atlas layout, scalability tables, availability
  reasons, exclusive-group gating with and without RT caps;
* GPU, plain compute: the denoiser on stochastic 1-spp AO (RMSE vs 256-spp reference 0.188 → 0.026; no ghosting
  after a camera move: |denoised − new| 0.054 vs |denoised − old| 0.31; half-res upsample 0.056), DDGI blending +
  border + apply with synthetic probe rays (L = max(y,0) → 0.659 for 2/3; constant field; scrolling resets), ReSTIR
  reservoirs sample lights ∝ their contribution (0.784 for a 4:1 pair) with spatial/temporal reuse and M clamping,
  and r.RayTracing being inert without support.
Needs an RTX-class GPU (written, `GTEST_SKIP` here): BLAS/TLAS builds and refits, every ray query / RT pipeline
dispatch, effect images (RT shadows vs shadow maps, RTAO darkening, off-screen reflections, GI colour bleeding,
refraction, ReSTIR with 32 lights, path tracer convergence + reset), runtime toggling without leaks, TLAS instance
count == live instances.

### Manual checklist on an NVIDIA RTX machine

1. `ox_render_gpu_tests --gtest_filter=RayTracingTest.*`: nothing skipped, no validation errors; look at
   `<temp>/oxwald_render_out/rt_shadows.png`.
2. Player/editor: toggle r.RayTracing at runtime several times — no hitch beyond BLAS builds, VRAM stable
   (`r.GpuTimings`, stats overlay), all RT passes listed (`RT.BuildAS`, `RT.Shadows`, …).
3. Shadows: contact hardening with sun `sourceRadius` 0.5° vs 3°; alpha-tested foliage shadows have holes; glass
   casts tinted shadows; with r.RayTracing.Shadows.ReSTIR and 100+ shadowed point lights the shadows stay stable.
4. Reflections: mirror floor shows objects behind the camera; roughness sweep 0 → 1 fades smoothly into probes;
   no fireflies on the sun; camera motion: acceptable smearing on mirrors.
5. AO/GI: contact darkening without screen-space halos; red wall bleeds onto a white floor; walking through a level
   the DDGI window scrolls without popping (new slices converge in ~1 s); no light leaks through thin walls.
6. Glass sphere: refraction flips the background, TIR ring at grazing angles, coloured absorption with
   absorptionDistance, reflections of off-screen objects.
7. Volumetric fog with RT: god rays from off-screen occluders, local light shafts.
8. r.PathTracing 1 (both Mode 0 and Mode 1): converges to a noise-free image in ~1000 spp, resets on any camera or
   object move, roughly matches the raster image (raster lacks GI / soft shadows).
9. Skinned character with a DeformedGeometryProvider: shadow follows the animation (refit), no stale bind pose.
10. Nsight Graphics capture: TLAS instance count = visible instances, compacted BLAS sizes ~50 % of uncompacted.

### Performance (Apple M4 Pro, 1080p, plain-compute parts only)

Denoiser on an RGBA16F 1080p signal (`DenoiserCost1080p`): temporal 0.56 ms, variance 0.29 ms, À-trous 1.2 ms per
iteration → 5.7 ms with 4 iterations (half resolution: ~1/4). Ray traced passes cannot be measured here.

### Shared files touched (additive)

`common/lighting.glsl` (view flag 256 → RGB sun mask, GpuShadow kind 2), `gpu_scene.hpp/.cpp` (`positionBuffer()`,
AccelStructInput on the position/index arenas — ignored by rhi without AS support), rhi `CommandList::refitBlas(blas,
geometry)` overload, `renderer.cpp` registration line + include.

### Known limits / TODO

* ShadowsRT replaces shadow maps entirely: without ReSTIR only 4 local lights per view are ray traced (the rest are
  unshadowed); non screen-space consumers (translucency) get unshadowed sun/local light in RT mode.
* The raster passes still run under the path tracer (the image is replaced before post-processing).
* DDGI: single camera-centred volume, no per-probe relocation / classification; translucency has no denoiser for
  rough glass (deterministic smooth refraction only).
* No async-compute AS builds (see above); AS memory sizes are estimates (rhi does not report them).
* Pass writable buffer references in push constants: storing through a reference read from another buffer was
  silently dropped on MoltenVK (found by the DDGI test).

## 12. Translucency, water & particles

Code `src/features/translucency/`, shaders `engine/shaders/render/translucency/`, public API
`features/translucency/translucency.hpp`, components `components/translucency.hpp` (`ParticleEmitterComponent`,
`WaterSurfaceComponent`, reflected; extracted by a hook into the `TranslucencySnapshot` RenderSnapshot extension).

| Feature (order) | Points | What |
| --- | --- | --- |
| `Translucency` (0) | AfterOpaque, Translucency | `SceneColorRefraction` (RGBA16F mip chain: copy + 2×2 box, then 4×4 binomial levels) and `SceneDepthCopy` (R32F) declared every frame, culled when unused; `RefractionBackDepth` (back faces of closed refractive meshes); refractive objects; transparent objects; sets `kViewFlagHashedAlpha` |
| `Water` (-50) | AfterOpaque, Translucency | `Water.Caustics` (multiplicative: water-column Beer-Lambert × animated caustics on underwater geometry, before the refraction copy); `Water.Surface` |
| `Particles` (100) | AfterOpaque, Translucency | `Particles.Simulate` (first view of the frame), `Particles.LowResDepth`, `Particles.Render`, `Particles.Composite` |
| `Underwater` (900) | Translucency | `Water.Underwater` post effect when the camera is below a surface (republishes `SceneColorHDR`) |

* **Refractive** (`BlendMode::Refractive`, blend off, back to front): refracted ray `refract(-V, N, 1/ior)` followed
  through the object (thickness) and to the background (`r.Refraction.MaxDistance`), foreground-leak check against
  `SceneDepthCopy`, roughness → mip (`lod = roughness × (mips-1)`), Beer-Lambert `pow(absorptionColor,
  thickness / absorptionDistance)` with thickness = back-face minus front depth (else `material.thickness`), Schlick
  with `F0 = ((ior-1)/(ior+1))²` (transmitted angle when exiting), TIR → environment reflection. Material
  `transmission` 0 is treated as 1 for refractive materials. Reflections: prefiltered IBL (reflection probes are not
  exposed per pixel to translucency yet).
* **Transparent**: `r.Translucency.Method` Auto/OIT/Sorted. Weighted Blended OIT (McGuire 2013 eq. 10 weights,
  accumulation RGBA16F ONE/ONE + revealage R8 ZERO/ONE_MINUS_SRC_COLOR, resolve SRC_ALPHA/ONE_MINUS_SRC_ALPHA); Auto
  uses the sorted path (premultiplied, back to front) when ≤ `r.Translucency.SortedMaxInstances` instances are
  visible. Materials with `kMaterialSortedTranslucency` (assets `renderQueueOffset != 0`) always take the sorted path.
* Translucent shading = `oxEvaluateLighting(screenSpace = false)` (clusters, CSM/spot/point shadows, SH + prefiltered
  IBL) + fog: `translucency/fog.glsl` uses `render/volumetrics/fog_sample.glsl` (`oxEvaluateVolumetricFog`) when that
  file declares it (`OX_HAS_VOLUMETRIC_FOG_SAMPLE`, detected once per process), else the height fog of the opaque pass.
* **Water**: dense camera-centred grid (cells grow with distance, clamped to the rectangle; `r.Water.GridResolution`),
  Gerstner displacement with the world module's `world/gerstner.glsl` layout (per-pixel/vertex evaluation reads the
  waves from the buffer, same math), detail normals (procedural tileable map or `normalMap`), refraction of the
  bottom with view-path absorption + in-scattering, reflection = planar (`render/reflections/planar.glsl` contract,
  `OX_HAS_PLANAR_REFLECTIONS`) → SSR on `SceneColorRefraction` (`r.Water.SSRSteps`) → prefiltered environment,
  Schlick F0 0.02 with TIR from below, shore foam (water depth over `SceneDepthCopy`) + crest foam, writes depth.
  Gameplay: `water_bridge.cpp` turns `gameplay::WorldRenderData::water` (WaterComponent waves, the buoyancy ones)
  into surfaces when render links gameplay; a `WaterSurfaceComponent` on the same entity supplies the look.
* **Particles**: per emitter persistent buffers (particles 64 B, dead list + two alive lists, counters with
  `VkDrawIndirectCommand` / `VkDrawIndexedIndirectCommand` written by the simulation → `drawIndirect` with count 1,
  no `drawIndirectCount`). Spawn (rate + bursts, CPU count), shapes point/sphere/cone/box/mesh surface, gravity, drag,
  curl-noise turbulence, depth-buffer collisions (bounce with friction / kill, normal from `Normals`), size/colour
  curves baked into a 64×2 RGBA16F gradient, flipbooks (two frames blended), billboard / velocity-stretched / mesh
  rendering, lit (per-vertex: sun + CSM, clustered lights, SH) or emissive, additive/alpha/premultiplied through one
  premultiplied blend state, soft particles, optional back-to-front sort (≤ 2048 particles, one-workgroup bitonic in
  shared memory). Rendered at 1/`r.Particles.ResolutionDivisor` into `ParticlesLowRes` against a closest-depth
  downsample, composited with depth-aware bilateral upsampling.
* **Alpha test**: `r.AlphaTest.Dither` (Off/On/Auto = with TAA) enables hashed alpha in `passes/depth_prepass.frag`
  (alpha sharpened by `fwidth`, per-pixel per-frame threshold); the forward pass inherits it through depth EQUAL.
  Alpha-to-coverage needs an MSAA path, which the renderer does not have.

| CVar | Default | Group: Low / Medium / High / Ultra |
| --- | --- | --- |
| `r.Translucency.Method`, `.SortedMaxInstances` | Auto, 4 | — |
| `r.Refraction`, `.Strength`, `.MaxDistance` | on, 1, 4 m | — |
| `r.Refraction.Mips` | 6 | Reflections: 3, 5, 6, 7 |
| `r.Refraction.BackfaceDepth` | on | Shading: off, on, on, on |
| `r.AlphaTest.Dither` | Auto | — |
| `r.Water`, `r.Water.Underwater`, `r.Water.MaxExtent` | on, on, 2000 m | — |
| `r.Water.GridResolution` | 256 | Shading: 96, 160, 256, 384 |
| `r.Water.SSRSteps` | 12 | Reflections: 0, 8, 12, 20 |
| `r.Water.Caustics` | on | Effects: off, on, on, on |
| `r.Particles` | on | — |
| `r.Particles.Budget` (per emitter) | 262144 | Effects: 16384, 65536, 262144, 1048576 |
| `r.Particles.ResolutionDivisor` | 2 | Effects: 4, 2, 2, 1 |
| `r.Particles.Collision`, `.Lighting`, `.SoftParticles` | on | Effects: off, on, on, on |
| `r.Particles.Sorting` | on | Effects: off, off, on, on |

Tests: `tests/translucency_cpu_tests.cpp`, `tests/gpu/translucency_tests.cpp` (goldens `translucency_*`: glass panes
OIT + sorted, refractive sphere, frosted glass, water, underwater, particles, foliage). 1080p on the M4 Pro
(`ParticlesTest.PerfReport1080p`, High, averaged): refraction source 0.31 + depth copy 0.12, back faces 0.02,
refractive 0.14, sorted glass 0.15, water surface 0.48 (Ultra 0.73), particles simulate 0.03 + render 0.31 +
low-res depth 0.06 + composite 0.06 ms.

## 13. GPU-driven rendering & performance (area gpu-driven)

Code: `src/features/gpu_driven/` (`gpu_driven.*` culling, `texture_streaming.*`), shaders `shaders/render/gpu_driven/`,
public helpers `<oxwald/render/features/gpu_driven/gpu_driven.hpp>`. Measurements: `docs/dev/perf.md`.

* **Draw path (`r.GpuDriven`, default on; 0 = the CPU path, kept as fallback, also used automatically when the cull
  pipeline is unavailable).** Persistent *draw sets* (main: Opaque+Masked; shadow: casters) group instances into
  batches (bucket, variant, mesh, material) with one indexed indirect command per LOD; rebuilt only when
  `GpuScene::structureVersion()` changes (no per-instance CPU work per frame). Culling jobs (`cull.comp`, stages
  RESET → CULL → PREFIX → SCATTER, all jobs of a pass in one dispatch per stage) do flags / frustum / sphere /
  draw distance / LOD selection and write compacted instance ids. Draws: `vkCmdDrawIndexedIndirect` over a fixed max
  count with zero-instance padding (one call per pipeline variant), `drawIndexedIndirectCount` with compacted
  commands when the device supports it (`r.GpuDriven.DrawCount`).
* **Two-phase occlusion (`r.GpuDriven.Occlusion`)**: phase 1 draws what was visible last frame (per-view visibility
  buffer), `HiZ.Early` builds a half-resolution pyramid from that depth, phase 2 (`GpuCull.Late`) tests everything
  against it and `DepthPrepass.Late` draws the newly visible instances; ForwardOpaque draws both lists. No reprojection,
  no popping on disocclusion (tested). Shadows use frustum/sphere culling per cascade / spot / point light (no HiZ).
* **LOD**: screen-space error (`selectLod` in `gpu_types.hpp`, mirrored in `cull_common.glsl`; CPU lists use the same
  function): coarsest LOD with `error · scale · projScale / distance ≤ r.GpuDriven.LODErrorPixels · 2^LODBias`.
  `GpuScene::meshLods()` / `meshLodAddress()`: `kMaxMeshLods` `GpuMeshLod` per mesh info (index ranges + meshlets).
  Shadow casters use the camera's LOD.
* **Meshlets (`r.GpuDriven.Meshlets`, default off)**: instances whose LOD has meshlets go to `meshlet_cull.comp`
  (frustum, backface cone, HiZ in phase 2) which writes a compacted index buffer (`slot << 7 | local vertex`, drawn by
  `meshlet.vert`, one indirect draw per variant). `r.GpuDriven.MeshShaders` uses `meshlet.task` / `meshlet.mesh`
  (VK_EXT_mesh_shader) instead — compile-tested here, only enabled on devices with mesh + task shaders.
* **API for features**: `FeatureContext::cullDrawList(filter, instanceMultiplier)` (GPU-culled when possible,
  otherwise `buildDrawList`), `lodSelection()`, `DrawFilter::lod`; `DrawList` gained indirect fields (`gpuDriven()`,
  `indirectRuns`, …): always draw lists with `drawBatches()`. `drawLists()` still returns the CPU-culled lists of all
  four buckets (built lazily when the frame is GPU-driven).
* **Async compute**: `asyncComputeHint(caps)` (`r.AsyncCompute` Off/On/Auto; Auto = off on portability devices).
  LightCulling and HiZ use it. The render graph now splits a batch before the first pass that waits on another
  queue, and `RenderStats` reports `asyncComputeMs` / `asyncOverlapMs` / `gpuFrameWallMs` from timestamps.
* **CPU**: `ExtractOptions::jobs` (+ `parallelThreshold`) extracts mesh renderers with `parallelFor`; extract and the
  camera draw lists allocate nothing in steady state (test with a counting `operator new`). `r.ParallelRecording`
  (Off/On/Auto, needs `RendererDesc::jobs`) records large CPU-path prepass/forward passes into secondary command
  lists on the job system.
* **Texture streaming (`r.Streaming*`, budget `r.Streaming.PoolSizeMB` in Scalability::Textures 256/512/1024/2048)**:
  textures start with their ≤ 64 px tail; wanted mips from screen-space texel density (bounding sphere × UV tiling),
  budget enforced by coarsening the least important textures, residency changes via async uploads + swap one frame
  later (`GpuResourceCache::createTextureFromData/replaceTexture`, `ITextureStreamingHook`). Distance heuristic, no
  GPU feedback pass; data comes from the CPU copy kept by the streamer.
* **Async PSOs**: meshlet / mesh shader pipelines compile on the job system (`Device::create*PipelineAsync`) when
  `RendererDesc::jobs` is set; the instanced path is the placeholder until they are ready
  (`RenderStats::pipelinesCompiling`). Persistent cache: `DeviceDesc::pipelineCachePath` (set it in the runtime).
* **Stats**: `RenderStats::gpuCulling` (tested / frustum / occluded / visible, draws, triangles, average LOD, meshlets,
  shadow jobs; read back `latencyFrames` late), `indirectDrawCalls`, `indirectCommands`, `streaming`, `vram`
  (geometry, textures, gpu-driven buffers, transient targets), `parallelRecordedChunks`, async timings.

| CVar | Default | Group: levels |
| --- | --- | --- |
| `r.GpuDriven`, `.Occlusion`, `.Shadows` | on, on, on | — |
| `r.GpuDriven.LODErrorPixels` | 1 | ViewDistance: 2, 1.5, 1, 0.75 |
| `r.GpuDriven.Meshlets`, `.MeshShaders`, `.MeshletIndexBudgetMB` | off, off, 64 | — |
| `r.GpuDriven.DrawCount` | Auto (MaxCount / IndirectCount) | — |
| `r.AsyncCompute`, `r.ParallelRecording` (+ `.MinBatches` 256) | Auto, Auto | — |
| `r.Streaming`, `.PoolSizeMB`, `.TailSize`, `.MipBias`, `.MaxUploadMBPerFrame`, `.DropDelayFrames` | on, 1024, 64, 0, 32, 30 | PoolSizeMB: Textures 256/512/1024/2048 |

MoltenVK pitfalls found here (see perf.md): SPIRV-Cross forwards loads through buffer references past later stores to
the same address (use atomics / load into a local before storing), loading a whole struct holding 64-bit references
through a reference miscompiles, arrays of buffer references inside a buffer-reference block crash spirv-reflect
(store `uint64_t` and cast), and 64-bit loads need `buffer_reference_align = 8`.

Tests: `tests/gpu/gpu_driven_tests.cpp` (CPU vs GPU path images, occlusion behind a wall, same-frame disocclusion,
LOD switch distance, meshlets vs instanced, runtime toggle, steady-state allocations, parallel recording, streaming
budget, async compute, async PSO, mesh shader compile, padding cost, stress scene). Goldens `gpu_driven_shadows`,
`gpu_driven_streaming`.


## World: terrain, vegetation, sky, skinning (world-skinning area)

Code `src/features/world/`, public header `features/world/world_skinning.hpp`, components `components/world.hpp`,
shaders `engine/shaders/render/world/`. Registered by `registerWorldSkinningFeatures(f)`. Render links `Oxwald::world`
(and gameplay for the bridge) when those modules are configured (`OX_RENDER_HAS_WORLD`, `OX_RENDER_HAS_GAMEPLAY`).

| Feature | Point / order | What |
| --- | --- | --- |
| `Skinning` | PreDepth -2000 | compute skinning (LBS / DQS) into the `GpuSkinnedVertex` arena, publishes `SkinnedVertices` |
| `WorldSky` | PreDepth + AfterOpaque -1100, group `Sky` prio 100 | sky cube for the IBL, sky pass, aerial perspective; falls back to the core sky without a world sky |
| `WorldGeometry` | AfterDepth + AfterOpaque -10000 | `World.Prepass` (LOAD Depth/Normals/Velocity/EntityID) + `World.HiZ`, `World.Forward` (before the sky) |
| `WorldShadows` | Shadows 100 | `World.ShadowCascades` (LOAD into ShadowCascades) + `World.ShadowMask` (replaces the core mask) |

**Data.** `WorldSnapshot` (snapshot extension): terrains (immutable heightfield/splat copies + version / dirty rect
since `dirtySinceVersion`: partial uploads when the renderer saw that version, else full), vegetation batches
(`world::VegetationInstanceGpu` + cells), `VegetationPrototypeDesc`s (`VegetationPrototypesComponent`; missing
prototypes use built-in procedural tree / grass / bush), sky, wind, grass interactors. Call
`finalizeWorldSnapshot()` after filling it (sets `SnapshotEnvironment::iblKey`, adds an environment for a lone sky).
In the runtime, `gameplay_bridge.cpp` (an `ExtractHookEx`, `ExtractOptions::services`) fills it from gameplay's
`WorldRenderData` and adds `SkinnedMeshComponent`s as `SnapshotMesh`es with current + previous palettes
(`SkinningSnapshot` carries dual-quaternion entities). It also forwards the wind to `volumetrics::setWorldWind`.

**Terrain.** R32F/R16 heightmap + RGBA8 normal map (compute, mips) + R8 hole mask + 2 RGBA8 splat textures per terrain,
updated by region copies (`immediateSubmit`, rare). CDLOD selection runs on the render thread per view / cascade
(`r.Terrain.LODScale` scales the ranges; shadow cascades use the main camera for LOD so shadows match). Splat
shading samples the layers' scene materials through bindless indices (no texture arrays needed: layers may differ in
size), strongest `r.Terrain.MaxLayers` layers, height blend (ORM occlusion or albedo luminance as height),
triplanar above `triplanarSlopeDeg`, rotated second sample for tiling breakup, low-frequency macro variation.
Optional tessellation (`r.Terrain.Tessellation`, Ultra; needs `DeviceCaps::tessellationShader`, rhi got
`GraphicsPipelineDesc::tessControl/tessEval/patchControlPoints`): LOD-0 patches within 40 m, distance-based factors
falling to 1 at the range (crack-free against untessellated patches), dominant layer height × `tessellationHeight`.

**Vegetation.** Instances live in one arena; per view / cascade the CPU culls cells (frustum + distance → possible LOD
range → slot capacity), `veg_cull.comp` tests every instance (sphere vs planes, density thinning by `random`, LOD +
fade mirroring `world::selectVegetationLod`) and appends `(instance, fade)` records to (prototype, LOD) slots,
`veg_args.comp` writes instance counts into a fixed list of `VkDrawIndexedIndirectCommand`s (one per submesh and LOD,
no drawIndirectCount). Cross-fade: dithered (IGN) complementary masks. Impostors: hemi-octahedral 8×8 frames ×128²
(albedo sRGB + object normal), baked in-frame (`Vegetation.ImpostorBake`) once per prototype mesh set, 4-frame blend.
Wind: `world/wind.glsl` trunk sway (height²), branch / leaf flutter from vertex colour R/G/B; grass bends away from
interactors. Foliage normals stay canopy-outward and are mirrored to the viewer side of the card; back-lit leaves
get the sun only through the translucency term.

**Sky.** Preetham × twilight factor (one decade per 2.5° below the horizon), night + moonlit sky, stars in the
celestial frame (`starsRotation`), moon disc lit by the sun (phase from geometry), sun disc with limb darkening. The
dome (no discs) goes into a radiance cube re-rendered when `worldSkyIblKey` changes (sun/moon moved by
`r.Sky.IBLUpdateDegrees`); the view constants then point the core IBL capture / translucency at it (`skyMode` 1).
Aerial perspective: Rayleigh + haze extinction with an 8 km scale height, in-scatter from the cube, skipped when
`VolumetricFog` is active.

**Skinning.** `GpuScene::setSkinOutputResolver` hands every skinned instance a double-buffered region (swapped each
frame; previous palette skinned too on the first frame); instances get `kInstanceSkinnedOutput` and
`oxFetchVertex` reads `SceneHeader.skinnedVertices` (model space, 40 B: position, normal, tangent) for current and
previous positions → motion vectors. **Ray tracing hook**: `ISkinnedOutputs` (`renderer.features().find("Skinning")`)
lists outputs; the feature also installs `rt::RayTracingSceneApi::setDeformedGeometryProvider`: buffer = arena
(`AccelStructInput` when AS are supported), offset = `paletteOffset × 40` of the instance, stride 40, vertex count of
the whole mesh, version = frame stamp. Declare a read on `SkinnedVertices` for the barrier. Without compute
(`r.Skinning.Compute 0`) the vertex shader skins (linear only).

**CVars** (Low / Medium / High / Ultra): Foliage — `r.Foliage.Density` 0.35/0.6/0.85/1, `.DrawDistanceScale` and
`.ImpostorDistanceScale` 0.5/0.75/1/1.5, `.Grass` and `.Shadows` off/on/on/on; ViewDistance — `r.Terrain.LODScale`
0.5/0.75/1/1.5; Shading — `r.Terrain.MaxLayers` 2/3/4/8, `.Triplanar` off/on/on/on, `.Tessellation` off/off/off/on,
`r.Sky.CubeSize` 64/128/128/256; Volumetrics — `r.Sky.AerialPerspective` off/on/on/on. Ungrouped: `r.Terrain`,
`r.Terrain.Shadows`, `r.Foliage`, `r.Foliage.Impostors`, `r.Sky.AerialPerspective.Density`, `r.Sky.IBLUpdateDegrees`,
`r.Skinning.Compute`.

**Tests** (`world_skinning_*`): goldens `world-skinning_terrain_splat_csm`, `_terrain_shadowmask`,
`_terrain_tessellation`, `_vegetation_impostors`, `_sky_noon/_sunset/_night`, `_skinned_pose_a/_b`,
`_skinned_velocity`; dirty-rect upload == full upload; IBL key throttling; DQS vs LBS; RT output exposure;
`PerfReport1080p` (2 km terrain, 20k trees, 25k grass clumps, sky: ≈7.5 ms GPU on M4 Pro, World.Forward ≈4.5 ms).

**Limits / TODO.** Terrain/vegetation are not in spot/point shadow maps or GPU-driven culling yet (WorldGeometry
draws its own passes); HiZ / ShadowMask are rebuilt rather than extended. Impostors use a single bake resolution and
no depth. Tessellated patches may show pinhole cracks inside a patch (barycentric order); only the prepass /
forward are tessellated, shadows use the base mesh. The bridge copies heightfields on every version change
(brush strokes on 2k terrains cost a few ms per edited frame). Grass is the dominant forward cost (thin blades,
quad overdraw).
