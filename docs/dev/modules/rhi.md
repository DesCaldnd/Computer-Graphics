# rhi — Vulkan device, resources, bindless, pipelines, render graph

Target `Oxwald::rhi` (`engine/rhi`), plus `Oxwald::rhi_glfw` (GLFW surface provider, separate so the core library
stays window-system agnostic). Namespace `ox::rhi`. Thin layer over Vulkan: it does not hide `Vk*` types completely
(formats, layouts, blend factors are Vulkan enums) but owns every object behind generational handles.

## Building blocks

| Header | What |
| --- | --- |
| `vulkan.hpp` | `volk.h` with `VK_NO_PROTOTYPES`. Never include `<vulkan/vulkan.h>` with prototypes, never link `Vulkan::Vulkan`. |
| `environment.hpp` | `configureVulkanEnvironment()`, `initializeVulkanLoader()` — points the loader at MoltenVK + validation layer. |
| `device.hpp` | `Device` (instance, queues, VMA, bindless heap, frames, submission, uploads, pipelines, AS), `DeviceDesc`. |
| `device_caps.hpp` | `DeviceCaps` incl. `rayTracingSupported()` / `whyRayTracingUnavailable()`, `toString()`. |
| `handles.hpp` | `BufferHandle`, `TextureHandle`, `SamplerHandle`, `PipelineHandle`, `AccelStructHandle` (index + generation). |
| `types.hpp`, `format.hpp`, `access.hpp` | descriptors, format table, `Access` → (sync2 stage, access, layout). |
| `command_list.hpp` | `CommandList`: barriers, dynamic rendering, draws/indirect/indirect-count/mesh, dispatch, copies/blits/clears, mips, labels, timestamps, TLAS build. |
| `pipeline.hpp` | `GraphicsPipelineDesc`, `ComputePipelineDesc`, `RayTracingPipelineDesc`, blend/depth/raster state. |
| `shader_compiler.hpp` | shaderc GLSL→SPIR-V, includes, defines, SPIR-V disk cache, spirv-reflect validation. |
| `swapchain.hpp` | `ISurfaceProvider`, `Swapchain` (present modes, resize, sRGB/HDR10/scRGB). |
| `render_graph.hpp` | `RenderGraph`, `PassBuilder`, `PassContext`, `RenderGraphPlan`. |
| `acceleration_structure.hpp` | `BlasDesc`, `TlasDesc`, `TlasInstance`. |
| `profiling.hpp` | `GpuTiming`, `GpuMemoryStats`, `RenderDocCapture`. |
| `glfw_surface.hpp` (rhi_glfw) | `initGlfwVulkan()`, `GlfwSurfaceProvider`. |

## Device, frames, submission

```cpp
ox::rhi::DeviceDesc desc;            // headless by default (tests, tools)
desc.surface = &glfwProvider;        // or the editor's QVulkanInstance-based provider
desc.pipelineCachePath = projectCacheDir / "pipelines.bin";
auto device = ox::rhi::Device::create(desc);
auto swapchain = ox::rhi::Swapchain::create(*device, {ox::rhi::PresentMode::Mailbox});

while (running) {
    device->beginFrame();                 // waits for frame N-framesInFlight, recycles command lists,
                                          // runs deferred destruction, swaps hot-reloaded pipelines
    if (!swapchain->acquire()) { device->endFrame(); continue; } // minimized / recreated
    graph.reset();  /* declare passes */
    graph.execute(*device, {.swapchain = swapchain.get()});
    swapchain->present();
    device->endFrame();                   // flushes async uploads, collects Tracy GPU zones
}
```

* One timeline semaphore per physical queue. `submit()` returns a `TimelinePoint`; `SubmitInfo::waits` adds
  cross-queue dependencies. `immediateSubmit(fn)` records + submits + waits (tools, tests, loading).
* Queues: graphics, async compute and transfer get separate `VkQueue`s when available (MoltenVK exposes 4 identical
  families with one queue each → three different families, so queue-family ownership transfers are exercised on Mac).
* `destroy(handle)` is deferred until the GPU finished every submission that could reference the object; bindless
  indices are recycled at the same time. Stale handles resolve to nothing (`isAlive()` is false).
* Only one `Device` may exist at a time (volk keeps global function pointers).
* Texture state tracking: `CommandList::transition(tex, Access)` emits a sync2 barrier from the tracked state; the render
  graph updates the tracking for imported textures at the end of a frame.

## Bindless model

