// CDLOD terrain vertex morphing. Mirrors ox::world::cdlodMorphFactor / cdlodMorphVertex
// (engine/world/src/terrain_lod.cpp).
// Instance data = ox::world::TerrainPatchGpu (32 bytes):
//   vec4 offsetSizeLod;  xy = node min corner (world XZ), z = node size (m), w = lod
//   vec4 morph;          x = morphStart, y = morphEnd, z = 1/(end-start), w = quadrant mask
// Vertex data = ox::world::TerrainGridVertex: vec2 uv in [0,1], float skirt (1 = skirt vertex).
#ifndef OX_WORLD_CDLOD_GLSL
#define OX_WORLD_CDLOD_GLSL

float oxCdlodMorphFactor(float dist, vec4 morph) {
    return clamp((dist - morph.x) * morph.z, 0.0, 1.0);
}

vec2 oxCdlodMorphVertex(vec2 uv, float gridDim, float morphK) {
    vec2 frac = fract(uv * gridDim * 0.5) * 2.0;
    return uv - frac / gridDim * morphK;
}

// Typical use (heightmap sampled with a sampler covering the whole terrain):
//   vec2 xz   = inst.offsetSizeLod.xy + uv * inst.offsetSizeLod.z;
//   float h   = sampleHeight(xz);
//   float k   = oxCdlodMorphFactor(distance(cameraPos, vec3(xz.x, h, xz.y)), inst.morph);
//   uv        = oxCdlodMorphVertex(uv, gridDim, k);
//   xz        = inst.offsetSizeLod.xy + uv * inst.offsetSizeLod.z;
//   h         = sampleHeight(xz) - skirt * skirtDepth(lod);

#endif
