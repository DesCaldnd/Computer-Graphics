#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/render/neural/neural.hpp>
#include <oxwald/rhi/command_list.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/pipeline.hpp>

#include <cstddef>
#include <cstring>
#include <format>

namespace ox::render::neural {

namespace {

CVar<int> cvShaderPath("r.NeuralTextures.ShaderPath", 0,
                       "MLP path of the material shaders for InferenceOnSample (read when a renderer first creates "
                       "its neural material pipelines): Auto, Scalar (FP32), FP16, CoopVector",
                       CVarEnum{"Auto", "Scalar", "FP16", "CoopVector"});

bool validWidth(u32 v) { return v >= 16 && v <= 64 && v % 16 == 0; }

} // namespace

const char* neuralPathName(NeuralPath p) {
    switch (p) {
    case NeuralPath::ScalarF32: return "ScalarF32";
    case NeuralPath::ScalarF16: return "ScalarF16";
    case NeuralPath::CoopMatrix: return "CoopMatrix";
    case NeuralPath::CoopVector: return "CoopVector";
    }
    return "?";
}

bool neuralPathSupported(const rhi::DeviceCaps& caps, NeuralPath p, bool fragment) {
    switch (p) {
    case NeuralPath::ScalarF32: return true;
    case NeuralPath::ScalarF16: return caps.shaderFloat16;
    case NeuralPath::CoopMatrix:
        return !fragment && caps.cooperativeMatrix && (caps.cooperativeMatrixStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
               caps.subgroupSize >= 16 && caps.subgroupSize <= 64;
    case NeuralPath::CoopVector:
        return caps.cooperativeVector &&
               (caps.cooperativeVectorStages & (fragment ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT));
    }
    return false;
}

NeuralPath bestComputePath(const rhi::DeviceCaps& caps) {
    // RTX 3080, 8.3M samples 32 -> 64 -> 64 -> 16: CoopVector 4.6 ms, CoopMatrix 8.7 ms, FP16 18 ms, FP32 29 ms
    // (docs/dev/perf.md, "Neural rendering"): the driver's cooperative-vector kernels beat the KHR matrix tiles.
    for (NeuralPath p : {NeuralPath::CoopVector, NeuralPath::CoopMatrix, NeuralPath::ScalarF16})
        if (neuralPathSupported(caps, p)) return p;
    return NeuralPath::ScalarF32;
}

NeuralPath bestFragmentPath(const rhi::DeviceCaps& caps) {
    if (neuralPathSupported(caps, NeuralPath::CoopVector, true)) return NeuralPath::CoopVector;
    // Packed FP16 is not faster than FP32 on NVIDIA Ampere/Ada (same FMA rate), but halves the registers; keep FP32
    // there for accuracy and use FP16 on the GPUs with double-rate half math.
    if (caps.shaderFloat16 && caps.vendor != rhi::GpuVendor::Nvidia) return NeuralPath::ScalarF16;
    return NeuralPath::ScalarF32;
}

std::string neuralCapsString(const rhi::DeviceCaps& caps) {
    return std::format("fp16 {}, cooperative matrix {} (fp16 accumulate {}), cooperative vector {} (fragment {}), "
                       "subgroup {}; compute path {}, fragment path {}",
                       caps.shaderFloat16 ? "yes" : "no", caps.cooperativeMatrix ? "yes" : "no",
                       caps.cooperativeMatrixFp16Accumulate ? "yes" : "no", caps.cooperativeVector ? "yes" : "no",
                       (caps.cooperativeVectorStages & VK_SHADER_STAGE_FRAGMENT_BIT) ? "yes" : "no", caps.subgroupSize,
                       neuralPathName(bestComputePath(caps)), neuralPathName(bestFragmentPath(caps)));
}

NeuralPath materialShaderPath(const rhi::DeviceCaps& caps) {
    switch (cvShaderPath.get()) {
    case 1: return NeuralPath::ScalarF32;
    case 2: return caps.shaderFloat16 ? NeuralPath::ScalarF16 : NeuralPath::ScalarF32;
    case 3: return neuralPathSupported(caps, NeuralPath::CoopVector, true) ? NeuralPath::CoopVector : bestFragmentPath(caps);
    default: return bestFragmentPath(caps);
    }
}

std::vector<rhi::ShaderDefine> materialShaderDefines(const rhi::DeviceCaps& caps, bool neural) {
    if (!neural) return {{"OX_NTC_DISABLED"}};
    switch (materialShaderPath(caps)) {
    case NeuralPath::ScalarF16: return {{"OX_NN_FP16"}};
    case NeuralPath::CoopVector: return {{"OX_NN_COOPVEC"}};
    default: return {};
    }
}

// ---- NeuralNetwork -------------------------------------------------------------------------------------------

NeuralNetwork::NeuralNetwork(NeuralNetwork&& o) noexcept { *this = std::move(o); }

NeuralNetwork& NeuralNetwork::operator=(NeuralNetwork&& o) noexcept {
    if (this != &o) {
        release();
        m_device = o.m_device;
        m_buffer = o.m_buffer;
        m_address = o.m_address;
        m_bytes = o.m_bytes;
        m_layers = std::move(o.m_layers);
        o.m_device = nullptr;
        o.m_buffer = {};
        o.m_address = 0;
        o.m_bytes = 0;
    }
    return *this;
}

NeuralNetwork::~NeuralNetwork() { release(); }

bool NeuralNetwork::upload(rhi::Device& device, const nn::Mlp& network, std::string_view name) {
    release();
    const nn::PackedNetwork packed = nn::packFp16(network);
    if (packed.halves.empty()) return false;
    rhi::BufferDesc d;
    d.size = packed.bytes();
    d.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
    d.memory = rhi::MemoryUsage::GpuOnly;
    d.name = std::string(name);
    m_buffer = device.createBuffer(d, packed.halves.data());
    if (!m_buffer) return false;
    m_device = &device;
    m_address = device.address(m_buffer);
    m_bytes = d.size;
    m_layers = packed.layers;
    return true;
}

void NeuralNetwork::release() {
    if (m_device && m_buffer) m_device->destroy(m_buffer);
    m_device = nullptr;
    m_buffer = {};
    m_address = 0;
    m_bytes = 0;
    m_layers.clear();
}

// ---- NeuralInference -----------------------------------------------------------------------------------------

namespace {
struct EvalPush {
    VkDeviceAddress weights, inputs, outputs;
    u32 count;
    u32 w0, b0, w1, b1, w2, b2;
};
constexpr u32 kEvalPushSize = 52; // scalar layout in mlp_eval.comp (no tail padding)
static_assert(offsetof(EvalPush, b2) + 4 == kEvalPushSize);
} // namespace

NeuralInference::NeuralInference(rhi::Device& device) : m_device(&device) {}

NeuralInference::~NeuralInference() {
    for (auto& [key, p] : m_pipelines)
        if (p) m_device->destroy(p);
}

rhi::PipelineHandle NeuralInference::pipeline(const NeuralNetwork& net, NeuralPath path) {
    const auto& l = net.layers();
    if (l.size() != 3) return {};
    const u32 in = l[0].inputsPadded, hidden = l[0].outputsPadded, out = l[2].outputsPadded;
    if (!validWidth(in) || !validWidth(out) || (hidden != 32 && hidden != 64) || l[1].inputsPadded != hidden ||
        l[1].outputsPadded != hidden || l[2].inputsPadded != hidden)
        return {};
    if (!neuralPathSupported(m_device->caps(), path)) return {};
    const auto key = std::make_tuple(u32(path), in, hidden, out);
    if (auto it = m_pipelines.find(key); it != m_pipelines.end()) return it->second;
    std::vector<rhi::ShaderDefine> defs = {{"OX_NN_IN", std::to_string(in)},
                                           {"OX_NN_HIDDEN", std::to_string(hidden)},
                                           {"OX_NN_OUT", std::to_string(out)}};
    if (path == NeuralPath::ScalarF16) defs.push_back({"OX_NN_FP16"});
    if (path == NeuralPath::CoopVector) defs.push_back({"OX_NN_COOPVEC"});
    if (path == NeuralPath::CoopMatrix) {
        defs.push_back({"OX_NN_COOPMAT"});
        defs.push_back({"OX_NN_SUBGROUP", std::to_string(m_device->caps().subgroupSize)});
        if (m_device->caps().cooperativeMatrixFp16Accumulate) defs.push_back({"OX_NN_CM_FP16ACC"});
    }
    rhi::ComputePipelineDesc d;
    d.name = std::format("nn.eval.{}.{}x{}x{}", neuralPathName(path), in, hidden, out);
    d.shader = rhi::ShaderStageDesc::file("render/neural/mlp_eval.comp", defs);
    const rhi::PipelineHandle p = m_device->createComputePipeline(d);
    m_pipelines[key] = p;
    return p;
}

bool NeuralInference::record(rhi::CommandList& cmd, const NeuralNetwork& net, VkDeviceAddress inputs,
                             VkDeviceAddress outputs, u32 count, NeuralPath path) {
    const rhi::PipelineHandle p = pipeline(net, path);
    if (!p || !net.valid()) return false;
    const auto& l = net.layers();
    EvalPush pc{net.address(), inputs, outputs, count, l[0].weightOffset, l[0].biasOffset, l[1].weightOffset,
                l[1].biasOffset, l[2].weightOffset, l[2].biasOffset};
    cmd.bindPipeline(p);
    cmd.pushConstants(&pc, kEvalPushSize);
    if (path == NeuralPath::CoopMatrix) cmd.dispatch((count + 1023) / 1024); // 4 subgroups x 16 tiles x 16
    else cmd.dispatchThreads(count);
    return true;
}

bool NeuralInference::evaluate(const NeuralNetwork& net, std::span<const f32> inputs, u32 count, std::vector<f32>& outputs,
                               NeuralPath path, f64* gpuMs, u32 repeat) {
    if (!pipeline(net, path) || net.layers().size() != 3) return false;
    rhi::Device& dev = *m_device;
    const u32 in = net.layers()[0].inputsPadded, out = net.layers()[2].outputsPadded;
    if (inputs.size() < usize(count) * in) return false;
    rhi::BufferDesc bd;
    bd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst | rhi::BufferUsage::TransferSrc;
    bd.size = u64(count) * in * sizeof(f32);
    bd.name = "nn.eval.inputs";
    const rhi::BufferHandle bin = dev.createBuffer(bd, inputs.data());
    bd.size = u64(count) * out * sizeof(f32);
    bd.name = "nn.eval.outputs";
    const rhi::BufferHandle bout = dev.createBuffer(bd);
    const VkDeviceAddress ain = dev.address(bin), aout = dev.address(bout);
    bool ok = true;
    if (gpuMs) {
        // Frame command lists carry timestamp queries; read the retired frames' timings.
        f64 total = 0.0;
        u32 samples = 0;
        // Warm-up frames (clocks, first-use costs) are not counted.
        constexpr u32 kWarmup = 4;
        const u32 frames = kWarmup + std::max(repeat, 1u) + dev.framesInFlight() + 1;
        for (u32 f = 0; f < frames; ++f) {
            dev.beginFrame();
            for (const rhi::GpuTiming& t : dev.gpuTimings())
                if (t.name == "nn.eval" && f >= kWarmup + dev.framesInFlight()) total += t.milliseconds, ++samples;
            if (f < kWarmup + std::max(repeat, 1u)) {
                rhi::CommandList& cmd = dev.commandList(rhi::QueueType::Graphics, "nn.eval");
                cmd.beginTimestamp("nn.eval");
                ok &= record(cmd, net, ain, aout, count, path);
                cmd.endTimestamp();
                dev.submit(cmd);
            }
            dev.endFrame();
        }
        dev.waitIdle();
        *gpuMs = samples ? total / samples : 0.0;
    } else {
        for (u32 r = 0; r < std::max(repeat, 1u); ++r) {
            dev.immediateSubmit([&](rhi::CommandList& cmd) { ok &= record(cmd, net, ain, aout, count, path); });
        }
    }
    const std::vector<u8> bytes = dev.readBuffer(bout);
    outputs.resize(usize(count) * out);
    std::memcpy(outputs.data(), bytes.data(), std::min(bytes.size(), outputs.size() * sizeof(f32)));
    dev.destroy(bin);
    dev.destroy(bout);
    return ok;
}

} // namespace ox::render::neural
