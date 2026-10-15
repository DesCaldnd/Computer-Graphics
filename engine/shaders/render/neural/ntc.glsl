// OxwaldEngine neural texture compression: per-sample decoding in material shaders (r.NeuralTextures
// InferenceOnSample). Mirrors ox::assets::ntcNetworkInput (assets/neural/ntc.hpp) and
// ox::render::neural::GpuNeuralMaterial (render/src/neural/neural_textures.hpp).
//
// Stochastic texture filtering: one texel of one mip is decoded per sample. The mip is chosen between floor/ceil of
// the footprint LOD with probability = fraction (trilinear), the texel between the 4 bilinear neighbours with
// probability = bilinear weight, and the sample position is jittered along the major axis of anisotropic footprints;
// TAA integrates the filter over frames. Mips at or above `neuralMips` use the BC fallback tail through the
// regular material path (hardware filtering).
//
//   #include <render/neural/ntc.glsl>
//   OxMaterialSample ms = oxSampleMaterialView(pc.view, m, uv0, color, N, T, gl_FrontFacing, VIEW.mipBias,
//                                              gl_FragCoord.xy, false);
#ifndef OX_NEURAL_NTC_GLSL
#define OX_NEURAL_NTC_GLSL

#extension GL_EXT_buffer_reference_uvec2 : require

#include <render/common/material.glsl>
#include <render/neural/mlp.glsl>

struct NtcMaterial {
    OxNnWeights weights;
    uint w0;
    uint b0;
    uint w1;
    uint b1;
    uint w2;
    uint b2;
    uint hidden;     // 32 or 64
    uint width;      // mip 0
    uint height;
    uint neuralMips; // mips [0, neuralMips) decoded by the network
    uint minMip;     // first resident neural mip (latent streaming)
    uint pairs;
    uint channels;   // 4-bit offsets: albedo | alpha << 4 | normal << 8 | orm << 12 | emissive << 16 (15 = absent)
    uint features;   // G0 features | G1 features << 8
    uint grids[8];   // sampled index of the latent 2D array per (pair, grid)
    uint g0Size[4];  // G0 grid size per pair: width | height << 16
    uint latentSampler;
    uint flags;      // bit 0 bilinear (4 decodes) instead of stochastic, bit 1 decode in the depth prepass
    float mipBias;
    uint pad;
};

layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer OxNeuralMaterials { NtcMaterial m[]; };

const uint OX_NTC_FILTER_BILINEAR = 1u;
const uint OX_NTC_PREPASS = 2u;
const float OX_NTC_TWO_PI = 6.28318530718;

uvec3 oxNtcPcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

vec3 oxNtcSrgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

// Network inputs of texel `t` of `mip` (fixed layout: G0 [0,16), G1 [16,24), position encoding [24,28), parity 28).
#define OX_NTC_FILL_INPUTS(NM, MIP, T, STORE)                                                                   \
    {                                                                                                           \
        uint pair_ = (MIP) >> 1u;                                                                               \
        vec2 msize_ = vec2(max((NM).width >> (MIP), 1u), max((NM).height >> (MIP), 1u));                        \
        vec2 uv_ = (vec2(T) + 0.5) / msize_;                                                                    \
        uint f0_ = (NM).features & 0xFFu, f1_ = ((NM).features >> 8u) & 0xFFu;                                  \
        uint g0_ = (NM).grids[pair_ * 2u], g1_ = (NM).grids[pair_ * 2u + 1u];                                   \
        for (uint l = 0u; l < 4u; ++l) {                                                                        \
            vec4 v_ = l * 4u < f0_ ? textureLod(sampler2DArray(OX_TEX2D_ARRAY(g0_), OX_SAMPLER((NM).latentSampler)), \
                                                vec3(uv_, float(l)), 0.0) * 2.0 - 1.0                           \
                                   : vec4(0.0);                                                                 \
            STORE(l * 4u, v_.x) STORE(l * 4u + 1u, v_.y) STORE(l * 4u + 2u, v_.z) STORE(l * 4u + 3u, v_.w)     \
        }                                                                                                       \
        for (uint l = 0u; l < 2u; ++l) {                                                                        \
            vec4 v_ = l * 4u < f1_ ? textureLod(sampler2DArray(OX_TEX2D_ARRAY(g1_), OX_SAMPLER((NM).latentSampler)), \
                                                vec3(uv_, float(l)), 0.0) * 2.0 - 1.0                           \
                                   : vec4(0.0);                                                                 \
            STORE(16u + l * 4u, v_.x) STORE(17u + l * 4u, v_.y) STORE(18u + l * 4u, v_.z) STORE(19u + l * 4u, v_.w) \
        }                                                                                                       \
        uint gs_ = (NM).g0Size[pair_];                                                                          \
        vec2 c_ = uv_ * vec2(float(gs_ & 0xFFFFu), float(gs_ >> 16u));                                          \
        vec2 fr_ = c_ - floor(c_);                                                                              \
        STORE(24u, sin(OX_NTC_TWO_PI * fr_.x)) STORE(25u, cos(OX_NTC_TWO_PI * fr_.x))                           \
        STORE(26u, sin(OX_NTC_TWO_PI * fr_.y)) STORE(27u, cos(OX_NTC_TWO_PI * fr_.y))                           \
        STORE(28u, ((MIP) & 1u) != 0u ? 1.0 : -1.0)                                                             \
        STORE(29u, 0.0) STORE(30u, 0.0) STORE(31u, 0.0)                                                         \
    }

