# Module `animation` (`Oxwald::animation`, namespace `ox::anim`)

Skeletal animation runtime written in-house (ozz-animation isn't in vcpkg). It depends only on `core`
(types/log/assert) plus glm, with assimp as a private dependency for importing. There are no ECS or
renderer dependencies: the integration layer (`gameplay`/`render`) wraps it in components and systems.

Umbrella header: `#include <oxwald/animation/animation.hpp>`.

| Header | Contents |
| --- | --- |
| `transform.hpp` | `Transform` (T/R/S), `nlerpShortest`, `slerpShortest`, `rotationBetween` |
| `skeleton.hpp` | `Skeleton`: joint names, parents (topologically sorted), bind pose, inverse bind matrices |
| `pose.hpp` | `Pose`, `JointMask`, `localToModel`/`modelToLocal`, `blendPoses`, `blendPosesWeighted`, `PoseAccumulator`, additive helpers |
| `clip.hpp` | `AnimationClip` (SoA tracks, Step/Linear/CubicSpline, slerp/nlerp), `SamplingCursor`, events, `RootMotionTrack`, `extractRootMotion`, `makeAdditiveClip` |
| `compact_clip.hpp` | `CompactClip`: flat SoA runtime format with optional 48-bit "smallest three" quaternions (`PackedQuat`) |
| `blend_space.hpp` | `BlendSpace1D`, `BlendSpace2D` (Delaunay / FreeformDirectional / FreeformCartesian), `delaunayTriangulate` |
| `animator.hpp` | `AnimatorController` (asset: parameters, layers, states, transitions), `Animator` (runtime), `applyRootMotion` |
| `ik.hpp` | Two-bone IK, aim IK, look-at chain, FABRIK, foot IK (ground raycast callback), `applyModelRotation` |
| `skinning.hpp` | `SkinnedMeshData` (4/8 influences), palette computation, `DualQuat`, CPU skinning (LBS/DQS) |
| `importer.hpp` | `importAnimationAsset` (assimp: glTF/GLB/FBX/...) with unit and axis conversion to Y-up meters |
| `serialization.hpp` | `ByteWriter`/`ByteReader` and `serialize`/`deserialize` for Skeleton, AnimationClip, CompactClip, SkinnedMeshData |
| `debug_draw.hpp` | `debugDrawSkeleton(skeleton, model, ownerWorld, lineFn)` — line callback `void(vec3, vec3, vec4)` |

GPU skinning shader: `engine/shaders/animation/skinning.comp` (see below).

## Frame pipeline

```cpp
using namespace ox::anim;

// once
auto imported = importAnimationAsset("hero.glb");            // std::optional<AnimationImport>
Skeleton& skel = imported->skeleton;
auto walk = std::make_shared<AnimationClip>(imported->clips[0]);
extractRootMotion(*walk, skel, {.rootJoint = skel.findJoint("Hips")});

auto ctrl = std::make_shared<AnimatorController>();
u32 speed = ctrl->addParameter("speed", ParamType::Float);
u32 jump  = ctrl->addParameter("jump", ParamType::Trigger);
u32 base  = ctrl->addLayer("Base");

auto loco = std::make_shared<BlendSpace1D>();
loco->addSample(0.0f, idleClip);
loco->addSample(1.5f, walk);
loco->addSample(4.0f, runClip);
i32 locoState = ctrl->addState(base, {"Locomotion", Motion::fromBlendSpace(loco, speed)});
StateDesc jumpDesc{"Jump", Motion::fromClip(jumpClip)};
jumpDesc.loop = false;
i32 jumpState = ctrl->addState(base, jumpDesc);
ctrl->addTransition(base, kAnyState, jumpState, 0.1f).when(jump, ConditionOp::Triggered);
auto& back = ctrl->addTransition(base, jumpState, locoState, 0.2f);
back.hasExitTime = true;
back.exitTime = 0.9f;

// Upper-body layer masked to the spine branch, plus an additive "breathing" layer.
u32 upper = ctrl->addLayer("Upper", LayerBlend::Override, 1.0f);
ctrl->layer(upper).mask = JointMask::fromBranch(skel, skel.findJoint("Spine1"));
...

Animator animator(skel, ctrl);

// per frame
animator.setFloat(speed, velocity);
animator.update(dt);
for (const FiredEvent& e : animator.events()) { /* footsteps, ... */ }
applyRootMotion(ownerTransform, animator.rootMotionDelta());

Pose pose = animator.pose();
std::vector<Transform> model;
localToModel(skel, pose, model);
solveFootIK(skel, pose, model, legs, footSettings, raycastFn);   // optional IK passes
solveLookAtChain(skel, pose, model, lookAt);

std::vector<glm::mat4> palette;
std::vector<glm::mat4> modelMats;
localToModel(skel, pose, modelMats);
computeSkinningMatrices(skel, modelMats, palette);                 // → GPU (or skinMesh on CPU)
```

## Details

- **Skeleton**: `addJoint(name, parent, bindLocal)` asserts that `parent < index`, so every hierarchy pass
  is a single forward loop. Inverse binds come from the importer (`aiBone::mOffsetMatrix` / glTF
  `inverseBindMatrices`) or from `finalize()`.
- **Clips**: each track stores `times` and `values` separately (SoA). Rotations use the shortest path for
  both slerp and nlerp, and `fixQuaternionHemispheres()` makes consecutive keys continuous. Cubic
  interpolation uses glTF semantics (tangents scaled by the key interval). Pass a `SamplingCursor` for
  sequential playback: it caches the last key index per track, so lookups are O(1). Seeks and backward
  playback fall back to binary search.
- **Root motion**: `extractRootMotion` moves the root's XZ translation and yaw (each channel can be turned
  on or off) into `clip.rootMotion` and makes the root track in-place. `Animator` accumulates per-frame
  deltas on layer 0 with blend weights, handling loop wraps. Apply them with
  `applyRootMotion(owner, delta)`; the delta is in the owner's local space.
