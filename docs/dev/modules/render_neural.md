# render/neural — MLP inference, neural texture compression, neural denoiser

Code: `engine/render/src/neural/`, public header `<oxwald/render/neural/neural.hpp>` (namespace `ox::render::neural`),
shaders `engine/shaders/render/neural/`. CPU side (training, reference inference, NTC format and encoder) lives in
`assets` because the material importer needs it: `<oxwald/assets/neural/mlp.hpp>` (`ox::nn`) and
`<oxwald/assets/neural/ntc.hpp>`. User guide: `docs/guide/36-neural-rendering.md`. Measurements: `docs/dev/perf.md`
("Neural rendering").

## MLP infrastructure

| Piece | What |
| --- | --- |
| `nn::Mlp` | dense layers (ReLU / linear), forward, `forwardTrain` + `backward` (tape), `serialize`, `roundToHalf` |
| `nn::trainRegression`, `nn::Adam`, `nn::WorkerPool` | MSE training with Adam and cosine decay. Work is split in fixed chunks reduced in chunk order, so results are bit-identical for any thread count (tested 1 vs 4 / 6 threads). Workers run with FTZ/DAZ (denormal Adam moments made training ~5x slower) |
| `nn::packFp16` | GPU layout: per layer FP16 W [outPad × inPad] row-major + FP16 bias, padding to 16, blocks 64-byte aligned (cooperative matrix tiles; cooperative vector offset/stride rules) |
| `NeuralNetwork` | weights buffer (device local, BDA) + `PackedLayer` offsets |
| `NeuralInference` | `mlp_eval.comp`, 3-layer networks IN → H → H → OUT (IN/OUT 16..64, H 32/64), paths below; `evaluate(..., &gpuMs)` measures with frame timestamps after 4 warm-up frames |
| `mlp.glsl` | `OX_NN_LAYER_F32` (portable: weights read as packed `uint`, FP32 math), `OX_NN_LAYER_F16` (`f16vec2`, `OX_NN_FP16`), `OX_NN_LAYER_CV` (`coopvecNV`, `coopVecMatMulAddNV`, row-major FP16, `OX_NN_COOPVEC`) |
| `mlp_coopmat.glsl` | `VK_KHR_cooperative_matrix` 16×16×16: 4 subgroups per workgroup, 16 tiles of 16 samples per subgroup with private ping-pong activation buffers in shared memory (rows padded by 8 halves against bank conflicts), bias tiles staged once per workgroup, weight tiles loaded straight from the weights buffer (`OX_NN_CM_SHARED_W` stages them in shared memory instead — slower, see perf.md); FP16 accumulators when `cooperativeMatrixFp16Accumulate` (`OX_NN_CM_FP16ACC`) |

`DeviceCaps` additions (rhi, additive): `cooperativeMatrix` (a subgroup-scope 16×16×16 FP16→FP32 config exists),
`cooperativeMatrixFp16Accumulate`, `cooperativeMatrixStages`, `cooperativeVector` (FP16 input/matrix/bias/result
config), `cooperativeVectorTraining`, `cooperativeVectorStages`, `storageBuffer16BitAccess`,
`shaderBufferInt64Atomics`. `Device` enables `VK_KHR_cooperative_matrix` / `VK_NV_cooperative_vector` and their
feature structs when present. `rhi::formatInfo` knows `VK_FORMAT_B4G4R4A4_UNORM_PACK16`. Path selection:
`bestComputePath` (CoopVector > CoopMatrix > FP16 > FP32, the measured order on the RTX 3080), `bestFragmentPath` (CoopVector > FP16 except NVIDIA >
FP32); `r.NeuralTextures.ShaderPath` overrides the material shaders' path (read when a renderer creates its
pipelines, `materialShaderDefines`). glslang 16.2 compiles both extensions for the Vulkan 1.2 / SPIR-V 1.5 target;
cooperative vectors work through buffer references (no descriptors needed).

## Neural texture compression

