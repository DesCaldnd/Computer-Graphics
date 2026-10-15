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
| `multi_device.hpp` | `enumerateAdapters`, `SecondaryDevice`, `CrossDeviceChannel`, `PortableDispatch` (split rendering, see "Multi-device"). |
| `split_scheduler.hpp` | `CrossDeviceTransfer`, `selectTransferPath`, `chooseSplit`, `SplitController`, `TimingFilter` (pure CPU). |
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
  On NVIDIA the async compute queue comes from a dedicated family (compute | transfer, no graphics). Barriers may
  only name pipeline stages the command buffer's queue family can execute: every physical queue stores the stage
  mask of its family (`queueFamilyStages` in `device.cpp`) and `DeviceState::sanitizeStages` filters with it.
* `Device::hasInstanceExtension(name)` / `hasDeviceExtension(name)` report what the device was really created with
  (required, optional and `DeviceDesc::optionalInstanceExtensions` / `optionalDeviceExtensions`, which are skipped
  silently when unsupported).
* `Device::addShutdownCallback(fn)`: runs once in `~Device` with the GPU idle and every rhi resource still alive,
  last registered first, before `vkDestroyDevice`. For libraries that keep their own Vulkan objects on the device
  (NVIDIA NGX, see `render_postprocess.md`).
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

Async batches (queue hints with `RGCompileOptions::asyncCompute`, families from `RGCompileOptions::queueFamilies`).
The rules below were fixed on the first device with a dedicated compute family (NVIDIA RTX 3080); before that the
measured overlap was zero:

* A batch is split before the first pass that touches a resource another queue's batch used — any access, even a
  read after a read, because the resource changes its owning queue family. Passes before the split do not wait, so
  they overlap with the async batch.
