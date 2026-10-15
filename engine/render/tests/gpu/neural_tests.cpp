// Neural rendering on the GPU: MLP inference paths against the CPU reference (render/neural, assets/neural).
#include "render_fixture.hpp"

#include <oxwald/assets/neural/mlp.hpp>
#include <oxwald/render/neural/neural.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;
using namespace ox::render::neural;

namespace {

class NeuralGpuTest : public RenderTest {
protected:
    nn::Mlp network(u32 in, u32 hidden, u32 out, u64 seed) {
        nn::Mlp net = nn::Mlp::make(in, {hidden, hidden}, out);
        net.initialize(seed);
        // Non-zero biases so their path is exercised.
        nn::Pcg32 rng(seed + 1);
        for (u32 l = 0; l < 3; ++l)
            for (u32 o = 0; o < net.layers()[l].outputs; ++o) net.params()[net.biasOffset(l) + o] = rng.uniform(-0.2f, 0.2f);
        net.roundToHalf();
        return net;
    }
    std::vector<f32> inputs(u32 count, u32 in, u64 seed) {
        std::vector<f32> x(usize(count) * in);
        nn::Pcg32 rng(seed);
        for (f32& v : x) v = nn::roundToHalf(rng.uniform(-1.0f, 1.0f));
        return x;
    }
    // max |gpu - cpu| relative to max(1, |cpu|)
    f32 maxError(const nn::Mlp& net, const std::vector<f32>& x, const std::vector<f32>& gpu, u32 count) {
        std::vector<f32> cpu(usize(count) * net.outputs());
        net.forwardBatch(x.data(), count, cpu.data());
        f32 err = 0.0f;
        for (usize i = 0; i < cpu.size(); ++i) err = std::max(err, std::abs(gpu[i] - cpu[i]) / std::max(1.0f, std::abs(cpu[i])));
        return err;
    }
};

} // namespace

TEST_F(NeuralGpuTest, CapsAndPaths) {
    const rhi::DeviceCaps& caps = device->caps();
    std::printf("  %s\n  %s\n", caps.gpuName.c_str(), neuralCapsString(caps).c_str());
    EXPECT_TRUE(neuralPathSupported(caps, NeuralPath::ScalarF32));
    EXPECT_TRUE(neuralPathSupported(caps, NeuralPath::ScalarF32, true));
    EXPECT_FALSE(neuralPathSupported(caps, NeuralPath::CoopMatrix, true)); // compute only
    EXPECT_TRUE(neuralPathSupported(caps, bestComputePath(caps)));
    // Measured order (perf.md): cooperative vector > cooperative matrix > FP16 > FP32.
    if (neuralPathSupported(caps, NeuralPath::CoopVector)) EXPECT_EQ(bestComputePath(caps), NeuralPath::CoopVector);
    else if (neuralPathSupported(caps, NeuralPath::CoopMatrix)) EXPECT_EQ(bestComputePath(caps), NeuralPath::CoopMatrix);
    EXPECT_TRUE(neuralPathSupported(caps, bestFragmentPath(caps), true));
}

TEST_F(NeuralGpuTest, ScalarInferenceMatchesCpuReference) {
    const nn::Mlp net = network(32, 64, 16, 11);
    NeuralNetwork gpu;
    ASSERT_TRUE(gpu.upload(*device, net));
    NeuralInference inference(*device);
    const u32 count = 1000;
    const auto x = inputs(count, 32, 5);
    std::vector<f32> y;
    ASSERT_TRUE(inference.evaluate(gpu, x, count, y, NeuralPath::ScalarF32));
    EXPECT_LT(maxError(net, x, y, count), 1e-4f);
    // Hidden width 32 and a narrower output.
    const nn::Mlp small = network(16, 32, 3, 12);
    NeuralNetwork gpuSmall;
    ASSERT_TRUE(gpuSmall.upload(*device, small));
    const auto xs = inputs(count, 16, 6);
    ASSERT_TRUE(inference.evaluate(gpuSmall, xs, count, y, NeuralPath::ScalarF32));
    std::vector<f32> cpu(usize(count) * 3), gpu3(usize(count) * 3);
    for (u32 s = 0; s < count; ++s)
        for (u32 o = 0; o < 3; ++o) gpu3[s * 3 + o] = y[s * 16 + o];
    EXPECT_LT(maxError(small, xs, gpu3, count), 1e-4f);
}

