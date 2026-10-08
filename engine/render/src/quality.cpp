#include <oxwald/core/log.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/quality.hpp>
#include <oxwald/render/render_feature.hpp>
#include <oxwald/render/render_settings.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/render_graph.hpp>

#include <cmath>
#include <format>

namespace ox::render {

namespace {

// Reference "score 100" GPU (GTX 1060 / RX 580 class): blended RGBA16F fill, FP32 FMA throughput, copy bandwidth.
constexpr f64 kRefFillGPix = 30.0;
constexpr f64 kRefAluGFlops = 4000.0;
constexpr f64 kRefBandwidthGBs = 180.0;

constexpr u32 kFillLayers = 8;
constexpr u32 kAluThreads = 256 * 1024;
constexpr u32 kAluIterations = 256;
constexpr u32 kBandwidthElements = 4u << 20; // vec4 → 64 MiB per buffer

} // namespace

std::array<QualityLevel, kScalabilityGroupCount> levelsForScore(f64 score, bool rayTracingSupported) {
    QualityLevel l = QualityLevel::Low;
    if (score >= 160.0) l = QualityLevel::Ultra;
    else if (score >= 80.0) l = QualityLevel::High;
    else if (score >= 35.0) l = QualityLevel::Medium;
    std::array<QualityLevel, kScalabilityGroupCount> out;
    out.fill(l);
    // Expensive groups one step lower unless the GPU is far above the Ultra threshold.
    auto lower = [](QualityLevel q) { return q == QualityLevel::Low ? q : QualityLevel(i32(q) - 1); };
    if (score < 250.0) {
        out[usize(Scalability::GlobalIllumination)] = lower(l);
        out[usize(Scalability::Volumetrics)] = lower(l);
    }
    out[usize(Scalability::RayTracing)] = rayTracingSupported ? lower(l) : QualityLevel::Low;
    return out;
}

BenchmarkResult runGpuBenchmark(rhi::Device& device, const BenchmarkOptions& options) {
    registerRenderCVars();
    BenchmarkResult result;
    result.gpuName = device.caps().gpuName;
    if (!device.caps().timestampQueries) {
        OX_LOG_WARN("render", "benchmark: no timestamp queries, quality detection unavailable");
        return result;
    }
    rhi::PipelineHandle fill = createFullscreenPipeline(device, "bench.fill", "render/bench/fill.frag",
                                                        {VK_FORMAT_R16G16B16A16_SFLOAT}, {rhi::BlendState::alpha()});
    rhi::PipelineHandle alu = createComputePipeline(device, "bench.alu", "render/bench/alu.comp");
    rhi::PipelineHandle bw = createComputePipeline(device, "bench.bandwidth", "render/bench/bandwidth.comp");
    const u64 bwBytes = u64(kBandwidthElements) * 16;
    rhi::BufferHandle src = device.createBuffer({bwBytes, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly, "bench.src"});
    rhi::BufferHandle dst = device.createBuffer({bwBytes, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly, "bench.dst"});
    rhi::BufferHandle aluOut = device.createBuffer({kAluThreads * 16ull, rhi::BufferUsage::Storage, rhi::MemoryUsage::GpuOnly,
                                                    "bench.aluOut"});
    rhi::RenderGraph graph;
    const u32 w = options.width, h = options.height;
    f64 bestFill = 1e9, bestAlu = 1e9, bestBw = 1e9;

    // One test per frame: MoltenVK may merge consecutive compute passes into one Metal encoder, which smears
    // timestamps between passes of the same command buffer.
    auto runTest = [&](u32 test) -> f64 {
        device.beginFrame();
        graph.reset();
        const char* name = test == 0 ? "Bench.Fill" : test == 1 ? "Bench.ALU" : "Bench.Bandwidth";
        if (test == 0) {
            rhi::TextureDesc td;
            td.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            td.width = w;
            td.height = h;
            td.usage = rhi::TextureUsage::None;
            td.name = "bench.target";
            const rhi::RGTexture target = graph.createTexture(td);
            graph.markOutput(target);
            graph.addPass(name).color(target, VK_ATTACHMENT_LOAD_OP_CLEAR).execute([&](rhi::PassContext& p) {
                p.cmd.bindPipeline(fill);
                for (u32 i = 0; i < kFillLayers; ++i) p.cmd.draw(3);
            });
        } else if (test == 1) {
            graph.addPass(name, rhi::PassType::Compute).sideEffect().execute([&](rhi::PassContext& p) {
                struct {
                    u64 dst;
                    u32 iterations;
                    f32 seed;
                } pc{device.address(aluOut), kAluIterations, 0.5f};
                p.cmd.bindPipeline(alu);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch(kAluThreads / 256);
            });
        } else {
            graph.addPass(name, rhi::PassType::Compute).sideEffect().execute([&](rhi::PassContext& p) {
                struct {
                    u64 src, dst;
                    u32 count;
                } pc{device.address(src), device.address(dst), kBandwidthElements};
                p.cmd.bindPipeline(bw);
                p.cmd.pushConstants(pc);
                p.cmd.dispatch(kBandwidthElements / 256);
            });
        }
        graph.execute(device, {.timestamps = true, .asyncCompute = false});
        device.endFrame();
        f64 ms = -1.0;
        for (u32 k = 0; k < device.framesInFlight() + 1 && ms < 0.0; ++k) {
            device.beginFrame();
            for (const rhi::GpuTiming& t : device.gpuTimings()) {
                if (t.name == name) ms = t.milliseconds;
            }
            device.endFrame();
        }
        return ms;
    };
    for (u32 it = 0; it < std::max(options.iterations, 1u) + 1; ++it) { // +1 warm-up
        const f64 tf = runTest(0), ta = runTest(1), tb = runTest(2);
        if (it == 0 || tf <= 0.0 || ta <= 0.0 || tb <= 0.0) continue;
        bestFill = std::min(bestFill, tf);
        bestAlu = std::min(bestAlu, ta);
        bestBw = std::min(bestBw, tb);
    }
    device.waitIdle();
    graph.releaseResources(device);
    device.destroy(src);
    device.destroy(dst);
    device.destroy(aluOut);
    for (auto p : {fill, alu, bw}) device.destroy(p);
    if (bestFill >= 1e9 || bestAlu >= 1e9 || bestBw >= 1e9) {
        OX_LOG_WARN("render", "benchmark: timestamps were not collected");
        return result;
    }
    result.fillMs = bestFill;
    result.aluMs = bestAlu;
    result.bandwidthMs = bestBw;
    result.fillRateGPixels = f64(w) * h * kFillLayers / (bestFill * 1e-3) / 1e9;
    result.aluGFlops = f64(kAluThreads) * kAluIterations * 8 /*FMA per iter*/ * 4 /*lanes*/ * 2 / (bestAlu * 1e-3) / 1e9;
    result.bandwidthGBs = f64(bwBytes) * 2 / (bestBw * 1e-3) / 1e9;
    result.score = 100.0 * std::cbrt((result.fillRateGPixels / kRefFillGPix) * (result.aluGFlops / kRefAluGFlops) *
                                     (result.bandwidthGBs / kRefBandwidthGBs));
    result.levels = levelsForScore(result.score, device.caps().rayTracingSupported());
    result.valid = true;
    return result;
}

BenchmarkResult autoDetectQuality(rhi::Device& device, const BenchmarkOptions& options) {
    BenchmarkResult r = runGpuBenchmark(device, options);
    if (r.valid) OX_LOG_INFO("render", "{}", r.toString());
    return r;
}

void applyQuality(const BenchmarkResult& result) {
    if (!result.valid) return;
    for (usize g = 0; g < kScalabilityGroupCount; ++g) scalability::setGroup(Scalability(g), result.levels[g]);
}

std::string BenchmarkResult::toString() const {
    std::string s = std::format("GPU benchmark ({}): fill {:.1f} GPix/s ({:.2f} ms), ALU {:.0f} GFLOPS ({:.2f} ms), "
                                "bandwidth {:.0f} GB/s ({:.2f} ms) → score {:.0f}; levels:",
                                gpuName, fillRateGPixels, fillMs, aluGFlops, aluMs, bandwidthGBs, bandwidthMs, score);
    for (usize g = 0; g < kScalabilityGroupCount; ++g) {
        s += std::format(" {}={}", scalability::groupName(Scalability(g)), scalability::levelName(levels[g]));
    }
    return s;
}

} // namespace ox::render