* The cross-frame ordering barrier (last frame's readers and writers of a reused transient) is skipped when the two
  passes run on different queue families: those stages would be invalid on the other family. `execute()` waits on
  the previous execution's timeline points of the other queues instead.
* `execute()` holds an async batch back until the following graphics batch is recorded and submits the two back to
  back. Submitted on its own, the async batch was already finished by the time the CPU had recorded the graphics
  passes meant to overlap with it.

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

`createBlas` (build + compaction for static meshes, `allowUpdate` keeps scratch for `CommandList::refitBlas`;
`refitBlas(blas, geometry)` refits from new buffers, e.g. ping-pong skinning output),
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
checks the swapchain path manually. Planner tests added with the NVIDIA bring-up:
`RenderGraphPlan.AsyncComputeBarriersNameNoGraphicsStages`,
`RenderGraphPlan.IndependentGraphicsPassesDoNotWaitForAsyncCompute` (run on Windows only so far).

## Windows / NVIDIA notes

Verified on Windows 11 with an NVIDIA RTX 3080 (driver 617.42, Vulkan 1.4), MSVC 14.44.

* The loader (`vulkan-1.dll`) and the validation layer come from vcpkg and are deployed by
  `ox_deploy_vulkan_runtime` like on macOS; the NVIDIA ICD comes from the driver. The Windows loader treats a
  manifest `library_path` as manifest-relative only when it contains a backslash, so the deployed layer manifest
  uses `.\\`.
* Do not run tests elevated: the loader ignores `VK_ADD_LAYER_PATH` in elevated processes.
* `DeviceLifecycle.SurfaceDeviceAfterHeadlessDevice` skips: the NVIDIA Windows driver has no
  `VK_EXT_headless_surface`.
* The SPIR-V disk cache writes through unique temporary names (several test processes share the cache under
  `ctest -j`; a fixed `<path>.tmp` raced).
* Mesh shaders and `drawIndirectCount` are available and used by the GPU-driven path (`render.md` §13).

## Polish additions (0.1.0)
* Manual Tracy GPU zones: `CommandList::beginGpuZone(name)` / `endGpuZone()`, `ScopedGpuZone`,
  `OX_RHI_GPU_ZONE(cmd, "name")` — graphics queue, only while a Tracy profiler is connected, otherwise no-ops.
* `ShaderCompileDesc::includeDirs`: per-compile include directories (searched after the including file's directory,
  before the compiler roots; part of the cache key) — for inline/generated sources whose `path` is only a name.
* `Device::savePipelineCache()` writes atomically (temp + rename); the runtime (`user://cache/pipeline_cache.bin`)
  and the editor (user data dir) set `DeviceDesc::pipelineCachePath`.
* Physical-device selection checks present support with volk instead of vk-bootstrap's surface path: vk-bootstrap
  caches instance functions from the first instance of the process, so a windowed device after a headless one used
  to crash in `get_present_queue_index` (regression test `DeviceLifecycle.SurfaceDeviceAfterHeadlessDevice`).
* `DeployVulkanRuntime.cmake` uses private temp names (parallel POST_BUILD deployments into one `bin/vulkan` raced).
  `ox_deploy_vulkan_runtime` also copies the NGX/DLSS runtime (`OX_DLSS_RUNTIME_FILES`, Windows/Linux) next to the
  executable.

## Multi-device (split rendering)

`multi_device.hpp` + `split_scheduler.hpp` (new, used by `render::addPortableSegment`, guide chapter 35). Explicit
multi-adapter without device groups (none exist across vendors): a compute-only **secondary `VkDevice`** next to the
primary `Device`, created on another `VkPhysicalDevice` of the primary's instance.

* `enumerateAdapters(primary)` → `AdapterInfo` (type, UUIDs, LUID, local memory, `suitable` + reason, external memory
  extensions); `describeAdapters()` for logs. Requirements of a secondary: Vulkan 1.2, timeline semaphores,
  descriptor indexing with update-after-bind + partially bound, non-uniform sampled indexing, BDA, scalar layout,
  synchronization2, a compute queue.
* `SecondaryDevice::create(primary, {adapterIndex, allowSameAdapter, transfer, calibrate})`: picks the best other
  adapter (integrated first, never the CPU type); `allowSameAdapter` creates it on the primary's physical device
  (single-GPU testing). It never presents: no surface, no swapchain, no graphics queue needed.
  * Calls Vulkan through its own `VolkDeviceTable` (volk's global device functions belong to the primary, so the
    "one `Device` per process" rule still holds; secondaries are not `Device`s).
  * A bindless set 0 with the same bindings as the primary (sampled / storage / samplers, default samplers at
    indices 0–5 mirrored from the primary's descs) + the same 128-byte push-constant layout, so the **same SPIR-V**
    runs on both. `preparePipeline(handle)` recompiles the primary pipeline's `ComputePipelineDesc` (shader
    compiler + SPIR-V cache of the primary) per `pipelineVersion` (hot reload works).
  * Calibration: a 1080p post-process-like kernel (9 bilinear taps + ALU, RGBA8) on both devices, best of 3 →
    `calibratedSpeedRatio()` (secondary / primary GPU time). Same RTX 3080 for both: 0.123 / 0.121 ms → 0.99.
* `CrossDeviceCaps` + `selectTransferPath(caps, preference)`:
  | Path | When | How |
  | --- | --- | --- |
  | `ExternalMemory` | `deviceUUID` **and** `driverUUID` equal (same physical GPU) + primary exports opaque Win32/fd memory | one dedicated device-local allocation exported by the primary, imported by the secondary; `VK_QUEUE_FAMILY_EXTERNAL` release/acquire on both sides |
  | `HostPointer` | `VK_EXT_external_memory_host` on both | aligned host allocation imported into both devices as buffers; GPU copies only (the dGPU reads/writes it over PCIe, an iGPU natively). The real path for dGPU + iGPU |
  | `Staging` | always | host-visible buffers per device (cached for CPU reads), the worker `memcpy`s between them |
  The primary needs `appendMultiDeviceExtensions(desc)` (optional external memory win32/fd + host extensions) for
  the first two; without them Staging is used.
* `CrossDeviceChannel` (`SecondaryDevice::createChannel({textures, inputs, outputs, slots})`): secondary textures
  (2D, 1 mip, colour; usage by role, format features checked) + per-direction transfer buffers with one region per
  texture per slot. Primary side, inside a graph pass on the graphics queue: `recordExport(cmd, slot, inputs)`
  (images in `TransferRead`), `recordImport(cmd, slot, outputs)` (`TransferWrite`). `submit(slot, ticket,
  exportDone, dispatches)` queues a job; `wait`/`ready`/`timing(slot)`.
* **Synchronisation is on the CPU** (opaque semaphores need equal UUIDs; D3D12-fence interop would need D3D12): one
  worker thread per secondary waits `vkWaitSemaphores` on the primary's graphics timeline for the export's
  submission, does the staging `memcpy`, records upload copy → `PortableDispatch`es (push constants patched with the
  secondary's bindless indices) → download copy with 4 timestamps, submits with its own timeline and waits.
  Textures are transitioned `UNDEFINED → GENERAL` per job (results are fully rewritten). The owner pipelines by
  `latency` frames so the render thread normally never waits.
* Failure: `VK_ERROR_DEVICE_LOST` / timeouts mark the device `lost()` (`lostReason()`); queued and later jobs fail,
  `createChannel` refuses. `simulateDeviceLost()` makes the next job fail (tests). `liveCount()` for leak tests.
  Destroying a channel waits for its jobs, frees the secondary side at once and retires the primary side through
  the primary's deferred deletion (the GPU may still copy from it).
* `split_scheduler.hpp` (pure CPU, `ox_rhi_tests`): `SplitSegmentCost`, `SplitCostModel` (`TransferBandwidth`
  defaults per path, `primaryFrameMs`, `minGainMs`, `secondaryBudgetFraction` 0.9), `estimateSegment`,
  `chooseSplit` (max primary saving s.t. secondary + CPU copies ≤ 90 % of the new frame; exact subset search ≤ 16
  segments, greedy beyond; `force` = offload all portable), `SplitController` (hysteresis: N consecutive equal
  decisions), `TimingFilter` (EMA).
* Tests: `ox_rhi_tests` `SplitTransferPath.*`, `SplitScheduler.*`, `SplitController.*`, `TimingFilter.*`;
  `ox_rhi_gpu_tests` `MultiDeviceTest.*` (adapters, refusal without a second GPU, calibration, round trips through
  every path with odd sizes and slot reuse, chain with an intermediate texture, device loss, unsupported formats).
  All run with the secondary on the RTX 3080 itself; the Intel UHD 630 of this machine is disabled in firmware.
* `RenderGraph::textureDesc(RGTexture)` (additive): declared description of a graph texture (channel signatures).

## Low latency (VK_NV_low_latency2) and swapchain proxies

* `DeviceDesc::enableLowLatency` (default on, windowed devices): `VK_NV_low_latency2` + `VK_KHR_present_id` (+ the
  `presentId` feature) when listed; on Windows both come from NVIDIA's implicit present layer (`VK_LAYER_NV_present`,
  driver 617.42: revision 3). `DeviceCaps::lowLatency2 / presentId`.
* `SwapchainDesc::lowLatency` (default on): swapchains are created with `VkSwapchainLatencyCreateInfoNV`.
  `Swapchain::setLowLatencyMode({enabled, boost, minimumIntervalUs})` (any thread, re-applied to every recreated
  swapchain), `latencySleep(timeoutMs)` (`vkLatencySleepNV` + host wait on a private timeline semaphore; returns ms
  slept or < 0), `setLatencyMarker(presentId, LatencyMarker)`, `latencyTimings()` (`vkGetLatencyTimingsNV`, ≤ 64
  `LatencyFrameTimings` sorted by present id; `pcLatencyMs()` = simulation start → GPU render end).
  `setPresentId(id)` (render thread, before the frame's submit): `Device::submit` chains
  `VkLatencySubmissionPresentIdNV` into the frame's `VkSubmitInfo2` (submits with `SubmitInfo::swapchain`), `present()`
  chains `VkPresentIdKHR` and emits `PresentStart/End`. A mutex guards the `VkSwapchainKHR` against recreation while
  the game thread is inside a latency call (`ConcurrentGameThreadSleepWhileTheRenderThreadRecreates`).
* `ISwapchainProxy` + `Swapchain::setProxy(proxy)`: the swapchain is created / acquired / presented through the
  proxy's entry points from the next acquire on (AMD FSR 3 frame generation's swapchain, render `features/framegen`).
  Switching in or out rebuilds the swapchain; Reflex and present ids are off while a proxy owns it
  (`lowLatencySupported()` false). `DeviceDesc::frameGenerationQueues` reserves the proxy's private queues
  (`Device::frameGenerationQueues()`: present + image acquire in the graphics family, one compute queue).
  `Device::graphicsQueueMutex()` exposes the graphics queue's submission lock.
* Tests (`ox_rhi_gpu_tests`, small GLFW window, skipped without display / extension): `LowLatencyTest.*` — markers in
  frame order as timestamped by the driver, `VkLatencySubmissionPresentIdNV` ties GPU times to the frame, exactly one
  sleep per frame, mode toggles + recreation without validation errors, concurrent sleep during recreation.

## Variable rate shading (VK_KHR_fragment_shading_rate)

* `DeviceDesc::enableFragmentShadingRate` (default on); `DeviceCaps::fragmentShadingRate`, `pipelineShadingRate`,
  `attachmentShadingRate`, `primitiveShadingRate`, attachment texel size range / aspect, `shadingRateMask` (bit
  `(log2 w << 2) | log2 h` per supported fragment size), combiner and depth/stencil-write properties,
  `shadingRateSupported(w, h)`.
* `TextureUsage::ShadingRateAttachment`, `Access::ShadingRateRead` (FRAGMENT_SHADING_RATE_ATTACHMENT stage / layout).
* Render graph: `PassBuilder::shadingRate(rate, texelSize)` attaches an R8_UINT rate image to a graphics pass
  (`VkRenderingFragmentShadingRateAttachmentInfoKHR` in `RenderingDesc::shadingRate`); `RenderGraph::pass(i)` returns
  the builder of an already declared pass, `passType`, `passRenderExtent`, `passShadingRate` let a feature attach it
  after the passes were declared.
* Graphics pipelines (`GraphicsPipelineDesc::variableRateShading`, default on) get
  `VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR` with combiners (KEEP, REPLACE) when the
  device has attachment rates — required for draws in a scope with a rate image; without one the rate is 1×1 and the
  image is bit-identical to a device without the extension (`RenderTest.VrsOffMatchesDeviceWithoutTheExtension`).
* The only user is render's `VariableRateShading` feature: [render_vrs.md](render_vrs.md).

## Known limits / TODO

* Render graph: whole-resource barriers (no per-mip/layer tracking), declaration order is the execution order
  (no reordering for overlap), aliasing places every resource at offset 0 of its slot (no sub-allocation packing),
  one cached physical layout per graph.
* No split barriers / events; no sparse resources.
* Multithreaded recording: `Device::secondaryCommandList(threadSlot, SecondaryRenderingInfo)` (per-thread pools per
  frame slot, `kMaxRecordingThreads` = 64), `CommandList::end()` / `executeSecondary()`, `RenderingDesc::secondaryContents`,
  render graph `PassBuilder::secondaryCommandLists()` + `PassContext::secondaryRendering`. MoltenVK replays secondaries
  serially at submit, so it is slower there (docs/dev/perf.md).
* Async pipelines: `Device::createGraphicsPipelineAsync` / `createComputePipelineAsync(desc, jobs)`, `isPipelineReady`,
  `pendingPipelineCompiles`; results are published in `beginFrame` (never during recording).
* `CommandList::drawMeshTasksIndirect`. `GpuTiming::startMs` / `queue` (overlap analysis). The graph splits a batch
  before the first pass that waits on another queue (independent passes keep overlapping with async compute); see
  "Render graph" for the exact rules.
* `ox_add_module` passes `TEST_LABELS` unescaped to `gtest_discover_tests` (a list becomes extra properties) —
  rhi registers its GPU tests itself with `LABELS "rhi\;gpu"`.
* Modules configured before `engine/rhi` alphabetically (`render`) must `include()` `OxwaldVulkanRuntime.cmake`
  themselves to call `ox_deploy_vulkan_runtime`.