#if defined(OX_NN_COOPVEC)
OX_NN_LAYER_CV(oxNtcL0h64, 32, 64)
OX_NN_LAYER_CV(oxNtcL1h64, 64, 64)
OX_NN_LAYER_CV(oxNtcL2h64, 64, 16)
OX_NN_LAYER_CV(oxNtcL0h32, 32, 32)
OX_NN_LAYER_CV(oxNtcL1h32, 32, 32)
OX_NN_LAYER_CV(oxNtcL2h32, 32, 16)
#define OX_NTC_STORE_CV(I, V) x[I] = float16_t(V);
void oxNtcDecode(NtcMaterial nm, uint mip, ivec2 t, out float y[16]) {
    coopvecNV<float16_t, 32> x;
    OX_NTC_FILL_INPUTS(nm, mip, t, OX_NTC_STORE_CV)
    OxNnWeightsF16 w = OxNnWeightsF16(nm.weights);
    coopvecNV<float16_t, 16> o;
    if (nm.hidden == 32u) {
        o = oxNtcL2h32(w, nm.w2, nm.b2, false, oxNtcL1h32(w, nm.w1, nm.b1, true, oxNtcL0h32(w, nm.w0, nm.b0, true, x)));
    } else {
        o = oxNtcL2h64(w, nm.w2, nm.b2, false, oxNtcL1h64(w, nm.w1, nm.b1, true, oxNtcL0h64(w, nm.w0, nm.b0, true, x)));
    }
    for (uint i = 0u; i < 16u; ++i) y[i] = float(o[i]);
}
#elif defined(OX_NN_FP16)
OX_NN_LAYER_F16(oxNtcL0h64, 32, 64)
OX_NN_LAYER_F16(oxNtcL1h64, 64, 64)
OX_NN_LAYER_F16(oxNtcL2h64, 64, 16)
OX_NN_LAYER_F16(oxNtcL0h32, 32, 32)
OX_NN_LAYER_F16(oxNtcL1h32, 32, 32)
OX_NN_LAYER_F16(oxNtcL2h32, 32, 16)
#define OX_NTC_STORE_F(I, V) xf[I] = (V);
void oxNtcDecode(NtcMaterial nm, uint mip, ivec2 t, out float y[16]) {
    float xf[32];
    OX_NTC_FILL_INPUTS(nm, mip, t, OX_NTC_STORE_F)
    f16vec2 x[16];
    for (uint i = 0u; i < 16u; ++i) x[i] = f16vec2(xf[2u * i], xf[2u * i + 1u]);
    f16vec2 o[8];
    if (nm.hidden == 32u) {
        f16vec2 h0[16], h1[16];
        oxNtcL0h32(nm.weights, nm.w0, nm.b0, true, x, h0);
        oxNtcL1h32(nm.weights, nm.w1, nm.b1, true, h0, h1);
        oxNtcL2h32(nm.weights, nm.w2, nm.b2, false, h1, o);
    } else {
        f16vec2 h0[32], h1[32];
        oxNtcL0h64(nm.weights, nm.w0, nm.b0, true, x, h0);
        oxNtcL1h64(nm.weights, nm.w1, nm.b1, true, h0, h1);
        oxNtcL2h64(nm.weights, nm.w2, nm.b2, false, h1, o);
    }
    for (uint i = 0u; i < 8u; ++i) y[2u * i] = float(o[i].x), y[2u * i + 1u] = float(o[i].y);
}
#else
OX_NN_LAYER_F32(oxNtcL0h64, 32, 64)
OX_NN_LAYER_F32(oxNtcL1h64, 64, 64)
OX_NN_LAYER_F32(oxNtcL2h64, 64, 16)
OX_NN_LAYER_F32(oxNtcL0h32, 32, 32)
OX_NN_LAYER_F32(oxNtcL1h32, 32, 32)
OX_NN_LAYER_F32(oxNtcL2h32, 32, 16)
#define OX_NTC_STORE_F(I, V) x[I] = (V);
void oxNtcDecode(NtcMaterial nm, uint mip, ivec2 t, out float y[16]) {
    float x[32];
    OX_NTC_FILL_INPUTS(nm, mip, t, OX_NTC_STORE_F)
    if (nm.hidden == 32u) {
        float h0[32], h1[32];
        oxNtcL0h32(nm.weights, nm.w0, nm.b0, true, x, h0);
        oxNtcL1h32(nm.weights, nm.w1, nm.b1, true, h0, h1);
        oxNtcL2h32(nm.weights, nm.w2, nm.b2, false, h1, y);
    } else {
        float h0[64], h1[64];
        oxNtcL0h64(nm.weights, nm.w0, nm.b0, true, x, h0);
        oxNtcL1h64(nm.weights, nm.w1, nm.b1, true, h0, h1);
        oxNtcL2h64(nm.weights, nm.w2, nm.b2, false, h1, y);
    }
}
#endif

