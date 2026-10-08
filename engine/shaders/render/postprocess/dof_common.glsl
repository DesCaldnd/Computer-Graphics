// Depth of field shared helpers. Thin lens circle of confusion, signed (negative = near field, positive = far).
#ifndef OX_PP_DOF_GLSL
#define OX_PP_DOF_GLSL

#include <render/postprocess/pp_common.glsl>

// cocScale = 0.5 · A·f / (S − f) / sensorHeight · imageHeight  (radius in pixels of the image the scale was made for)
// radius(D) = cocScale · (1 − S / D)
float dofCocRadius(float viewDepth, float focusDistance, float cocScale, float maxRadius) {
    float c = cocScale * (1.0 - focusDistance / max(viewDepth, 1e-4));
    return clamp(c, -maxRadius, maxRadius);
}

#endif
