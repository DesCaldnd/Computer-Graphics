// Push constants of the water passes (view, scene + 64 bytes).
#include "water_common.glsl"
layout(push_constant, scalar) uniform OxPushConstants {
    ViewBuffer view;
    SceneBuffer scene;
    WaterBuffer water;
    uint refraction;     // SceneColorRefraction
    uint refractionMips;
    uint sceneDepth;     // SceneDepthCopy (water surface) / Depth (caustics, underwater)
    uint planar;         // PlanarReflection (OX_INVALID_INDEX = SSR / probes)
    uint hdr;            // underwater: SceneColorHDR source
    uint shadowMask;     // caustics: ShadowMask
    uint fog;            // VolumetricFog
    uint ssrSteps;
    uint gridCells;
    uint flags;
} pc;
#define WATER pc.water.w
