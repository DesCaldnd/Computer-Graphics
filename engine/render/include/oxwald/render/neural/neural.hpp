#pragma once

// Neural rendering infrastructure (render/neural): small MLPs evaluated on the GPU, neural texture compression at
// run time and the neural denoiser. CPU reference / training: <oxwald/assets/neural/mlp.hpp>, NTC encoding:
// <oxwald/assets/neural/ntc.hpp>. Shaders: engine/shaders/render/neural/ (mlp.glsl, mlp_coopmat.glsl, ntc.glsl).
//
//   ox::nn::Mlp net = ...;                                   // trained on the CPU (or loaded)
//   ox::render::neural::NeuralNetwork gpu;
//   gpu.upload(device, net);                                 // FP16 weights, packFp16 layout
//   ox::render::neural::NeuralInference inference(device);
//   inference.evaluate(gpu, inputs, count, outputs, neural::bestComputePath(device.caps()));
//
// Paths: ScalarF32 (every device), ScalarF16 (shaderFloat16), CoopMatrix (VK_KHR_cooperative_matrix, compute),
// CoopVector (VK_NV_cooperative_vector, compute + fragment).

#include <oxwald/assets/neural/mlp.hpp>
#include <oxwald/assets/neural/ntc.hpp>
#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/rhi/device_caps.hpp>
#include <oxwald/rhi/handles.hpp>
#include <oxwald/rhi/shader_compiler.hpp>
#include <oxwald/rhi/vulkan.hpp>

#include <array>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace ox::rhi {
class Device;
class CommandList;
} // namespace ox::rhi

namespace ox::assets {
struct NeuralTextureSet;
}

namespace ox::render {
class Renderer;
}

namespace ox::render::neural {

enum class NeuralPath : u8 { ScalarF32, ScalarF16, CoopMatrix, CoopVector };
inline constexpr u32 kNeuralPathCount = 4;
[[nodiscard]] const char* neuralPathName(NeuralPath path);
// Whether the device can run `path` in compute shaders (fragment: only ScalarF32 / ScalarF16 / CoopVector).
[[nodiscard]] bool neuralPathSupported(const rhi::DeviceCaps& caps, NeuralPath path, bool fragment = false);
[[nodiscard]] NeuralPath bestComputePath(const rhi::DeviceCaps& caps);
[[nodiscard]] NeuralPath bestFragmentPath(const rhi::DeviceCaps& caps);
// Human-readable capability summary ("cooperative matrix: yes (16x16x16 fp16) ...").
[[nodiscard]] std::string neuralCapsString(const rhi::DeviceCaps& caps);

// Network weights on the GPU (ox::nn::packFp16 layout, device-local buffer, BDA).
class NeuralNetwork {
public:
    NeuralNetwork() = default;
    NeuralNetwork(const NeuralNetwork&) = delete;
    NeuralNetwork& operator=(const NeuralNetwork&) = delete;
    NeuralNetwork(NeuralNetwork&& o) noexcept;
    NeuralNetwork& operator=(NeuralNetwork&& o) noexcept;
    ~NeuralNetwork();

    bool upload(rhi::Device& device, const nn::Mlp& network, std::string_view name = "nn.weights");
    void release();

    [[nodiscard]] bool valid() const { return m_device != nullptr && m_address != 0; }
    [[nodiscard]] VkDeviceAddress address() const { return m_address; }
    [[nodiscard]] u64 bytes() const { return m_bytes; }
    [[nodiscard]] const std::vector<nn::PackedLayer>& layers() const { return m_layers; }
    [[nodiscard]] rhi::BufferHandle buffer() const { return m_buffer; }

private:
    rhi::Device* m_device = nullptr;
    rhi::BufferHandle m_buffer;
    VkDeviceAddress m_address = 0;
    u64 m_bytes = 0;
    std::vector<nn::PackedLayer> m_layers;
};

// Batch evaluation of 3-layer networks IN -> H -> H -> OUT (padded sizes: IN 16/32/48/64, H 32/64, OUT 16/32/48/64)
// in compute shaders (render/neural/mlp_eval.comp): tests, benchmarks, tools.
class NeuralInference {
public:
    explicit NeuralInference(rhi::Device& device);
    ~NeuralInference();
    NeuralInference(const NeuralInference&) = delete;
    NeuralInference& operator=(const NeuralInference&) = delete;

