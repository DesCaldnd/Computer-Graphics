// Auto exposure shared definitions (histogram layout, EV conversions).
#ifndef OX_PP_EXPOSURE_GLSL
#define OX_PP_EXPOSURE_GLSL

#include <render/postprocess/pp_common.glsl>

const uint kExposureBins = 128u;
const float kExposureMinEV = -10.0;
const float kExposureMaxEV = 22.0;

OX_BUFFER(ExposureHistogram, { uint bins[]; });
// Persistent per-view state: x = adapted EV100, y = valid (0/1), z = last target EV100, w = exposure.
OX_BUFFER(ExposureState, { vec4 state; });
OX_BUFFER(ExposureReadback, { float values[]; });

// Reflected-light meter calibration K = 12.5: EV100 = log2(L · 100 / K).
float exposureLuminanceToEV100(float lum) { return log2(max(lum, 1e-10) * 100.0 / 12.5); }
// Saturation based exposure: maximum luminance 1.2 · 2^EV100 maps to 1.
float exposureFromEV100(float ev) { return 1.0 / (1.2 * exp2(ev)); }

uint exposureBin(float ev) {
    float t = clamp((ev - kExposureMinEV) / (kExposureMaxEV - kExposureMinEV), 0.0, 0.99999);
    return uint(t * float(kExposureBins));
}
float exposureBinEV(uint bin) {
    return kExposureMinEV + (float(bin) + 0.5) / float(kExposureBins) * (kExposureMaxEV - kExposureMinEV);
}

#endif