TEST_F(NeuralGpuTest, Fp16InferenceMatchesCpuReference) {
    if (!neuralPathSupported(device->caps(), NeuralPath::ScalarF16)) GTEST_SKIP() << "no shaderFloat16";
    const nn::Mlp net = network(32, 64, 16, 21);
    NeuralNetwork gpu;
    ASSERT_TRUE(gpu.upload(*device, net));
    NeuralInference inference(*device);
    const u32 count = 1000;
    const auto x = inputs(count, 32, 7);
    std::vector<f32> y;
    ASSERT_TRUE(inference.evaluate(gpu, x, count, y, NeuralPath::ScalarF16));
    EXPECT_LT(maxError(net, x, y, count), 2.5e-2f);
}

TEST_F(NeuralGpuTest, CooperativeMatrixMatchesScalar) {
    if (!neuralPathSupported(device->caps(), NeuralPath::CoopMatrix))
        GTEST_SKIP() << "no VK_KHR_cooperative_matrix 16x16x16 fp16 in compute";
    const nn::Mlp net = network(32, 64, 16, 31);
    NeuralNetwork gpu;
    ASSERT_TRUE(gpu.upload(*device, net));
    NeuralInference inference(*device);
    const u32 count = 1000; // not a multiple of 16: tail handling
    const auto x = inputs(count, 32, 8);
    std::vector<f32> scalar, coop;
    ASSERT_TRUE(inference.evaluate(gpu, x, count, scalar, NeuralPath::ScalarF32));
    ASSERT_TRUE(inference.evaluate(gpu, x, count, coop, NeuralPath::CoopMatrix));
    f32 err = 0.0f;
    for (usize i = 0; i < scalar.size(); ++i) err = std::max(err, std::abs(scalar[i] - coop[i]) / std::max(1.0f, std::abs(scalar[i])));
    EXPECT_LT(err, 2e-2f); // FP16 activations between layers
    EXPECT_LT(maxError(net, x, coop, count), 2e-2f);
    // Hidden 32 variant.
    const nn::Mlp n32 = network(32, 32, 16, 32);
    NeuralNetwork g32;
    ASSERT_TRUE(g32.upload(*device, n32));
    ASSERT_TRUE(inference.evaluate(g32, x, count, coop, NeuralPath::CoopMatrix));
    EXPECT_LT(maxError(n32, x, coop, count), 2e-2f);
}

TEST_F(NeuralGpuTest, CooperativeVectorMatchesScalar) {
    if (!neuralPathSupported(device->caps(), NeuralPath::CoopVector)) GTEST_SKIP() << "no VK_NV_cooperative_vector";
    const nn::Mlp net = network(32, 64, 16, 41);
    NeuralNetwork gpu;
    ASSERT_TRUE(gpu.upload(*device, net));
    NeuralInference inference(*device);
    const u32 count = 1000;
    const auto x = inputs(count, 32, 9);
    std::vector<f32> y;
    ASSERT_TRUE(inference.evaluate(gpu, x, count, y, NeuralPath::CoopVector));
    EXPECT_LT(maxError(net, x, y, count), 3e-2f);
}

// Throughput of each path for a 1080p / 4K worth of samples (32 -> 64 -> 64 -> 16, the NTC decoder shape).
// OX_NEURAL_PERF=1 for full sizes; otherwise a small smoke run.
TEST_F(NeuralGpuTest, InferenceThroughput) {
    const bool full = std::getenv("OX_NEURAL_PERF") != nullptr;
    NeuralInference inference(*device);
    for (u32 hidden : {32u, 64u}) {
        const nn::Mlp net = network(32, hidden, 16, 51);
        NeuralNetwork gpu;
        ASSERT_TRUE(gpu.upload(*device, net));
        for (u32 count : full ? std::vector<u32>{1920u * 1080u, 3840u * 2160u} : std::vector<u32>{65536u}) {
            const auto x = inputs(count, 32, 10);
            for (u32 p = 0; p < kNeuralPathCount; ++p) {
                const NeuralPath path = NeuralPath(p);
                if (!neuralPathSupported(device->caps(), path)) continue;
                std::vector<f32> y;
                f64 ms = 0.0;
                ASSERT_TRUE(inference.evaluate(gpu, x, count, y, path, &ms, full ? 8 : 2));
                std::printf("  hidden %2u  %8u samples  %-10s %.3f ms  (%.1f Gsamples/s)\n", hidden, count, neuralPathName(path),
                            ms, ms > 0 ? count / ms * 1e-6 : 0.0);
            }
        }
    }
}
