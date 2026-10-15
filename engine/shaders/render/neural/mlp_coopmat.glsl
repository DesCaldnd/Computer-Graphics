// OxwaldEngine neural: MLP layers on tensor cores via VK_KHR_cooperative_matrix (16x16x16, FP16 inputs).
// Compute shaders only. Every subgroup pushes tiles of 16 samples through the three layers with two ping-pong
// activation buffers of its own in shared memory (no workgroup barriers between layers); bias tiles are staged once
// per workgroup. Weight tiles are loaded straight from the weights buffer (L1/L2 resident); OX_NN_CM_SHARED_W stages
// them in shared memory instead (slower on the RTX 3080: 44 KB of shared memory per workgroup halves the occupancy).
// Accumulators are FP16 when OX_NN_CM_FP16ACC is defined (DeviceCaps::cooperativeMatrixFp16Accumulate; twice the rate
// on GeForce GPUs), FP32 otherwise. Requires render/neural/mlp.glsl first (OxNnWeights).
//
// Shared-memory rows are padded by OX_NN_CM_PAD halves: with unpadded 64-half (128-byte) rows every row of a 16x16
// tile starts in the same bank and each coopMatLoad / coopMatStore serialises 16 ways. Padding + loading weights from
// the buffer took 8.3M samples (32 -> 64 -> 64 -> 16) from 65 ms to 8.7 ms on an RTX 3080 (cooperative vectors:
// 4.6 ms, so bestComputePath prefers those).
//
//   oxNnCoopStageLayer(weights, oxNnCoopW0(), w0, IN, H);   // all invocations, once per workgroup (no-op by default)
//   oxNnCoopStageLayer(weights, oxNnCoopW1(IN, H), w1, H, H);
//   oxNnCoopStageLayer(weights, oxNnCoopW2(IN, H), w2, H, OUT);
//   oxNnCoopStageBias(weights, layer, biasOffset, outPad);  // per layer (tile slot `layer`)
//   barrier();
//   // per tile: fill oxNnAct[oxNnActBase(sg, 0) + row * oxNnActStride(IN) + col] (FP16) with 16 inputs, then
//   oxNnCoopLayer(sg, 0, weights, w0, oxNnCoopW0(), 0, IN, H, true);        // buffer 0 -> 1
//   oxNnCoopLayer(sg, 1, weights, w1, oxNnCoopW1(IN, H), 1, H, H, true);    // 1 -> 0
//   oxNnCoopLayer(sg, 0, weights, w2, oxNnCoopW2(IN, H), 2, H, OUT, false); // 0 -> 1: read [oxNnActBase(sg, 1) + row * oxNnActStride(OUT) + col]
// (oxNnCoopStageLayer / the oxNnCoopW* shared offsets only matter with OX_NN_CM_SHARED_W.)
#ifndef OX_NEURAL_MLP_COOPMAT_GLSL
#define OX_NEURAL_MLP_COOPMAT_GLSL

#extension GL_KHR_cooperative_matrix : require
#extension GL_KHR_memory_scope_semantics : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_16bit_storage : require

#ifndef OX_NN_CM_MAXW
#define OX_NN_CM_MAXW 64
#endif
#ifndef OX_NN_CM_SUBGROUPS
#define OX_NN_CM_SUBGROUPS 4
#endif
#define OX_NN_CM_PAD 8 // halves (16 bytes): keeps 16-byte alignment, shifts every row by 4 banks
#ifndef OX_NN_CM_WEIGHTS
#define OX_NN_CM_WEIGHTS 8448 // halves: 32 -> 64 -> 64 -> 16 with padded rows needs 64*40 + 64*72 + 16*72 = 8320
#endif
#define OX_NN_CM_LAYERS 3
#define OX_NN_CM_ACT (16 * (OX_NN_CM_MAXW + OX_NN_CM_PAD))

#if defined(OX_NN_CM_FP16ACC)
#define OX_NN_ACC_T float16_t
#else
#define OX_NN_ACC_T float
#endif

#if defined(OX_NN_CM_SHARED_W)
shared float16_t oxNnW[OX_NN_CM_WEIGHTS];
#endif
shared OX_NN_ACC_T oxNnBias[OX_NN_CM_LAYERS * OX_NN_CM_ACT]; // per layer: 16 identical (padded) rows of the bias
shared float16_t oxNnAct[OX_NN_CM_SUBGROUPS * 2 * OX_NN_CM_ACT];

