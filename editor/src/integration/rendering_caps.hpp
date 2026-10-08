#pragma once

#include <oxwald/core/cvar.hpp>

#include <QString>

#include <array>
#include <memory>

namespace ox::editor {

// What the settings UI needs to know about the GPU/platform. Filled from rhi::DeviceCaps when the rhi module
// is integrated (see RhiCapsProvider), otherwise from a platform heuristic.
struct RenderingCaps {
    bool deviceAvailable = false;
    QString gpuName = QStringLiteral("Unknown GPU");
    QString vendor;
    QString driver;
    QString apiVersion;
    bool discreteGpu = false;
    bool unifiedMemory = false;
    quint64 vramBytes = 0;
    bool rayTracingSupported = false;
    QString rayTracingUnavailableReason; // empty when supported
    bool dlssSupported = false;
    QString dlssUnavailableReason; // empty when supported
    bool fsr1Supported = true;
    bool meshShaders = false;
    int cpuCores = 1;
    quint64 systemMemoryBytes = 0;
    QString platform;
};

class IRenderingCapsProvider {
public:
    virtual ~IRenderingCapsProvider() = default;
    [[nodiscard]] virtual RenderingCaps caps() const = 0;
};

// Fixed caps (tests, or when no device exists).
class StaticCapsProvider final : public IRenderingCapsProvider {
public:
    explicit StaticCapsProvider(RenderingCaps caps) : m_caps(std::move(caps)) {}
    [[nodiscard]] RenderingCaps caps() const override { return m_caps; }
    void set(RenderingCaps caps) { m_caps = std::move(caps); }

private:
    RenderingCaps m_caps;
};

// Platform/CPU facts plus "no device" reasons; the default when rhi is not integrated.
[[nodiscard]] RenderingCaps detectHostCaps();
// Why DLSS cannot run (empty when it can): needs Windows/Linux, an NVIDIA RTX GPU and the NGX runtime.
[[nodiscard]] QString dlssUnavailableReason(const QString& vendor, bool rayTracing, bool deviceAvailable);
[[nodiscard]] std::unique_ptr<IRenderingCapsProvider> createDefaultCapsProvider();

// ---- quality auto-detect (UE "Auto Detect" / Scalability benchmark) ------------------------------------------

struct BenchmarkResult {
    float cpuIndex = 100.0f; // 100 = baseline desktop (UE convention)
    float gpuIndex = 100.0f;
    std::array<QualityLevel, kScalabilityGroupCount> levels{};
    QString details;
};

// The renderer replaces the heuristic with a real GPU benchmark (render::benchmark) by registering its own
// implementation in EditorServices.
class IQualityBenchmark {
public:
    virtual ~IQualityBenchmark() = default;
    [[nodiscard]] virtual QString name() const = 0;
    [[nodiscard]] virtual BenchmarkResult run(const RenderingCaps& caps) = 0;
};

// CPU micro-benchmark + caps heuristic (VRAM, discrete GPU, vendor, RT support).
class HeuristicBenchmark final : public IQualityBenchmark {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("Heuristic (CPU benchmark + GPU caps)"); }
    [[nodiscard]] BenchmarkResult run(const RenderingCaps& caps) override;
    // Pure mapping used by run(); exposed for tests.
    [[nodiscard]] static std::array<QualityLevel, kScalabilityGroupCount> levelsFor(float cpuIndex, float gpuIndex,
                                                                                    const RenderingCaps& caps);
};

} // namespace ox::editor