// Decoded channels of one texel: albedo / emissive sRGB encoded, normal xy encoded, ORM linear.
struct OxNtcTexel {
    vec4 albedo;
    vec2 normal;
    vec3 orm;
    vec3 emissive;
};

OxNtcTexel oxNtcTexel(NtcMaterial nm, uint mip, ivec2 t) {
    float y[16];
    oxNtcDecode(nm, mip, t, y);
    for (uint i = 0u; i < 16u; ++i) y[i] = clamp(y[i], 0.0, 1.0);
    OxNtcTexel o;
    uint ch = nm.channels;
    uint a = ch & 15u, al = (ch >> 4u) & 15u, n = (ch >> 8u) & 15u, r = (ch >> 12u) & 15u, e = (ch >> 16u) & 15u;
    o.albedo = a != 15u ? vec4(y[a], y[a + 1u], y[a + 2u], al != 15u ? y[al] : 1.0) : vec4(1.0);
    o.normal = n != 15u ? vec2(y[n], y[n + 1u]) : vec2(0.5);
    o.orm = r != 15u ? vec3(y[r], y[r + 1u], y[r + 2u]) : vec3(1.0);
    o.emissive = e != 15u ? vec3(y[e], y[e + 1u], y[e + 2u]) : vec3(1.0);
    return o;
}

ivec2 oxNtcWrap(ivec2 t, ivec2 size) { return ((t % size) + size) % size; }

#ifndef OX_NTC_DECODE_ONLY // material sampling needs derivatives (fragment shaders)

