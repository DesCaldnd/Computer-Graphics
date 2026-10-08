# Integration backlog

Open items after the 0.1.0 polish pass, each with the reason it is still open. Fixed items were removed (see
CHANGELOG.md, "Полировка перед выпуском").

- ai: `BTContext::user` is a `void*` (owner set through `BehaviorTree::setUserData`). Kept on purpose: `ai` does not
  depend on `scene`, and gameplay wraps the owning entity. A typed handle would need an `ai` → `scene` dependency.
- animation: clips/skeletons use their own binary format instead of `ox::serial`. Migrating means a new on-disk
  format plus a cache-invalidation step for every imported asset; scheduled together with the fastgltf importer.
- rhi: no programmatic Xcode GPU capture trigger. Captures work through Xcode / `MTL_CAPTURE_ENABLED=1` and
  MoltenVK's `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE` (documented in `RenderDocCapture::unavailableReason`); an
  `MTLCaptureManager` wrapper needs an Objective-C++ path in rhi.
- editor/vcpkg: vcpkg's app-local step rewrites install names of the dylibs it copies into `OxwaldEditor.app`, which
  invalidates their ad-hoc signatures; the editor's POST_BUILD `codesign --force --deep` stays. The installed
  MoltenVK itself is validly signed now (overlay port), so the re-sign steps in `DeployVulkanRuntime.cmake` and
  the editor tests are safety nets only. Static Qt still links its own MoltenVK reference (ICD manifest points to
  Qt's copy).
- net: the replicated field set is fixed when an object spawns (by design: both sides build the same property
  list from the spawn data). Containers inside reflected fields replicate as whole blobs; entity references inside
  containers keep their UUIDs (only level entities resolve on clients).
- gameplay: one `ScriptComponent` per entity; child colliders are not merged into the parent's rigid body
  (compound shapes must be authored on the body entity); physics raycasts return nothing in edit mode (no physics
  world exists until play). All three are design changes in gameplay, not defects.
- render/postprocess: the DLSS jitter sign follows UE's convention and must be checked once with the NGX debug
  overlay on an RTX machine (no NVIDIA hardware here). The NGX runtime deployment (`ox_deploy_dlss_runtime`) and the
  executable-directory feature path are untested on Windows/Linux for the same reason.
- render: material height maps (`heightTexture`, `heightScale`) are not used yet — parallax needs a larger
  `GpuMaterial` (it is exactly 128 bytes today) and tangent-space ray marching in every material shader.
- render/editor: viewport asset loads run synchronously on the UI thread (the engine's job system dies with the
  engine on project switches while the viewport renderer lives on); needs an editor-owned job system.
- MoltenVK: copying a GLSL struct that contains a 64-bit `buffer_reference` by value reads zeros (driver bug);
  workaround in the shaders: access such members through the push constant block directly.
- render/volumetrics: sky light scattered by the froxel fog is not occluded (indoors the fog glows with the full sky
  ambient). Workaround: `VolumetricFogComponent::ambientIntensity` scales only the fog's sky light (keep the
  surfaces' `EnvironmentComponent::ambientIntensity`). A real fix needs sky visibility per froxel (irradiance-volume
  visibility or a sky-occlusion map), a new feature rather than a fix.
- runtime/net: while the engine loads a level asynchronously no systems run, so gameplay networking is not polled
  during the load (handshakes resume afterwards; long loads can hit the 5 s transport timeout). Polling without an
  attached world is fixed; polling during loads needs an engine-level network service.
- MoltenVK 1.4.2 logs "Blending is enabled for attachment with format VK_FORMAT_R32_UINT" for every non-blendable color attachment even when blendEnable is false (MVKPipeline.mm:1994, driver bug). Harmless; the rhi already disables blending for integer formats.