- **Additive**: `makeAdditiveClip(clip, skeleton, referencePose)` produces a delta clip. Joints without
  tracks default to identity. Blend it with `applyAdditive(base, delta, weight, mask)` or with an
  `LayerBlend::Additive` animator layer.
- **Blend spaces**: weights always sum to 1. In 1D, the space interpolates between the two nearest
  samples and clamps outside the range. In 2D Delaunay mode, a point inside a triangle gets
  barycentric weights; a point outside the hull is projected onto the nearest edge. The Freeform modes
  use gradient-band interpolation (Johansen 2009); the directional variant works in polar space, which
  suits locomotion. A blend-space state keeps all its clips phase-synchronised: normalised time advances
  by `dt / Σ wᵢ·durationᵢ`.
- **State machine**: parameters are Float/Int/Bool/Trigger.
  - Transition conditions: `Greater`, `Less`, `Equal`, `NotEqual`, `IsTrue`, `IsFalse`, `Triggered`.
  - Exit time: on a looping state, an exit time below 1 is checked on every cycle.
  - Transitions have a crossfade duration and a destination offset.
  - Any-state transitions are checked first and can interrupt a running crossfade. On interruption, the
    last output pose is frozen and used as the blend source, so there is no visible pop.
  - Triggers are consumed when they fire a transition.
  - At most one transition fires per layer per update.
- **Layers**: layer 0 is the base. Further layers apply in order, either as `Override` (lerp by layer
  weight × mask) or `Additive`.
- **Events**: `FiredEvent` entries for the window (prev, cur], including loop wraps. An event at t = 0
  fires when a state starts. Events from blended clips carry the clip's blend weight.
- **IK** (model space; every solver updates both the local pose and the model transforms, so solvers can
  be chained):
  - `solveTwoBoneIK`: analytic, bends toward a pole point, optional stretch (`maxStretch`), weight, and
    optional end-effector rotation. Without one, the end joint keeps its model-space orientation.
  - `solveAimIK`: aim axis plus optional up axis (twist control), max angle.
  - `solveLookAtChain`: distributes the correction over spine → neck → head using per-joint weights.
  - `solveFABRIK`: arbitrary chain, tolerance and iteration cap; `fabrikPositions` is the position-only
    core.
  - `solveFootIK`: casts world-space rays through a user callback (`GroundRaycast`, e.g. physics), drops
    the pelvis for the lowest foot, plants each ankle with two-bone IK, and tilts the feet to the ground
    normal (clamped).