One global descriptor set (set 0, update-after-bind, partially bound) + 128 bytes of push constants for every stage:

| binding | content |
| --- | --- |
| 0 | sampled images (`Device::sampledIndex(tex)`, assigned at creation when `TextureUsage::Sampled`) |
| 1 | storage images (`Device::storageIndex(tex, mip)`, created on first request) |
| 2 | samplers (`Device::samplerIndex(sampler)`; indices 0–5 are `DefaultSampler`) |

Buffers are never descriptors: pass `Device::address(buffer)` (buffer device address) in push constants and declare
`buffer_reference` types. Vertex pulling through BDA is the default; fixed-function vertex input remains available.
Pipelines whose shaders declare other descriptors are rejected at creation (spirv-reflect validation).

```glsl
#version 460
#include <common/bindless.glsl>

struct Vertex { vec3 position; vec2 uv; };
OX_READONLY_BUFFER(Vertices, { Vertex v[]; });
OX_PUSH_CONSTANTS({ Vertices vertices; mat4 viewProj; uint albedo; });   // ≤ 128 bytes, scalar layout

layout(location = 0) out vec2 uv;
void main() {
    Vertex vtx = pc.vertices.v[gl_VertexIndex];
    uv = vtx.uv;
    gl_Position = pc.viewProj * vec4(vtx.position, 1.0);
}
// fragment: outColor = OX_SAMPLE_2D(pc.albedo, OX_SAMPLER_ANISO_REPEAT, uv);
```

```cpp
struct PC { VkDeviceAddress vertices; glm::mat4 viewProj; u32 albedo; };
cmd.bindPipeline(meshPipeline);                         // also binds the bindless set
cmd.pushConstants(PC{device.address(vb), viewProj, device.sampledIndex(albedoTex)});
cmd.draw(vertexCount);
```

Storage images: `OX_IMAGE_STORE_2D(rgba16f, pc.target, coord, value)` (aliases exist for rgba8, rgba16f, rgba32f,
r32f, r32ui; add more in `bindless.glsl` when needed). Note that the sampled descriptor is written with
`SHADER_READ_ONLY_OPTIMAL`: sample a texture only in that layout (the render graph does this for you).

## Render graph

Passes declare how they use resources; the graph culls passes whose results are never consumed, orders them,
computes sync2 barriers + layout transitions (including queue-family release/acquire across queues), aliases
transient memory of resources with disjoint lifetimes, and adds per-pass debug labels, GPU timestamps and Tracy
zones. Compilation is pure CPU (`RenderGraph::compile()` → inspectable `RenderGraphPlan`) and cached by a hash of the
declared topology; physical transient resources are cached by the alias layout.

```cpp
using namespace ox::rhi;
RenderGraph graph;   // persistent across frames (owns cached plan + transient resources)

graph.reset();
RGTexture backbuffer = graph.importTexture(swapchain->currentTexture(), device.desc(swapchain->currentTexture()),
                                           {Access::Undefined, Access::Present});
RGTexture history = graph.importTexture(device, taaHistory, Access::SampledFragment); // persistent texture
RGTexture hdr   = graph.createTexture({.format = VK_FORMAT_R16G16B16A16_SFLOAT, .width = w, .height = h, .name = "HDR"});
RGTexture depth = graph.createTexture({.format = VK_FORMAT_D32_SFLOAT, .width = w, .height = h, .name = "Depth"});
RGTexture ao    = graph.createTexture({.format = VK_FORMAT_R8_UNORM, .width = w, .height = h, .name = "AO"});

graph.addPass("DepthPrepass").depth(depth)                       // CLEAR, reversed-Z clear value 0
    .execute([&](PassContext& ctx) { drawOpaqueDepth(ctx.cmd); });

graph.addPass("SSAO", PassType::Compute).queue(QueueType::Compute)   // async compute if available
    .read(depth, Access::SampledCompute)
    .overwrite(ao, Access::StorageWriteCompute)
    .execute([&](PassContext& ctx) {
        ctx.cmd.bindPipeline(ssao);
        ctx.cmd.pushConstants(SsaoPC{ctx.sampledIndex(depth), ctx.storageIndex(ao)});
        ctx.cmd.dispatchThreads(w, h);
    });

graph.addPass("Lighting")
    .read(ao, Access::SampledFragment)
    .read(history, Access::SampledFragment)
    .depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, {}, /*readOnly*/ true)
    .color(hdr)                                                    // dynamic rendering begins automatically
    .execute([&](PassContext& ctx) { drawLighting(ctx); });

graph.addPass("Tonemap").read(hdr, Access::SampledFragment).color(backbuffer)
    .execute([&](PassContext& ctx) { fullscreen(ctx.cmd, tonemap, ctx.sampledIndex(hdr)); });

graph.execute(device, {.swapchain = swapchain.get()});  // submits batches per queue with timeline waits
// or: graph.execute(cmd) to record everything into one graphics command list
std::ofstream("frame.dot") << graph.exportGraphviz();   // dot -Tsvg frame.dot > frame.svg
```