Format (`.oxntc`, `AssetType::NeuralTextureSet = 14`): header, layer table + FP16 network (32 → H → H → 16, H = 32 /
64), latent pairs (G0 at 1/stride of mip 2j, G1 at half of G0; Q4 nibble-packed or Q8; [layer][y][x][4]), four BC
fallback `.oxtex` blobs (albedo BC7 sRGB, normal BC5, ORM BC7, emissive BC7 sRGB) for mips ≤ `fallbackSize`, stats
JSON, CRC32. Network input (fixed positions, `kNtc*` constants): bilinear G0 [0,16), G1 [16,24), sin/cos of the
position inside the G0 cell [24,28), mip parity 28. Decoding is defined at texel centres of each mip; G0 sample
positions are multiples of 1/8 texel, so hardware bilinear filtering equals the CPU reference.

Encoder (`encodeNeuralTextures`): sources resized to a common size, Kaiser mip chains (sRGB-correct, renormalised
normals), level sampling ∝ texels (≥ 1/8 of mip 0's share), MSE on all channels, Adam (latents 10× the network
rate), uniform quantisation noise on the interpolated latents for 85 % of the steps, then latents quantised and
frozen while the network fine-tunes, FP16 rounding at the end. Latent gradients are scattered in sample order
(deterministic). `evaluateNeuralTextures` reports PSNR / SSIM (8×8 windows) per channel against the sources and
the same numbers for BC7 (UASTC level 1, as the importer) / BC5.

Importer: `MaterialImportSettings` (`neuralCompression` Off / On / Replace, preset or custom parameters). The
material importer reads the textures' source images through new `ImportContext` callbacks (`sourceOf`,
`readArtifact`; wired by `AssetRegistry`, unavailable in `importStandalone`), emits `<material>#NeuralTextures` and
sets `MaterialAsset::neuralTextures` / `neuralTexturesOnly` (`textureDependencies()` then lists only the set).
`oximport --ntc` (loose images) and `oximport <file> --project <dir>` (inside a project, metas untouched).

Runtime (`neural_textures.cpp`, feature `NeuralTextures`, PreDepth -20000, always enabled; registered by
`neural::registerNeuralFeatures(renderer)`, which also installs the resolve hook so materials are never resolved
without it):

* `GpuResourceCache::IMaterialResolveHook` (additive in the cache): materials with `neuralTextures` are resolved by
  the feature; the cache does not request their regular textures unless the hook declines (Off with textures kept).
* Sets load through `AssetProvider::loadNeuralTextures` (new optional member, filled by `makeAssetManagerProvider`)
  or `addNeuralTextureSet`. On load the fallback tails are registered as cache textures (derived UUIDs) — streamed by
  the texture streamer and used while decoding.
* DecodeOnLoad (and Off for `neuralTexturesOnly`): `decodeToTextures` on the JobSystem (CPU network + BC7 / BC5),
  results registered as cache textures (streamed). `NtcGpuDecoder` (`ntc_decode.comp`, every path) decodes on the
  GPU for tools / benchmarks.
* InferenceOnSample: latents as 2D arrays (`B4G4R4A4_UNORM_PACK16` when sampleable + linear-filterable, else RGBA8),
  weights buffer, slot in the `GpuNeuralMaterial` table (128 B, uploaded to frame memory per view,
  `GpuViewConstants::neuralMaterials`, formerly `shPad0/shPad1`). `GpuMaterial.flags`: `kMaterialNeural` (bit 6) +
  slot in bits 8..31; texture indices point at the fallback tail.
* Latent streaming: wanted first pair from the instances' projected size × UV tiling (same heuristic as the texture
  streamer), finer pairs dropped after 30 frames, `GpuNeuralMaterial::minMip` clamps the shader.
* Mode changes release the other mode's resources and refresh every material (test: no texture / buffer growth over
  three rounds of mode switches).

Shaders: `forward.frag` and `depth_prepass.frag` call `oxSampleMaterialView` (`ntc.glsl`); other material consumers
are unchanged and see the fallback. The renderer keeps two sets of prepass / forward pipelines
(`BuiltinPipelines::prepass/forward` with `OX_NTC_DISABLED`, `prepassNeural/forwardNeural` with the decoder and the
`materialShaderDefines` MLP path, created on the first frame with `r.NeuralTextures InferenceOnSample`) and picks the
set per view in `buildView`. Compiling the decoder into the shaders slowed regular materials even with the mode Off
(RTX 3080, material station at 4K: forward 2.3 ms with the cooperative-vector variant, 0.7 ms without). The
gpu-driven meshlet pipelines (`gpu_driven.cpp`) still compile `forward.frag` / `depth_prepass.frag` with the default
(FP32) decoder.