- **Skinning**: `palette[j] = model[j] · inverseBind[j]`, which is identity in the bind pose.
  `computeDualQuatPalette` and `SkinningMethod::DualQuaternion` give volume-preserving skinning (no
  "candy wrapper" collapse; scale is ignored). `skinMesh` is the CPU reference and fallback.
- **Compact clips**: `CompactClip::build(clip, {quantizeRotations, resampleRate})` resamples cubic tracks
  to linear and stores every key in a few flat arrays. Quantised rotations take 6 bytes instead of 16, with
  ≤ 2e-4 rad error.
- **Serialization**: a little-endian byte stream. Each blob has a 4-byte magic and a u32 version.
  Loading is bounds-checked: truncated or corrupt data returns `false` and never reads out of range.
  This will be replaced by `ox::serial` archives once core provides them.

## Importer

`importAnimationAsset(path, ImportSettings)` / `importAnimationAssetFromMemory(data, size, "glb")`.

- Joints are every node referenced by a bone, plus intermediate nodes up to the lowest common ancestor.
  With no bones (animation-only files), the animated nodes become joints. The transform of the
  ancestor above the skeleton is baked into the root joints (both bind pose and keys).
- Influences are limited to 4 or 8 (`maxInfluences`), duplicates are merged, weights are sorted by size
  and normalised. Vertices without weights go to joint 0.
- Axis and units:
  - `sourceUpAxis = Auto` reads the FBX `UpAxis`/`FrontAxis`/`CoordAxis` metadata; glTF is already
    Y-up. You can also force `Y`/`Z`/`X`.
  - `unitToMeters` (auto: FBX `UnitScaleFactor` × 0.01) and `scale` set the length conversion.
  - Conversion `C = R·s` is applied to mesh positions (`C`), normals (`R`), root joint translation and
    rotation, all other joints' translations (`s`), and inverse binds (`S·IB·C⁻¹`). The bind pose palette
    stays identity after conversion.
- Assimp exceptions are caught at the module boundary. Errors are reported as `std::nullopt` + `error` +
  `OX_LOG_ERROR`.

## Compute skinning shader — `engine/shaders/animation/skinning.comp`

The shader uses vertex pulling with one invocation per vertex (`local_size_x = 64`). All buffers are
passed as buffer device addresses in push constants (`GL_EXT_buffer_reference`, scalar layout). The shader
is self-contained because `common/bindless.glsl` doesn't exist yet.

| Push constant | Type | Contents |
| --- | --- | --- |
| `inVertices` | `SkinVertex[]` | `{vec3 position; vec3 normal; vec4 tangent;}`, 40 bytes, bind pose |
| `joints` | `uint[]` | two u16 joint indices per uint (low half first) — `SkinnedMeshData::joints` uploaded as-is |
| `weights` | `float[]` | `influences` floats per vertex |
| `palette` | `uint64_t` | mode 0: `mat4[]` (`computeSkinningMatrices`); mode 1: `vec4[2·J]` real/dual xyzw (`DualQuat` memory layout) |
| `outVertices` | `SkinVertex[]` | skinned output (same layout) |
| `vertexCount`, `influences` (4/8), `mode` | `uint` | |

Dispatch `ceil(vertexCount / 64)` workgroups. Before the vertex/raster pass reads `outVertices`, insert a
compute → vertex-attribute-read barrier. The test `AnimShaders.SkinningComputeCompilesToSpirv` compiles
the shader with shaderc, or with a `glslc`/`glslangValidator` found at configure time. If neither is
available, the test skips.

## Known limits / TODO

- Assimp does not expose glTF cubic-spline tangents, so imported cubic tracks arrive as assimp's resampled
  keys. The clip format supports cubic data for other sources, such as a future fastgltf importer.
- The importer handles one skeleton per file. Morph targets and non-skinned rigid attachments are not
  imported. Materials are referenced only by index.
- When a crossfade is interrupted, the frozen source contributes no root motion.
- Transitions are evaluated after time advances. Only any-state transitions can interrupt a running
  crossfade; ordered interruption sources (Unity-style) are not implemented.
- Two-bone IK assumes `mid` and `end` are direct children in the chain (twist bones in between are not
  compensated). IK assumes uniform scale.
- No reflection/ECS registration yet. The integration agent adds components on top (see the proposal in
  the report).
- The animation and skinning compute path is not yet wired to the RHI.
