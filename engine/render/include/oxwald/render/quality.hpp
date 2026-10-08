#pragma once

// GPU quality auto-detection (UE "synthbenchmark" style): a few milliseconds of fill-rate, ALU and bandwidth tests
// through the render graph, mapped to scalability levels. Works headless (no window / swapchain needed).
//
//   auto result = ox::render::autoDetectQuality(device);
//   ox::render::applyQuality(result);           // scalability::setGroup(...) for every group
//   OX_LOG_INFO("render", "{}", result.toString());

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/types.hpp>

#include <array>
#include <string>

namespace ox::rhi {
class Device;
}

namespace ox::render {

struct BenchmarkResult {
    bool valid = false;
    f64 fillRateGPixels = 0.0;   // RGBA16F blended pixels / s
    f64 aluGFlops = 0.0;         // compute FMA throughput
    f64 bandwidthGBs = 0.0;      // buffer copy through compute (read + write)
    f64 fillMs = 0.0, aluMs = 0.0, bandwidthMs = 0.0;
    f64 score = 0.0;             // 100 = reference mid-range GPU (GTX 1060 / RX 580 class)
    std::array<QualityLevel, kScalabilityGroupCount> levels{};
    std::string gpuName;

    [[nodiscard]] std::string toString() const;
};

struct BenchmarkOptions {
    u32 width = 1920;
    u32 height = 1080;
    u32 iterations = 3; // best of N
};

[[nodiscard]] BenchmarkResult runGpuBenchmark(rhi::Device& device, const BenchmarkOptions& options = {});
// Maps a score to levels (pure, tested): ray tracing stays Low when unsupported.
[[nodiscard]] std::array<QualityLevel, kScalabilityGroupCount> levelsForScore(f64 score, bool rayTracingSupported);
// Benchmark + mapping.
[[nodiscard]] BenchmarkResult autoDetectQuality(rhi::Device& device, const BenchmarkOptions& options = {});
void applyQuality(const BenchmarkResult& result);

} // namespace ox::render