// Material sample through the neural decoder (m.flags & OX_MATERIAL_NEURAL, VIEW.neuralMaterials set).
OxMaterialSample oxSampleMaterialNtc(ViewBuffer view, Material m, vec2 uv0, vec4 vertexColor, vec3 N, vec4 T,
                                     bool frontFacing, float mipBias, vec2 pixel, bool prepass) {
    NtcMaterial nm = view.v.neuralMaterials.m[m.flags >> 8u];
    vec2 uv = oxMaterialUv(m, uv0);
    vec2 size0 = vec2(float(nm.width), float(nm.height));
    vec2 dx = dFdx(uv) * size0, dy = dFdy(uv) * size0;
    if (prepass && (nm.flags & OX_NTC_PREPASS) == 0u) return oxSampleMaterial(m, uv0, vertexColor, N, T, frontFacing, mipBias);

    uvec3 rnd = oxNtcPcg3d(uvec3(uvec2(pixel), view.v.frameIndex));
    vec4 r = vec4(rnd.xyz, rnd.x ^ (rnd.y >> 7u)) * (1.0 / 4294967296.0);
    // Anisotropic footprint: LOD from the major axis divided by up to 8 samples, one of which (random) is taken.
    float lx = dot(dx, dx), ly = dot(dy, dy);
    float major = sqrt(max(lx, ly)), minor = sqrt(min(lx, ly));
    float aniso = min(ceil(major / max(minor, 1e-6)), 8.0);
    float lod = log2(max(major / aniso, 1e-6)) + mipBias + nm.mipBias;
    bool bilinear = (nm.flags & OX_NTC_FILTER_BILINEAR) != 0u;
    if (!bilinear && aniso > 1.0) uv += (lx > ly ? dFdx(uv) : dFdy(uv)) * (r.w - 0.5);
    float base = floor(lod);
    uint mip = bilinear ? uint(max(round(lod), 0.0)) : uint(max(base + (r.x < lod - base ? 1.0 : 0.0), 0.0));
    mip = max(mip, nm.minMip);
    if (mip >= nm.neuralMips) return oxSampleMaterial(m, uv0, vertexColor, N, T, frontFacing, mipBias);

    ivec2 msize = ivec2(max(nm.width >> mip, 1u), max(nm.height >> mip, 1u));
    vec2 p = uv * vec2(msize) - 0.5;
    vec2 f = p - floor(p);
    ivec2 t0 = ivec2(floor(p));
    OxNtcTexel tex;
    if (bilinear) {
        OxNtcTexel a = oxNtcTexel(nm, mip, oxNtcWrap(t0, msize));
        OxNtcTexel b = oxNtcTexel(nm, mip, oxNtcWrap(t0 + ivec2(1, 0), msize));
        OxNtcTexel c = oxNtcTexel(nm, mip, oxNtcWrap(t0 + ivec2(0, 1), msize));
        OxNtcTexel d = oxNtcTexel(nm, mip, oxNtcWrap(t0 + ivec2(1, 1), msize));
        tex.albedo = mix(mix(a.albedo, b.albedo, f.x), mix(c.albedo, d.albedo, f.x), f.y);
        tex.normal = mix(mix(a.normal, b.normal, f.x), mix(c.normal, d.normal, f.x), f.y);
        tex.orm = mix(mix(a.orm, b.orm, f.x), mix(c.orm, d.orm, f.x), f.y);
        tex.emissive = mix(mix(a.emissive, b.emissive, f.x), mix(c.emissive, d.emissive, f.x), f.y);
    } else {
        ivec2 t = t0 + ivec2(r.y < f.x ? 1 : 0, r.z < f.y ? 1 : 0);
        tex = oxNtcTexel(nm, mip, oxNtcWrap(t, msize));
    }

    uint ch = nm.channels;
    OxMaterialSample o;
    o.baseColor = m.baseColor * vertexColor;
    if ((ch & 15u) != 15u) o.baseColor *= vec4(oxNtcSrgbToLinear(tex.albedo.rgb), tex.albedo.a);
    o.metallic = m.metallic;
    o.perceptualRoughness = m.roughness;
    o.occlusion = 1.0;
    if (((ch >> 12u) & 15u) != 15u) {
        o.occlusion = mix(1.0, tex.orm.r, m.occlusionStrength);
        o.perceptualRoughness *= tex.orm.g;
        o.metallic *= tex.orm.b;
    }
    o.emissive = m.emissive;
    if (((ch >> 16u) & 15u) != 15u) o.emissive *= oxNtcSrgbToLinear(tex.emissive);
    vec3 n = normalize(N);
    if ((m.flags & OX_MATERIAL_DOUBLE_SIDED) != 0u && !frontFacing) n = -n;
    if (((ch >> 8u) & 15u) != 15u && dot(T.xyz, T.xyz) > 1e-8) {
        vec2 xy = (tex.normal * 2.0 - 1.0) * m.normalStrength;
        vec3 tn = vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
        vec3 tt = normalize(T.xyz - n * dot(n, T.xyz));
        vec3 b = cross(n, tt) * (T.w < 0.0 ? -1.0 : 1.0);
        n = normalize(tt * tn.x + b * tn.y + n * tn.z);
    }
    o.normal = n;
    return o;
}

// Material sample for the forward / prepass shaders: neural materials decode per sample, everything else (and
// every material when no table is bound) takes the regular path.
OxMaterialSample oxSampleMaterialView(ViewBuffer view, Material m, vec2 uv0, vec4 vertexColor, vec3 N, vec4 T,
                                      bool frontFacing, float mipBias, vec2 pixel, bool prepass) {
#if !defined(OX_NTC_DISABLED) // OX_NTC_DISABLED: pipelines used while r.NeuralTextures is not InferenceOnSample
    if ((m.flags & OX_MATERIAL_NEURAL) != 0u && uvec2(view.v.neuralMaterials) != uvec2(0u))
        return oxSampleMaterialNtc(view, m, uv0, vertexColor, N, T, frontFacing, mipBias, pixel, prepass);
#endif
    return oxSampleMaterial(m, uv0, vertexColor, N, T, frontFacing, mipBias);
}
#endif // OX_NTC_DECODE_ONLY

#endif
