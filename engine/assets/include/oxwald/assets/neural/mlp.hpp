#pragma once

// Small multilayer perceptrons on the CPU: reference inference, training (backprop + Adam) and FP16 packing for the
// GPU inference path (render/neural, engine/shaders/render/neural/mlp.glsl). Used by neural texture compression
// (neural/ntc.hpp) and the neural denoiser. Everything here is deterministic: a fixed seed gives bit-identical
// parameters on every machine, independent of the number of worker threads.
//
//   ox::nn::Mlp net = ox::nn::Mlp::make(32, {64, 64}, 16);       // 32 -> 64 -> 64 -> 16, ReLU hidden, linear out
//   net.initialize(seed);
//   ox::nn::TrainOptions o; o.steps = 2000;
//   ox::nn::trainRegression(net, inputs, targets, count, o);      // MSE + Adam
//   net.roundToHalf();                                            // what the GPU will see
//   ox::nn::PackedNetwork gpu = ox::nn::packFp16(net);            // padded FP16 rows for render::NeuralNetwork

#include <oxwald/core/types.hpp>

#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ox::nn {

// IEEE half conversion (round to nearest even, saturating to ±65504; NaN preserved).
[[nodiscard]] u16 floatToHalf(f32 v);
[[nodiscard]] f32 halfToFloat(u16 h);
[[nodiscard]] inline f32 roundToHalf(f32 v) { return halfToFloat(floatToHalf(v)); }

enum class Activation : u8 { None = 0, ReLU = 1 };

struct LayerDesc {
    u32 inputs = 0;
    u32 outputs = 0;
    Activation activation = Activation::ReLU;
};

// PCG32 (O'Neill): small, fast, reproducible.
class Pcg32 {
public:
    explicit Pcg32(u64 seed = 0x853c49e6748fea9bull, u64 stream = 0xda3e39cb94b95bdbull);
    u32 next();
    f32 uniform() { return f32(next() >> 8) * (1.0f / 16777216.0f); } // [0, 1)
    f32 uniform(f32 lo, f32 hi) { return lo + (hi - lo) * uniform(); }
    u32 below(u32 bound); // [0, bound)

private:
    u64 m_state = 0;
    u64 m_inc = 0;
};

// Fully connected network. Parameters per layer: W (outputs × inputs, row-major) followed by b (outputs).
class Mlp {
public:
    Mlp() = default;
    explicit Mlp(std::vector<LayerDesc> layers);
    // inputs -> hidden[0] -> ... -> outputs; hidden layers use `hiddenActivation`, the last layer `outputActivation`.
    [[nodiscard]] static Mlp make(u32 inputs, std::initializer_list<u32> hidden, u32 outputs,
                                  Activation hiddenActivation = Activation::ReLU,
                                  Activation outputActivation = Activation::None);
    [[nodiscard]] static Mlp make(u32 inputs, std::span<const u32> hidden, u32 outputs,
                                  Activation hiddenActivation = Activation::ReLU,
                                  Activation outputActivation = Activation::None);

    [[nodiscard]] const std::vector<LayerDesc>& layers() const { return m_layers; }
    [[nodiscard]] u32 inputs() const { return m_layers.empty() ? 0 : m_layers.front().inputs; }
    [[nodiscard]] u32 outputs() const { return m_layers.empty() ? 0 : m_layers.back().outputs; }
    [[nodiscard]] u32 maxWidth() const { return m_maxWidth; }
    [[nodiscard]] usize paramCount() const { return m_params.size(); }
    [[nodiscard]] usize weightOffset(u32 layer) const { return m_offsets[layer]; }
    [[nodiscard]] usize biasOffset(u32 layer) const {
        return m_offsets[layer] + usize(m_layers[layer].inputs) * m_layers[layer].outputs;
    }
    [[nodiscard]] std::span<f32> params() { return m_params; }
    [[nodiscard]] std::span<const f32> params() const { return m_params; }
    // Multiply-adds of one forward evaluation.
    [[nodiscard]] u64 macsPerSample() const;

    // He-uniform weights, zero biases.
    void initialize(u64 seed);
    // Rounds every parameter to the nearest FP16 value (the GPU stores FP16 weights).
    void roundToHalf();

    // One sample. `scratch` needs scratchSize() floats.
    [[nodiscard]] usize scratchSize() const { return usize(m_maxWidth) * 2; }
    void forward(const f32* in, f32* out, f32* scratch) const;
    // `count` samples, row-major [count × inputs] -> [count × outputs].
    void forwardBatch(const f32* in, u32 count, f32* out) const;

