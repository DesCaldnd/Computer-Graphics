#pragma once

// ECS components of the reflections-ao area: reflection probes, planar reflectors and irradiance volumes.
// Reflected and registered by registerReflectionTypes() (called from render::registerRenderTypes() and by
// registerReflectionFeatures()); extracted into the RenderSnapshot (ReflectionSnapshot extension) by the area's
// extract hook. The entity's world transform places them (scale is ignored, rotation is honoured).

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>

namespace ox::render {

enum class ReflectionProbeUpdate : u8 {
    Baked,    // captured once (or loaded from baked .oxcube data) and re-captured only by a bake request
    OnEnable, // captured whenever the probe becomes active or one of its parameters changes
    Realtime, // re-captured continuously, r.ReflectionProbes.RealtimeFacesPerFrame cube faces per frame
};

// Box-shaped local reflection probe (cubemap captured at the entity position).
struct ReflectionProbeComponent {
    bool enabled = true;
    glm::vec3 extents{5.0f, 3.0f, 5.0f}; // half size of the influence / projection box (local space, metres)
    f32 blendDistance = 1.0f;            // influence fades to 0 this far outside the box (metres)
    bool boxProjection = true;           // parallax-corrected box projection
    glm::vec3 captureOffset{0.0f};       // capture point relative to the entity (local space)
    u32 resolution = 128;                // cube face size (clamped to r.ReflectionProbes.Resolution)
    ReflectionProbeUpdate update = ReflectionProbeUpdate::Baked;
    i32 priority = 0;                    // higher wins where probes overlap (then smaller volume)
    f32 intensity = 1.0f;
    f32 nearPlane = 0.05f;
    f32 farPlane = 200.0f;
};

// Mirror plane: the scene is rendered from the reflected camera (oblique near plane on the reflector plane) and
// composited onto opaque surfaces lying on the plane; the water shader samples the same texture (PlanarReflection).
// Plane = the entity's local XZ plane through its origin, normal = local +Y.
struct PlanarReflectorComponent {
    bool enabled = true;
    glm::vec2 size{0.0f};       // half extents in local XZ for culling / compositing (0 = infinite plane)
    f32 resolutionScale = 1.0f; // × r.PlanarReflections.ResolutionScale × render resolution
    f32 clipOffset = 0.02f;     // clip plane offset along the normal (hides seams / z-fighting at the surface)
    f32 maxDistance = 500.0f;   // far plane of the reflected view (metres)
    f32 maxRoughness = 0.3f;    // surfaces rougher than this fall back to probes / SSR
    f32 distortion = 0.02f;     // screen-uv offset per unit of normal deviation (bumpy mirrors, water)
    f32 intensity = 1.0f;
    i32 priority = 0;
};

// Grid of baked irradiance probes (SH L1 + octahedral depth moments, DDGI layout) feeding IndirectDiffuse.
struct IrradianceVolumeComponent {
    bool enabled = true;
    glm::vec3 extents{10.0f, 4.0f, 10.0f}; // half size (local space, metres); probes span the whole box
    glm::ivec3 probeCount{8, 4, 8};        // probes per axis (≥ 2), at most 64 per axis and 16384 total
    f32 blendDistance = 1.0f;              // fade to the sky fallback outside the box (metres)
    f32 intensity = 1.0f;
    f32 normalBias = 0.25f;                // sample offset along the surface normal (× probe spacing)
    f32 viewBias = 0.15f;                  // sample offset towards the camera (× probe spacing)
    u32 captureResolution = 32;            // cube face size of the bake captures
    i32 priority = 0;
};

// Reflection + ComponentRegistry registration and the extract hook (idempotent).
void registerReflectionTypes();

} // namespace ox::render
