// Colour grading LUT encoding shared by the bake and the composite.
#ifndef OX_PP_GRADING_GLSL
#define OX_PP_GRADING_GLSL

#include <render/postprocess/pp_common.glsl>

const float kGradingMidGreyLog = -2.47393; // log2(0.18)
const float kGradingLogMin = -12.47393;    // mid grey − 10 EV
const float kGradingLogMax = 4.026069;     // mid grey + 6.5 EV

vec3 gradingLutDecode(vec3 t) { return exp2(mix(vec3(kGradingLogMin), vec3(kGradingLogMax), t)); }
vec3 gradingLutEncode(vec3 x) {
    return clamp((log2(max(x, vec3(1e-10))) - kGradingLogMin) / (kGradingLogMax - kGradingLogMin), 0.0, 1.0);
}

// Applies the LUT to exposed linear scene values (values above the range are extrapolated linearly).
vec3 gradingApply(uint lut, float size, vec3 x) {
    vec3 t = gradingLutEncode(x);
    vec3 coord = t * ((size - 1.0) / size) + 0.5 / size;
    vec3 graded = OX_SAMPLE_3D(lut, OX_SAMPLER_LINEAR_CLAMP, coord).rgb;
    float maxV = exp2(kGradingLogMax);
    float over = ppMax3(x) / maxV;
    return over > 1.0 ? graded * over : graded;
}

#endif