Declaration rules: one declaration per resource per pass. `read()` = depends on previous contents, `write()` =
read-modify-write, `overwrite()` = previous contents irrelevant; `color(LOAD)` vs `color(CLEAR)` likewise.
Imported resources and `markOutput()` resources are culling roots, as are `sideEffect()` passes. Transients used on
more than one queue are never aliased. Resource granularity is the whole resource (per-mip barriers: use
`CommandList::textureBarrier` inside a pass). Destroy graphs before the device (`releaseResources(device)`).

## Shaders and hot reload

* GLSL 4.60 → SPIR-V 1.5 (Vulkan 1.2 env) via shaderc. `#include "x"` searches the including file's directory, then
  the roots; `#include <x>` only the roots. Roots: `ShaderCompilerOptions::includeRoots` + `engine/shaders`
  (dev path baked in; packaged builds use `<exe>/shaders`; `OXWALD_SHADER_DIR` overrides).
* Optimisation in RelWithDebInfo/Release, debug info (`-g`, RenderDoc source debugging) in Debug/RelWithDebInfo.
* SPIR-V disk cache (`<temp>/oxwald/shader_cache` by default): key = path + stage + entry + defines + options + main
  source; each entry stores its include list with content hashes and is invalidated when any include changes.
* `ShaderStageDesc::file(path)` makes a pipeline hot-reloadable. `Device::beginFrame()` polls the main file and every
  transitive include (mtime/size, every `hotReloadPollMs`), recompiles affected pipelines and swaps the `VkPipeline`
  behind the unchanged `PipelineHandle` (old one goes to deferred destruction). Compile errors are logged and the
  previous pipeline keeps running; a pipeline whose first compile fails is a valid handle with a null `VkPipeline`
  (draws are skipped) so fixing the file brings it to life. `pipelineVersion(handle)` counts successful builds.
* Persistent `VkPipelineCache` (`DeviceDesc::pipelineCachePath`), validated against vendor/device/UUID, saved on
  device destruction or `savePipelineCache()`.

## Uploads & readback

* `createBuffer(desc, data)`, `writeBuffer`, `readBuffer`, `uploadTexture` (all mips/layers/cube faces, BC/ASTC/ETC2
  block sizes), `readTexture` — synchronous, for loading/tools/tests.
* `uploadBufferAsync` / `uploadTextureAsync`: data goes through a ring staging buffer (32 MiB default, larger uploads
  get a dedicated staging buffer) and is copied on the transfer queue. `flushUploads()` (also called by `endFrame()`
  and before any graphics submit) submits the batch; the next graphics submit waits on the transfer timeline and
  records the queue-family **acquire** barriers automatically.

## DeviceCaps on this machine (Apple M4 Pro, MoltenVK 1.4.2)

```
Vulkan: device 1.3.357, using 1.3; portability subset: yes; unified memory: yes
Queues: graphics family 0, async compute yes (family 1), dedicated transfer yes (family 2)
Ray tracing: AS no, ray query no, RT pipeline no -> unavailable (MoltenVK does not implement Vulkan ray tracing)
Geometry: mesh no, geometry shader no, tessellation yes, multiview yes, VS layer output yes, drawIndirectCount no
Textures: anisotropy 16, BC yes, ASTC yes, ETC2 yes; bindless: 1M sampled / 1M storage / 500K samplers
Subgroups: 32; timestamps yes (1 ns); memory budget yes
```

## macOS / MoltenVK notes

* The Khronos loader, MoltenVK and the validation layer come from vcpkg. `ox_deploy_vulkan_runtime(<target>)`
  (in `engine/rhi/cmake/OxwaldVulkanRuntime.cmake`) copies them into `<exe dir>/vulkan/` with relocatable manifests;
  `configureVulkanEnvironment()` sets `VK_DRIVER_FILES`/`VK_ADD_LAYER_PATH` to that folder (or to the dev copy in
  `<build>/vulkan-dev`) unless the user already set them. vcpkg rewrites MoltenVK's install name, which breaks its
  code signature — the deploy step re-signs ad hoc, otherwise macOS kills the process on load.