    // Training support. Activations of every layer for `count` samples are recorded in `tape`, then backward()
    // accumulates dLoss/dParams into `grad` (paramCount() floats) and optionally writes dLoss/dInputs.
    struct Tape {
        u32 count = 0;
        std::vector<f32> values; // per layer: input activations [count × inputs], then final outputs
        std::vector<f32> delta;  // scratch
        std::vector<f32> delta2;
    };
    void forwardTrain(const f32* in, u32 count, Tape& tape, f32* out = nullptr) const;
    [[nodiscard]] const f32* tapeOutputs(const Tape& tape) const;
    void backward(Tape& tape, const f32* dOut, f32* grad, f32* dIn = nullptr) const;

    // Transposed weights (inputs × outputs per layer) used by forward(); rebuilt by syncTransposed() after the
    // parameters changed (initialize/roundToHalf/an optimiser step call it themselves).
    void syncTransposed();

    // Binary (de)serialisation: layer table + FP32 parameters.
    [[nodiscard]] std::vector<std::byte> serialize() const;
    [[nodiscard]] static bool deserialize(std::span<const std::byte> data, Mlp& out);

    bool operator==(const Mlp& o) const;

private:
    void layout();

    std::vector<LayerDesc> m_layers;
    std::vector<usize> m_offsets;
    std::vector<f32> m_params;
    std::vector<f32> m_transposed; // W^T per layer, same offsets as m_params (biases unused)
    u32 m_maxWidth = 0;
};

struct AdamParams {
    f32 beta1 = 0.9f;
    f32 beta2 = 0.999f;
    f32 epsilon = 1e-8f;
};

class WorkerPool;

class Adam {
public:
    void reset(usize count);
    // params -= lr * mHat / (sqrt(vHat) + eps), split over `pool` when given (element-wise, so deterministic).
    void step(std::span<f32> params, std::span<const f32> grads, f32 lr, const AdamParams& p = {},
              WorkerPool* pool = nullptr);
    [[nodiscard]] u32 steps() const { return m_t; }

private:
    std::vector<f32> m_m, m_v;
    u32 m_t = 0;
};

// Fixed partition parallel loop: work item i of `count` runs fn(i). A persistent pool of std::threads; the
// partitioning never depends on the thread count, so callers that reduce per-item results in item order get
// deterministic results.
class WorkerPool {
public:
    explicit WorkerPool(u32 threads = 0); // 0 = hardware concurrency
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    void run(u32 count, const std::function<void(u32)>& fn);
    [[nodiscard]] u32 threads() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

struct TrainOptions {
    u32 steps = 1000;
    u32 batch = 256;
    f32 learningRate = 1e-3f;
    f32 finalLearningRate = 1e-4f; // cosine decay to this value
    u64 seed = 1;
    u32 threads = 0;              // 0 = hardware concurrency (results do not depend on it)
    u32 lossEvery = 50;           // record the batch loss every n steps
};

struct TrainResult {
    f32 initialLoss = 0.0f; // MSE over the whole data set before training
    f32 finalLoss = 0.0f;   // after training
    std::vector<f32> history;
    f64 milliseconds = 0.0;
};

// Minimises the mean squared error on (inputs [count × net.inputs()], targets [count × net.outputs()]).
TrainResult trainRegression(Mlp& net, std::span<const f32> inputs, std::span<const f32> targets, u32 count,
                            const TrainOptions& options);
[[nodiscard]] f32 meanSquaredError(const Mlp& net, std::span<const f32> inputs, std::span<const f32> targets, u32 count);

// ---- GPU packing -----------------------------------------------------------------------------------------------
// Layout consumed by render::NeuralNetwork and mlp.glsl: per layer a row-major FP16 weight matrix
// [outputsPadded × inputsPadded] (zero padded) followed by FP16 biases [outputsPadded]. Padding is to multiples of
// 16 (cooperative matrix tiles; also satisfies the 16-byte stride / 64-byte offset rules of cooperative vectors:
// every block starts at a multiple of 32 halves).
struct PackedLayer {
    u32 inputs = 0, outputs = 0;
    u32 inputsPadded = 0, outputsPadded = 0;
    u32 weightOffset = 0; // in halves from the start of the blob
    u32 biasOffset = 0;   // in halves
    u32 activation = 0;   // Activation
    u32 pad = 0;
};
struct PackedNetwork {
    std::vector<PackedLayer> layers;
    std::vector<u16> halves;
    [[nodiscard]] usize bytes() const { return halves.size() * 2; }
};
[[nodiscard]] u32 padTo(u32 v, u32 multiple);
[[nodiscard]] PackedNetwork packFp16(const Mlp& net, u32 padding = 16);

} // namespace ox::nn
