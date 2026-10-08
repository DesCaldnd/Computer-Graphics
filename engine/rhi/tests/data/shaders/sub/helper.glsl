#ifndef TEST_HELPER_GLSL
#define TEST_HELPER_GLSL
#include "constants.glsl"
uint helperValue(uint i) { return i * HELPER_SCALE + OFFSET_VALUE; }
#endif
