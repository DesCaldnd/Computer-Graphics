// OxwaldEngine neural: small MLP inference in any shader stage.
//
// Weights blob = ox::nn::packFp16 (assets/neural/mlp.hpp): per layer an FP16 weight matrix [outPad x inPad]
// (row-major, zero padded to multiples of 16) followed by FP16 biases [outPad]; offsets are in halves and 64-byte
// aligned. Pass the blob's device address (render::NeuralNetwork::address()) and the per-layer offsets
// (NeuralNetwork::layers()) in push constants or a table.
//
// Layer functions are generated for compile-time sizes (fully unrolled loops keep activations in registers):
//   OX_NN_LAYER_F32(name, NIN, NOUT)  float x[NIN] -> float y[NOUT]          portable (FP32 math, FP16 storage)
//   OX_NN_LAYER_F16(name, NIN, NOUT)  f16vec2 x[NIN/2] -> f16vec2 y[NOUT/2]  needs shaderFloat16 (define OX_NN_FP16)
//   OX_NN_LAYER_CV(name, NIN, NOUT)   coopvecNV<float16_t, NIN> -> <NOUT>     VK_NV_cooperative_vector (OX_NN_COOPVEC)
// The cooperative-matrix path (VK_KHR_cooperative_matrix, compute only, 16 samples per subgroup) lives in
// render/neural/mlp_coopmat.glsl.
#ifndef OX_NEURAL_MLP_GLSL
#define OX_NEURAL_MLP_GLSL

#include <common/bindless.glsl>

#if defined(OX_NN_FP16) || defined(OX_NN_COOPVEC)
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#endif
#if defined(OX_NN_COOPVEC)
#extension GL_EXT_shader_16bit_storage : require
#extension GL_NV_cooperative_vector : require
#endif

// Two halves per uint: readable on every device (no 16-bit storage needed).
OX_READONLY_BUFFER(OxNnWeights, { uint u[]; });

float oxNnHalf(OxNnWeights w, uint halfIndex) {
    vec2 p = unpackHalf2x16(w.u[halfIndex >> 1u]);
    return (halfIndex & 1u) != 0u ? p.y : p.x;
}

#define OX_NN_LAYER_F32(NAME, NIN, NOUT)                                                                       \
    void NAME(OxNnWeights w, uint wOff, uint bOff, bool relu, float x[NIN], out float y[NOUT]) {             \
        for (uint o = 0u; o < uint(NOUT); ++o) {                                                               \
            uint row = (wOff + o * uint(NIN)) >> 1u;                                                           \
            float acc = oxNnHalf(w, bOff + o);                                                                 \
            for (uint i = 0u; i < uint(NIN) / 2u; ++i) {                                                       \
                vec2 ww = unpackHalf2x16(w.u[row + i]);                                                        \
                acc = fma(ww.x, x[2u * i], acc);                                                               \
                acc = fma(ww.y, x[2u * i + 1u], acc);                                                          \
            }                                                                                                  \
            y[o] = relu ? max(acc, 0.0) : acc;                                                                 \
        }                                                                                                      \
    }

#if defined(OX_NN_FP16)
// Packed half2 math, FP16 accumulation per pair of inputs (like the tensor-core paths).
#define OX_NN_LAYER_F16(NAME, NIN, NOUT)                                                                       \
    void NAME(OxNnWeights w, uint wOff, uint bOff, bool relu, f16vec2 x[NIN / 2], out f16vec2 y[NOUT / 2]) { \
        for (uint o = 0u; o < uint(NOUT); o += 2u) {                                                           \
            uint r0 = (wOff + o * uint(NIN)) >> 1u, r1 = r0 + (uint(NIN) >> 1u);                               \
            f16vec2 a0 = f16vec2(0.0), a1 = f16vec2(0.0);                                                      \
            for (uint i = 0u; i < uint(NIN) / 2u; ++i) {                                                       \
                a0 = fma(unpackFloat2x16(w.u[r0 + i]), x[i], a0);                                              \
                a1 = fma(unpackFloat2x16(w.u[r1 + i]), x[i], a1);                                              \
            }                                                                                                  \
            f16vec2 r = f16vec2(a0.x + a0.y, a1.x + a1.y) + unpackFloat2x16(w.u[(bOff + o) >> 1u]);         \
            y[o >> 1u] = relu ? max(r, f16vec2(0.0)) : r;                                                      \
        }                                                                                                      \
    }
#endif

#if defined(OX_NN_COOPVEC)
OX_READONLY_BUFFER(OxNnWeightsF16, { float16_t h[]; });
// Row-major FP16 matrix, offsets / stride in bytes (64-byte aligned offsets, 16-byte multiple stride).
#define OX_NN_LAYER_CV(NAME, NIN, NOUT)                                                                        \
    coopvecNV<float16_t, NOUT> NAME(OxNnWeightsF16 w, uint wOff, uint bOff, bool relu,                         \
                                    coopvecNV<float16_t, NIN> x) {                                             \
        coopvecNV<float16_t, NOUT> y;                                                                          \
        coopVecMatMulAddNV(y, x, gl_ComponentTypeFloat16NV, w.h, wOff * 2u, gl_ComponentTypeFloat16NV, w.h,    \
                           bOff * 2u, gl_ComponentTypeFloat16NV, NOUT, NIN,                                    \
                           gl_CooperativeVectorMatrixLayoutRowMajorNV, false, uint(NIN) * 2u);                 \
        if (relu) y = max(y, coopvecNV<float16_t, NOUT>(0.0));                                                \
        return y;                                                                                              \
    }
#endif

#endif
