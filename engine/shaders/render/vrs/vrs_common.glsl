// Variable rate shading: shared helpers of the rate image passes. The per-tile decision mirrors
// ox::render::vrs::decideRate (engine/render/src/features/vrs/vrs_decision.cpp); keep both in sync.
#ifndef OX_RENDER_VRS_COMMON_GLSL
#define OX_RENDER_VRS_COMMON_GLSL

#include <render/common/view.glsl>

// Storage alias for the R8_UINT rate image (same binding as bindless.glsl).
layout(set = 0, binding = 1, r8ui) uniform uimage2D oxImages2D_r8ui[];

const uint OX_VRS_MODE_ADAPTIVE = 0u;
const uint OX_VRS_MODE_MOTION = 1u;
const uint OX_VRS_MODE_FOVEATED = 2u;
const uint OX_VRS_MODE_DEBUG_OVERLAY = 3u;

const float OX_VRS_LUMA_BIAS = 0.1;      // Weber floor: thresholds are relative to (mean luma + bias)
const float OX_VRS_QUARTER_FACTOR = 2.13; // error of quarter rate relative to half rate (Yang et al. 2019)

// Perceptual luma of a pre-exposed HDR colour: Reinhard to [0,1), then square root (≈ display gamma).
float oxVrsLuma(vec3 c) {
    float l = dot(max(c, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
    l = min(l, 65000.0); // inf / garbage guard
    return sqrt(l / (1.0 + l));
}

uint oxVrsEncode(uint lw, uint lh) { return (lw << 2u) | lh; }

// Coarsest log2 rate of one axis whose estimated error stays below the just-noticeable difference.
uint oxVrsContentAxis(float err, float meanLuma, float tau) {
    float jnd = tau * (meanLuma + OX_VRS_LUMA_BIAS);
    if (OX_VRS_QUARTER_FACTOR * err < jnd) return 2u;
    if (err < jnd) return 1u;
    return 0u;
}

uint oxVrsMotionAxis(float pixels, float motionPixels) {
    if (motionPixels <= 0.0) return 0u;
    if (pixels >= 3.0 * motionPixels) return 2u;
    if (pixels >= motionPixels) return 1u;
    return 0u;
}

uint oxVrsCocLevel(float cocPixels) {
    if (cocPixels >= 6.0) return 2u;
    if (cocPixels >= 3.0) return 1u;
    return 0u;
}

uint oxVrsPeripheryLevel(float dist, float start) {
    if (start <= 0.0 || start >= 1.0) return 0u;
    if (dist > start + (1.0 - start) * 0.5) return 2u;
    if (dist > start) return 1u;
    return 0u;
}

// Largest supported fragment size ≤ (lw, lh): shrinks the larger axis first.
uint oxVrsFitSupported(uint lw, uint lh, uint supportedMask) {
    for (int i = 0; i < 5; ++i) {
        if (((supportedMask >> oxVrsEncode(lw, lh)) & 1u) != 0u) return oxVrsEncode(lw, lh);
        if (lw >= lh && lw > 0u) lw--; else if (lh > 0u) lh--;
    }
    return 0u;
}

struct OxVrsTile {
    bool statsValid;
    float meanLuma;
    float errorX;
    float errorY;
    float motionX;
    float motionY;
    float minCoc;
    float periphery;
    bool prevValid;
    uint prevRate;
};

struct OxVrsParams {
    uint mode;
    float threshold;
    float hysteresis;
    float motionScale;
    float motionPixels;
    float periphery;
    bool depthOfField;
    uint maxLog2;
    uint supportedMask;
};

uint oxVrsDecide(OxVrsTile t, OxVrsParams p) {
    uint lw = 0u, lh = 0u;
    bool adaptive = p.mode == OX_VRS_MODE_ADAPTIVE || p.mode == OX_VRS_MODE_DEBUG_OVERLAY;
    if (adaptive && t.statsValid) {
        // The eye cannot resolve detail that moves: motion lowers the error (per axis).
        float ex = t.errorX / (1.0 + p.motionScale * t.motionX);
        float ey = t.errorY / (1.0 + p.motionScale * t.motionY);
        // Hysteresis: coarsen only below the stricter threshold, refine only above the looser one.
        float tStrict = p.threshold * (1.0 - p.hysteresis);
        float tLoose = p.threshold * (1.0 + p.hysteresis);
        uint sx = oxVrsContentAxis(ex, t.meanLuma, tStrict), lx = oxVrsContentAxis(ex, t.meanLuma, tLoose);
        uint sy = oxVrsContentAxis(ey, t.meanLuma, tStrict), ly = oxVrsContentAxis(ey, t.meanLuma, tLoose);
        if (t.prevValid) {
            lw = clamp((t.prevRate >> 2u) & 3u, sx, lx);
            lh = clamp(t.prevRate & 3u, sy, ly);
        } else {
            lw = sx;
            lh = sy;
        }
    }
    if (p.mode == OX_VRS_MODE_MOTION) {
        lw = max(lw, oxVrsMotionAxis(t.motionX, p.motionPixels));
        lh = max(lh, oxVrsMotionAxis(t.motionY, p.motionPixels));
    }
    if (p.mode != OX_VRS_MODE_FOVEATED && p.depthOfField && t.minCoc > 0.0) {
        uint c = oxVrsCocLevel(t.minCoc);
        lw = max(lw, c);
        lh = max(lh, c);
    }
    float start = p.mode == OX_VRS_MODE_FOVEATED && p.periphery <= 0.0 ? 0.5 : p.periphery;
    uint f = oxVrsPeripheryLevel(t.periphery, start);
    lw = min(max(lw, f), p.maxLog2);
    lh = min(max(lh, f), p.maxLog2);
    return oxVrsFitSupported(lw, lh, p.supportedMask);
}

#endif
