// OxwaldEngine bindless resource access.
//
// Layout (matches ox::rhi::Device): set 0
//   binding 0: sampled images  (texture2D / textureCube / texture2DArray / texture3D / utexture2D aliases)
//   binding 1: storage images  (one alias per format, see OX_STORAGE_IMAGE_FORMATS)
//   binding 2: samplers        (indices 0..5 are the DefaultSampler constants below)
// Buffers are not descriptors: pass their device addresses in push constants (≤ 128 bytes) and declare
// buffer_reference types with OX_BUFFER(...). Everything uses the scalar block layout (C++ struct packing).
#ifndef OX_BINDLESS_GLSL
#define OX_BINDLESS_GLSL

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D oxTextures2D[];
layout(set = 0, binding = 0) uniform texture2DArray oxTextures2DArray[];
layout(set = 0, binding = 0) uniform textureCube oxTexturesCube[];
layout(set = 0, binding = 0) uniform texture3D oxTextures3D[];
layout(set = 0, binding = 0) uniform utexture2D oxUTextures2D[];
layout(set = 0, binding = 2) uniform sampler oxSamplers[];
layout(set = 0, binding = 2) uniform samplerShadow oxShadowSamplers[];

layout(set = 0, binding = 1, rgba8) uniform image2D oxImages2D_rgba8[];
layout(set = 0, binding = 1, rgba16f) uniform image2D oxImages2D_rgba16f[];
layout(set = 0, binding = 1, rgba32f) uniform image2D oxImages2D_rgba32f[];
layout(set = 0, binding = 1, r32f) uniform image2D oxImages2D_r32f[];
layout(set = 0, binding = 1, r32ui) uniform uimage2D oxImages2D_r32ui[];
layout(set = 0, binding = 1, rgba16f) uniform image2DArray oxImages2DArray_rgba16f[];
layout(set = 0, binding = 1, rgba16f) uniform image3D oxImages3D_rgba16f[];

// DefaultSampler (ox::rhi::DefaultSampler)
const uint OX_SAMPLER_LINEAR_REPEAT = 0u;
const uint OX_SAMPLER_LINEAR_CLAMP = 1u;
const uint OX_SAMPLER_NEAREST_REPEAT = 2u;
const uint OX_SAMPLER_NEAREST_CLAMP = 3u;
const uint OX_SAMPLER_ANISO_REPEAT = 4u;
const uint OX_SAMPLER_SHADOW = 5u;
const uint OX_INVALID_INDEX = 0xFFFFFFFFu;

#define OX_TEX2D(idx) oxTextures2D[nonuniformEXT(idx)]
#define OX_TEX2D_ARRAY(idx) oxTextures2DArray[nonuniformEXT(idx)]
#define OX_TEXCUBE(idx) oxTexturesCube[nonuniformEXT(idx)]
#define OX_TEX3D(idx) oxTextures3D[nonuniformEXT(idx)]
#define OX_SAMPLER(idx) oxSamplers[nonuniformEXT(idx)]

#define OX_SAMPLE_2D(tex, smp, uv) texture(sampler2D(OX_TEX2D(tex), OX_SAMPLER(smp)), (uv))
#define OX_SAMPLE_2D_LOD(tex, smp, uv, lod) textureLod(sampler2D(OX_TEX2D(tex), OX_SAMPLER(smp)), (uv), (lod))
#define OX_SAMPLE_2D_ARRAY(tex, smp, uvw) texture(sampler2DArray(OX_TEX2D_ARRAY(tex), OX_SAMPLER(smp)), (uvw))
#define OX_SAMPLE_CUBE(tex, smp, dir) texture(samplerCube(OX_TEXCUBE(tex), OX_SAMPLER(smp)), (dir))
#define OX_SAMPLE_3D(tex, smp, uvw) texture(sampler3D(OX_TEX3D(tex), OX_SAMPLER(smp)), (uvw))
#define OX_SAMPLE_SHADOW(tex, uvz) texture(sampler2DShadow(OX_TEX2D(tex), oxShadowSamplers[OX_SAMPLER_SHADOW]), (uvz))
#define OX_FETCH_2D(tex, coord, lod) texelFetch(OX_TEX2D(tex), (coord), (lod))

#define OX_IMAGE2D(fmt, idx) oxImages2D_##fmt[nonuniformEXT(idx)]
#define OX_IMAGE_STORE_2D(fmt, idx, coord, value) imageStore(OX_IMAGE2D(fmt, idx), (coord), (value))
#define OX_IMAGE_LOAD_2D(fmt, idx, coord) imageLoad(OX_IMAGE2D(fmt, idx), (coord))

// Buffer device address types:
//   OX_BUFFER(Vertices, { Vertex v[]; });       -> layout(buffer_reference, scalar) buffer Vertices { Vertex v[]; };
//   OX_READONLY_BUFFER(Indices, { uint i[]; });
#define OX_BUFFER(Name, Body) layout(buffer_reference, scalar, buffer_reference_align = 4) buffer Name Body
#define OX_READONLY_BUFFER(Name, Body) layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Name Body

// Push constants (≤ 128 bytes), scalar layout:
//   OX_PUSH_CONSTANTS({ Vertices vertices; uint albedo; }) -> accessible as pc.vertices, pc.albedo
#define OX_PUSH_CONSTANTS(Body) layout(push_constant, scalar) uniform OxPushConstants Body pc

#endif