    // inputs: FP32 [count x inputsPadded], outputs: FP32 [count x outputsPadded] (device addresses).
    bool record(rhi::CommandList& cmd, const NeuralNetwork& net, VkDeviceAddress inputs, VkDeviceAddress outputs,
                u32 count, NeuralPath path);
    // Uploads, runs `repeat` times (immediate submits), reads back. gpuMs: average GPU time of one evaluation
    // (timestamp queries), when requested and supported. False when the path is unsupported or the shape invalid.
    bool evaluate(const NeuralNetwork& net, std::span<const f32> inputs, u32 count, std::vector<f32>& outputs,
                  NeuralPath path, f64* gpuMs = nullptr, u32 repeat = 1);

private:
    rhi::PipelineHandle pipeline(const NeuralNetwork& net, NeuralPath path);
    rhi::Device* m_device;
    std::map<std::tuple<u32, u32, u32, u32>, rhi::PipelineHandle> m_pipelines;
};

// Decodes the neural mips of a NeuralTextureSet on the GPU (render/neural/ntc_decode.comp) into RGBA8 images:
// DecodeOnLoad with r.NeuralTextures.GpuDecode, tools and benchmarks. Synchronous (submit + readback).
class NtcGpuDecoder {
public:
    explicit NtcGpuDecoder(rhi::Device& device);
    ~NtcGpuDecoder();
    NtcGpuDecoder(const NtcGpuDecoder&) = delete;
    NtcGpuDecoder& operator=(const NtcGpuDecoder&) = delete;

    struct Result {
        bool ok = false;
        std::vector<std::array<assets::Image, 4>> mips; // per neural mip: albedo, normal, ORM, emissive (empty = absent)
        f64 gpuMs = 0.0; // decode of all neural mips (timestamps; with `timed`)
    };
    // `timed`: runs `repeat` frames with timestamp queries and reports the average GPU time.
    Result decode(const assets::NeuralTextureSet& set, NeuralPath path, bool timed = false, u32 repeat = 1,
                  bool readback = true);

private:
    rhi::PipelineHandle pipeline(NeuralPath path);
    rhi::Device* m_device;
    std::map<u32, rhi::PipelineHandle> m_pipelines;
};

// Shader defines selecting the MLP path of the material shaders (forward / prepass): OX_NN_FP16 or OX_NN_COOPVEC
// (r.NeuralTextures.ShaderPath, read when the renderer creates its neural material pipelines). neural = false:
// OX_NTC_DISABLED, the variant without the decoder that the renderer uses unless r.NeuralTextures is
// InferenceOnSample (the decoder's registers slowed regular materials: forward 0.9 -> 2.3 ms at 4K on an RTX 3080).
[[nodiscard]] std::vector<rhi::ShaderDefine> materialShaderDefines(const rhi::DeviceCaps& caps, bool neural = true);
[[nodiscard]] NeuralPath materialShaderPath(const rhi::DeviceCaps& caps);

// --- neural texture compression at run time ------------------------------------------------------------------
// r.NeuralTextures: Off = regular textures (materials whose textures were replaced by the neural set are decoded
// as with DecodeOnLoad), DecodeOnLoad = the neural set is decoded into BC7 / BC5 textures when the material loads
// (disk / download savings; GPU memory and sampling as BC), InferenceOnSample = the material shaders run the
// decoder per sample (stochastic texture filtering, latents + weights in GPU memory; low-detail mips and
// non-inference consumers use the BC fallback tail).
enum class NeuralTextureMode : i32 { Off = 0, DecodeOnLoad = 1, InferenceOnSample = 2 };
[[nodiscard]] NeuralTextureMode neuralTextureMode();
[[nodiscard]] const char* neuralTextureModeName(NeuralTextureMode mode);

// Statistics of the neural material manager (RenderStats has no slot for them: tests and the debug UI read these).
struct NeuralTextureStats {
    u32 materials = 0;          // materials with a neural texture set
    u32 inferenceMaterials = 0; // resident for InferenceOnSample
    u32 decodedMaterials = 0;   // decoded into BC textures
    u32 pending = 0;            // loads / decodes in flight
    u64 latentBytes = 0;        // GPU memory of resident latents
    u64 weightBytes = 0;
    u64 fallbackBytes = 0;      // BC tails uploaded for inference
    u64 decodedBytes = 0;       // BC textures produced by DecodeOnLoad
    u32 latentPairsResident = 0;
    u32 latentPairsTotal = 0;
    f64 lastDecodeMs = 0.0;     // CPU time of the last decode (network + BC encode)
};

// Neural denoiser: r.Denoiser Neural selects the learned À-trous kernel inside rt::SvgfDenoiser (temporal and
// variance passes unchanged). Weights: engine/shaders/render/neural/denoiser_weights.glsl, trained by the opt-in
// test NeuralDenoiserTest.TrainWeights on path traced references.
[[nodiscard]] bool neuralDenoiserSelected();

// Registers the "NeuralTextures" feature (always enabled: it resolves neural materials in every mode) and installs
// its material hook in the renderer's resource cache. Called by registerBuiltinFeatures().
void registerNeuralFeatures(Renderer& renderer);
[[nodiscard]] NeuralTextureStats neuralTextureStats(Renderer& renderer);
// Registers a neural texture set directly (tests, tools, procedural content) instead of loading it through the
// AssetProvider (AssetProvider::loadNeuralTextures). Materials reference it with MaterialAsset::neuralTextures.
void addNeuralTextureSet(Renderer& renderer, const Uuid& id, std::shared_ptr<const assets::NeuralTextureSet> set);
// Waits for pending neural texture loads / decodes and applies them (tests, loading screens). Render thread.
void flushNeuralTextures(Renderer& renderer);

} // namespace ox::render::neural