uint oxNnActBase(uint sg, uint buf) { return (sg * 2u + buf) * uint(OX_NN_CM_ACT); }
uint oxNnActStride(uint n) { return n + uint(OX_NN_CM_PAD); }
// Shared offsets of the padded weight matrices [nout x (nin + PAD)].
uint oxNnCoopW0() { return 0u; }
uint oxNnCoopW1(uint nin, uint h) { return h * (nin + uint(OX_NN_CM_PAD)); }
uint oxNnCoopW2(uint nin, uint h) { return oxNnCoopW1(nin, h) + h * (h + uint(OX_NN_CM_PAD)); }

// Row-major W [nout x nin] at half offset `wOff` of the blob (64-byte aligned, nin a multiple of 16) -> shared rows
// of nin + PAD halves at `sOff`.
void oxNnCoopStageLayer(OxNnWeights w, uint sOff, uint wOff, uint nin, uint nout) {
#if defined(OX_NN_CM_SHARED_W)
    uint pairsPerRow = nin / 2u, stride = nin + uint(OX_NN_CM_PAD);
    for (uint i = gl_LocalInvocationIndex; i < nout * pairsPerRow; i += gl_WorkGroupSize.x) {
        uint row = i / pairsPerRow, col = 2u * (i % pairsPerRow);
        uint dst = sOff + row * stride + col;
        if (dst + 1u >= uint(OX_NN_CM_WEIGHTS)) continue;
        f16vec2 v = unpackFloat2x16(w.u[(wOff >> 1u) + i]);
        oxNnW[dst] = v.x;
        oxNnW[dst + 1u] = v.y;
    }
#endif
}

void oxNnCoopStageBias(OxNnWeights w, uint layer, uint biasOffset, uint outPad) {
    for (uint i = gl_LocalInvocationIndex; i < 16u * outPad; i += gl_WorkGroupSize.x)
        oxNnBias[layer * uint(OX_NN_CM_ACT) + (i / outPad) * oxNnActStride(outPad) + i % outPad] =
            OX_NN_ACC_T(oxNnHalf(w, biasOffset + i % outPad));
}

// Activations of subgroup `sg` in buffer `buf` [16 x nin] -> act(W x + b) in buffer buf ^ 1 [16 x nout].
// `sOff` = shared offset of the staged layer (oxNnCoopW0/1/2), `layer` = bias tile slot.
void oxNnCoopLayer(uint sg, uint buf, OxNnWeights w, uint wOff, uint sOff, uint layer, uint nin, uint nout, bool relu) {
    uint src = oxNnActBase(sg, buf), dst = oxNnActBase(sg, buf ^ 1u);
    uint sIn = oxNnActStride(nin), sOut = oxNnActStride(nout), sW = nin + uint(OX_NN_CM_PAD);
    for (uint n = 0u; n < nout / 16u; ++n) {
        coopmat<OX_NN_ACC_T, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator> c;
        coopMatLoad(c, oxNnBias, layer * uint(OX_NN_CM_ACT) + n * 16u, sOut, gl_CooperativeMatrixLayoutRowMajor);
        for (uint k = 0u; k < nin / 16u; ++k) {
            coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA> a;
            coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB> b;
            coopMatLoad(a, oxNnAct, src + k * 16u, sIn, gl_CooperativeMatrixLayoutRowMajor);
            // Row-major W [nout x nin] read column-major = W^T tile (K x N).
#if defined(OX_NN_CM_SHARED_W)
            coopMatLoad(b, oxNnW, sOff + n * 16u * sW + k * 16u, sW, gl_CooperativeMatrixLayoutColumnMajor);
#else
            // Straight from the weights buffer (L1 / L2 resident; offset and stride in uints = 2 halves).
            coopMatLoad(b, w.u, (wOff + n * 16u * nin + k * 16u) >> 1u, nin >> 1u, gl_CooperativeMatrixLayoutColumnMajor);
#endif
            c = coopMatMulAdd(a, b, c);
        }
        if (relu)
            for (int i = 0; i < c.length(); ++i) c[i] = max(c[i], OX_NN_ACC_T(0.0));
#if defined(OX_NN_CM_FP16ACC)
        coopMatStore(c, oxNnAct, dst + n * 16u, sOut, gl_CooperativeMatrixLayoutRowMajor);
#else
        coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator> h =
            coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>(c);
        coopMatStore(h, oxNnAct, dst + n * 16u, sOut, gl_CooperativeMatrixLayoutRowMajor);
#endif
    }
    subgroupMemoryBarrierShared();
    subgroupBarrier();
}

#endif
