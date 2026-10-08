// OxwaldEngine render / world: octahedral impostors (hemi-octahedral frame grid baked at load, see impostor_bake.*).
#ifndef OX_RENDER_WORLD_IMPOSTOR_COMMON_GLSL
#define OX_RENDER_WORLD_IMPOSTOR_COMMON_GLSL

#include "vegetation_common.glsl"

OX_RENDER_PUSH(VegInstanceBuffer instances; VegRecordBuffer records; VegFrameBuffer frame; VegProtoBuffer protos; OxWorldMatrixBuffer matrices; uint proto; uint material; uint entityId; uint lightDirOct; uint inputs[4];);

#endif
