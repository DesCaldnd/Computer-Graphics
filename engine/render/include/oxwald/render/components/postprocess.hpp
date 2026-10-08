#pragma once

// ECS components of the postprocess-upscalers area: post-process volumes (UE style).
// Reflected and registered by registerPostProcessTypes() (called from render::registerRenderTypes() and by the
// feature registration); extracted into the RenderSnapshot by the area's extract hook and blended per view by the
// post-process features (see features/postprocess/postprocess.hpp).
//
// A volume overrides whole categories (exposure, bloom, depth of field, ...) when the matching `override*` flag is
// set. Volumes are blended in ascending priority over the cvar defaults: unbound volumes always apply with
// `blendWeight`; bound volumes are boxes (entity transform, half extents `extents`) whose weight fades to zero over
// `blendRadius` metres outside the box.

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>

namespace ox::render {

struct PostProcessSettings {
    // --- exposure (eye adaptation) ---
    bool overrideExposure = false;
    bool autoExposure = false;            // histogram auto exposure (else camera / manual EV100)
    f32 exposureCompensation = 0.0f;      // EV added on top of r.Exposure.Compensation
    f32 minEV100 = -4.0f;                 // darkest scene the adaptation follows
    f32 maxEV100 = 20.0f;                 // brightest
    f32 adaptationSpeedUp = 3.0f;         // EV per second when the scene gets brighter
    f32 adaptationSpeedDown = 1.0f;       // EV per second when it gets darker
    // Average the luminance histogram between these percentiles (the dark half — shadows, sky-less corners — and the
    // brightest 10 % — sky, highlights — are ignored); the average is exposed as middle grey.
    f32 histogramLowPercent = 50.0f;
    f32 histogramHighPercent = 90.0f;

    // --- bloom (threshold-less, energy conserving) ---
    bool overrideBloom = false;
    f32 bloomIntensity = 0.04f; // fraction of the image replaced by its blurred version
    Uuid bloomDirtTexture;      // lens dirt mask (Texture asset), multiplied with the wide bloom
    f32 bloomDirtIntensity = 0.0f;

    // --- depth of field (physical camera) ---
    bool overrideDepthOfField = false;
    f32 focusDistance = 0.0f;  // metres; 0 = depth of field off
    f32 aperture = 0.0f;       // f-number; 0 = the camera's CameraComponent::aperture
    f32 focalLength = 0.0f;    // millimetres; 0 = derived from the vertical FOV on a 24 mm (full frame) sensor
    f32 maxBokehSize = 1.5f;   // largest circle of confusion radius in percent of the view width

    // --- motion blur ---
    bool overrideMotionBlur = false;
    f32 motionBlurAmount = 0.5f; // shutter fraction of the frame time (0.5 = 180° shutter)
    f32 motionBlurMax = 5.0f;    // largest blur length in percent of the view width

    // --- white balance & grading (applied in HDR before the tonemapper through a 32³ log-space LUT) ---
    bool overrideWhiteBalance = false;
    f32 temperature = 6500.0f; // Kelvin of the illuminant made neutral (UE style: lower = cooler image)
    f32 tint = 0.0f;           // green (-) / magenta (+), -1..1
    bool overrideGrading = false;
    f32 saturation = 1.0f;
    f32 contrast = 1.0f;          // around 18 % grey in log space
    glm::vec3 lift{0.0f};         // ASC-CDL style offset of the shadows
    glm::vec3 gamma{1.0f};        // power applied to the mid-tones
    glm::vec3 gain{1.0f};         // slope (multiplies the highlights)
    // User LUT (display referred, applied after tonemapping): strip texture N²×N (e.g. 1024×32 or 256×16), red along
    // x inside a slice, green along y, blue across the slices.
    bool overrideLut = false;
    Uuid lutTexture;
    f32 lutIntensity = 1.0f;

    // --- lens & film ---
    bool overrideLens = false;
    f32 vignetteIntensity = 0.0f;   // 0..1
    f32 chromaticAberration = 0.0f; // 0..1 (fringe width up to ~0.5 % of the view at the corners)
    f32 filmGrainIntensity = 0.0f;  // 0..1
    f32 sharpen = 0.0f;             // contrast adaptive sharpening 0..1 (r.Sharpen when not overridden)
};

struct PostProcessVolumeComponent {
    bool enabled = true;
    bool unbound = true;          // infinite extent (global settings), else a box
    glm::vec3 extents{5.0f};      // box half extents in the entity's local space (scaled by the transform)
    f32 blendRadius = 1.0f;       // metres outside the box over which the weight fades to 0
    f32 blendWeight = 1.0f;       // 0..1
    i32 priority = 0;             // higher priorities are applied last (win)
    PostProcessSettings settings;
};

// Reflection + ComponentRegistry registration (idempotent) and the extract hook that copies volumes into the
// snapshot (PostProcessVolumesSnapshot extension).
void registerPostProcessTypes();

} // namespace ox::render