## Neural denoiser

`r.Denoiser Neural` (value 3, added to the postprocess-owned cvar and `pp::DenoiserType`; Ray Reconstruction
selection treats it like SVGF) switches `rt::SvgfDenoiser`'s À-trous pipelines to `denoise_neural.comp` (pass names
`<effect>.NeuralAtrous<i>`); temporal accumulation, variance and upsampling are unchanged. Per tap 6 features → 16 →
16 → 1 logit, weight = B3 × exp(logit); one MLP per iteration, constants in the generated
`denoiser_weights.glsl`. Training tool: `NeuralDenoiserTest.TrainWeights` (`OX_TRAIN_NEURAL_DENOISER=1`, ~30 s on
the RTX 3080 / i9-9900K; path tracer at 1/2/4/16 spp vs 1536 spp + synthetic 1- and 8-spp AO vs 256 spp; greedy
per-iteration training with a relative L2 loss; CPU replica of both kernels for held-out PSNR, measured on
x / (1 + x) after scaling the reference to a mean luminance of 0.18 — the path traced frames are in physical units).
`denoise_neural.comp` generates one tap-MLP function per iteration so every weight index is a compile-time constant
(a runtime iteration index put the weight array in local memory: 35.7 → 7.2 ms at 1080p).

**Status: experimental.** Shipped weights (2026-10-14): held-out path traced frames noisy 31.9 dB, SVGF (spatial
replica) 40.1 dB, neural 40.5 dB; on the GPU AO probe see perf.md — better than SVGF on frame 1, worse when
converged. `r.Denoiser` describes it as experimental and the settings menus do not offer it.

## Tests

`ox_assets_tests` `NeuralNet.*` (half conversion, forward, training reduces loss + determinism across thread counts +
serialisation, FP16 packing) and `NeuralTextures.*` (round-trip quality, determinism + `.oxntc` round trip +
corruption, DecodeOnLoad BC validity, presets, material importer sub-asset + AssetManager loading; `Report` opt-in
with `OX_NTC_REPORT`). `ox_render_gpu_tests` (label gpu): `NeuralGpuTest.*` (caps, scalar / FP16 / cooperative
matrix / cooperative vector vs the CPU reference, throughput with `OX_NEURAL_PERF`), `NeuralTexturesTest.*`
(InferenceOnSample vs regular textures PSNR — with `r.VRS` pinned off, content-adaptive VRS coarsened the plane
differently per mode: 25.7 dB instead of 33.7 dB for DecodeOnLoad —, DecodeOnLoad, fallback-only lower bound, mode switching without leaks,
latent residency, GPU decode = CPU decode on every path, `PerfReportMaterialStation` with `OX_NTC_PERF`),
`NeuralDenoiserTest.*` (SVGF vs neural RMSE/PSNR and 1080p cost; training tool). `oximport.ntc` (ctest, tools).

## Limits / TODO

* Training is CPU only (no GPU trainer); 2048² sets take minutes.
* The KHR cooperative-matrix kernel was tuned from 65 ms to 8.7 ms (8.3M samples, hidden 64; padded shared rows,
  weights read from the buffer, 1024 samples per workgroup) but stays ~2x slower than the driver's cooperative-vector
  kernels (4.6 ms), so it is only chosen without `VK_NV_cooperative_vector`. Not tried: double-buffered input loads,
  keeping the A tiles in registers across output tiles.
* No BC7-compressed latents; no per-material channel weighting; only `.oxmat` materials can be compressed by the
  importer (not materials embedded in models).
* InferenceOnSample affects forward + prepass only; translucency, captures, RT hits and shadow alpha tests use the
  fallback tail. The prepass alpha test uses the fallback alpha.
* Latent residency is not counted in `r.Streaming.PoolSizeMB`.
* The neural denoiser is **experimental** (cvar description; not offered in the settings menus): on the AO probe it
  beats SVGF on a fresh history (frame 1) but is blurrier once the temporal history has converged, and its À-trous
  stage costs ~3x SVGF's (perf.md). It is trained on spatial CPU replicas of the À-trous stage (1/2/4/16 spp path
  traced frames, 1/8 spp AO); the GPU pipeline's variance comes from temporal moments, which the training does not
  model — the likely next step is training on frames captured after the temporal pass. Not verified on MoltenVK / Android hardware (scalar paths only compile-tested there).
