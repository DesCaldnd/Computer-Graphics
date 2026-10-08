// OxwaldEngine volumetrics: cloud layer helpers shared by the cloud passes and the fog (cloud shadows).
#ifndef OX_VOLUMETRICS_CLOUD_COMMON_GLSL
#define OX_VOLUMETRICS_CLOUD_COMMON_GLSL

float oxRemap(float v, float lo, float hi, float newLo, float newHi) {
    return newLo + (v - lo) / max(hi - lo, 1e-5) * (newHi - newLo);
}

// Local coverage from the weather map (r ∈ [0, 1], mean 0.5) and the layer's global coverage.
float oxCloudLocalCoverage(float weather, float coverage) {
    return clamp(coverage + (weather - 0.5) * 0.7, 0.0, 1.0) * smoothstep(0.0, 0.05, coverage);
}

// Vertical density profile for a normalised height h ∈ [0, 1] in the layer and a cloud type
// (0 stratus: thin band at the bottom, 0.5 cumulus, 1 cumulonimbus: full height).
float oxCloudHeightGradient(float h, float type) {
    float top = mix(0.25, 1.0, type);
    float bottomFade = smoothstep(0.0, mix(0.05, 0.12, type), h);
    float topFade = 1.0 - smoothstep(top * mix(0.4, 0.7, type), top, h);
    return bottomFade * topFade;
}

#endif