* No geometry shaders: use layered rendering via `gl_Layer` from the vertex shader (`DeviceCaps::shaderOutputLayer`)
  or multiview (`RenderingDesc::viewMask`, `GraphicsPipelineDesc::viewMask`) for cubemap/cascade rendering, or one
  pass per layer. No mesh shaders, no `drawIndirectCount` (compact on the GPU and use `drawIndirect` with a CPU max,
  or GPU-driven `dispatchIndirect`), no ray tracing (the RT checkbox is disabled with `whyRayTracingUnavailable()`).
* `MVK_CONFIG_LOG_LEVEL` defaults to warnings+errors; Tracy's GPU context is created only once a profiler connects
  because its 64K timestamp pool is emulated by MoltenVK.

## Debugging & profiling

* Validation: on in Debug/RelWithDebInfo, off in Release; `OX_VULKAN_VALIDATION=0/1` overrides. Messages are routed
  to `OX_LOG` (category `vulkan`); `Device::validationErrorCount()` lets tests fail on any error.
* Every object gets a `VK_EXT_debug_utils` name (from the desc `name`); render graph passes are wrapped in debug
  labels; `CommandList::ScopedLabel` for manual scopes.
* GPU timings: `CommandList::beginTimestamp/endTimestamp` and automatically per render graph pass;
  `Device::gpuTimings()` returns the last retired frame. `Device::memoryStats()` (VMA budgets) for the stats overlay.
* Tracy: with `OX_ENABLE_TRACY`, GPU zones per render graph pass on the graphics queue (`TracyVulkan.hpp` using the
  volk function pointers).
* RenderDoc: `device.renderDoc().triggerCapture()` / `startFrameCapture()` when the app was launched from RenderDoc
  (or `initialize(true)` to load `renderdoc.dll`/`librenderdoc.so`). On macOS it reports unavailable — use Xcode's
  Metal frame capture (`MTL_CAPTURE_ENABLED=1`, Debug → Capture GPU Workload) or MoltenVK's
  `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE`.

## Ray tracing (compiled always, used only with caps)

`createBlas` (build + compaction for static meshes, `allowUpdate` keeps scratch for `CommandList::refitBlas`),
`createTlas` + `CommandList::buildTlas(tlas, instances, update)` (per-frame instance ring), `accelStructAddress()`
(use `accelerationStructureEXT(addr)` in GLSL ray queries), `createRayTracingPipeline` (+ automatic SBT,
`CommandList::traceRays`). All return invalid handles and log `whyRayTracingUnavailable()` on unsupported devices.

## Tests

`ox_rhi_tests` (CPU): handle pools, formats/access mapping, render graph planner (culling, barriers, layout
transitions, WAR/WAW, aliasing incl. memory types, async-compute batches + ownership transfer, plan caching,
graphviz), shader compiler (include resolution, defines, reflection, errors, SPIR-V cache hit/invalidation, every
engine shader compiles). `ox_rhi_gpu_tests` (label `gpu`, skip without a device, fail on any validation error):
device/caps, buffer + texture upload/readback, async uploads with ownership transfer, mips/cube/depth/BC, 100-frame
stress with deferred destruction, compute via BDA, offscreen triangle, reversed-Z depth, MSAA resolve, bindless
sampling + storage images, hot reload, pipeline cache, render graph frames (graphics→compute, aliasing,
async compute), BLAS/TLAS (skipped on MoltenVK). `tools/rhi_window_smoke --frames 120 [--resize] [--present mailbox]`
checks the swapchain path manually.

## Known limits / TODO

* Render graph: whole-resource barriers (no per-mip/layer tracking), declaration order is the execution order
  (no reordering for overlap), aliasing places every resource at offset 0 of its slot (no sub-allocation packing),
  one cached physical layout per graph.
* No split barriers / events; no sparse resources; no secondary command buffers / multithreaded recording yet.
* `ox_add_module` passes `TEST_LABELS` unescaped to `gtest_discover_tests` (a list becomes extra properties) —
  rhi registers its GPU tests itself with `LABELS "rhi\;gpu"`.
* Modules configured before `engine/rhi` alphabetically (`render`) must `include()` `OxwaldVulkanRuntime.cmake`
  themselves to call `ox_deploy_vulkan_runtime`.
