#include "integration/rendering_caps.hpp"

#include <QElapsedTimer>
#include <QSysInfo>
#include <QThread>

#include <algorithm>
#include <cmath>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace ox::editor {

namespace {

quint64 systemMemory() {
#if defined(__APPLE__)
    int64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) return quint64(mem);
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0) return quint64(pages) * quint64(page);
#elif defined(_WIN32)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) return st.ullTotalPhys;
#endif
    return 0;
}

QString hostGpuGuess() {
#if defined(__APPLE__)
    char buf[256] = {};
    size_t len = sizeof(buf);
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) {
        QString cpu = QString::fromUtf8(buf);
        if (cpu.contains(QLatin1String("Apple"))) return cpu + QStringLiteral(" GPU");
    }
    return QStringLiteral("Apple GPU");
#else
    return QStringLiteral("Unknown GPU");
#endif
}

} // namespace

QString dlssUnavailableReason(const QString& vendor, bool rayTracing, bool deviceAvailable) {
#if defined(__APPLE__)
    Q_UNUSED(vendor);
    Q_UNUSED(rayTracing);
    Q_UNUSED(deviceAvailable);
    return QStringLiteral("DLSS needs an NVIDIA RTX GPU on Windows or Linux. macOS (Metal via MoltenVK) is not "
                          "supported by NVIDIA NGX.");
#else
    if (!deviceAvailable) return QStringLiteral("No Vulkan device was created, so DLSS support cannot be checked.");
    if (!vendor.contains(QLatin1String("NVIDIA"), Qt::CaseInsensitive)) {
        return QStringLiteral("DLSS needs an NVIDIA RTX GPU (current GPU vendor: %1).").arg(vendor.isEmpty() ? QStringLiteral("unknown") : vendor);
    }
    if (!rayTracing) return QStringLiteral("DLSS needs an NVIDIA RTX (Turing or newer) GPU with tensor cores.");
#if !defined(OX_ENABLE_DLSS_RUNTIME)
    return QStringLiteral("This build was compiled without the NVIDIA DLSS runtime (OX_ENABLE_DLSS).");
#else
    return {};
#endif
#endif
}

RenderingCaps detectHostCaps() {
    RenderingCaps c;
    c.cpuCores = std::max(1, QThread::idealThreadCount());
    c.systemMemoryBytes = systemMemory();
    c.platform = QSysInfo::prettyProductName() + QStringLiteral(" (") + QSysInfo::currentCpuArchitecture() + QLatin1Char(')');
    c.gpuName = hostGpuGuess();
#if defined(__APPLE__)
    c.vendor = QStringLiteral("Apple");
    c.unifiedMemory = true;
    c.vramBytes = c.systemMemoryBytes * 2 / 3;
#endif
    c.deviceAvailable = false;
    c.rayTracingSupported = false;
#if OX_EDITOR_HAS_RHI
    c.rayTracingUnavailableReason = QStringLiteral("No Vulkan device has been created yet.");
#else
    c.rayTracingUnavailableReason =
        QStringLiteral("This editor build has no Vulkan backend (rhi module not integrated), so ray tracing "
                       "support cannot be queried.");
#endif
    c.dlssSupported = false;
    c.dlssUnavailableReason = dlssUnavailableReason(c.vendor, false, false);
    c.fsr1Supported = true;
    return c;
}

#if OX_EDITOR_HAS_RHI
std::unique_ptr<IRenderingCapsProvider> createRhiCapsProvider(); // rhi_caps.cpp
#endif

std::unique_ptr<IRenderingCapsProvider> createDefaultCapsProvider() {
#if OX_EDITOR_HAS_RHI
    return createRhiCapsProvider();
#else
    return std::make_unique<StaticCapsProvider>(detectHostCaps());
#endif
}

// ---- heuristic benchmark --------------------------------------------------------------------------------------

namespace {

// ~20 ms of scalar float work, scaled so that a 2020-era desktop core scores ~100 per core-equivalent.
float cpuMicroBenchmark(int cores) {
    QElapsedTimer t;
    t.start();
    volatile float sink = 0.0f;
    float x = 1.0001f;
    qint64 iterations = 0;
    while (t.nsecsElapsed() < 20'000'000) {
        for (int i = 0; i < 20000; ++i) {
            x = x * 1.000001f + std::sqrt(x) * 0.0001f;
            if (x > 1000.0f) x = 1.0001f;
        }
        iterations += 20000;
    }
    sink = x;
    (void)sink;
    const double perSecond = double(iterations) / (double(t.nsecsElapsed()) * 1e-9);
    const double singleCore = perSecond / 3.5e8 * 100.0; // baseline
    const double multi = singleCore * std::sqrt(double(std::max(1, cores)) / 8.0);
    return float(std::clamp(multi, 10.0, 400.0));
}

QualityLevel levelFromIndex(float index) {
    // UE thresholds (sg.* mapping in BaseScalability.ini): <20 Low, <50 Medium, <70 High, else Epic/Ultra.
    if (index < 20.0f) return QualityLevel::Low;
    if (index < 50.0f) return QualityLevel::Medium;
    if (index < 70.0f) return QualityLevel::High;
    return QualityLevel::Ultra;
}

} // namespace

std::array<QualityLevel, kScalabilityGroupCount> HeuristicBenchmark::levelsFor(float cpuIndex, float gpuIndex,
                                                                                const RenderingCaps& caps) {
    std::array<QualityLevel, kScalabilityGroupCount> lv{};
    const QualityLevel cpu = levelFromIndex(cpuIndex);
    const QualityLevel gpu = levelFromIndex(gpuIndex);
    const QualityLevel both = QualityLevel(std::min(int(cpu), int(gpu)));
    const quint64 gb = caps.vramBytes / (1024ull * 1024ull * 1024ull);
    const QualityLevel tex = gb >= 10 ? QualityLevel::Ultra
                            : gb >= 6 ? QualityLevel::High
                            : gb >= 3 ? QualityLevel::Medium
                                      : QualityLevel::Low;
    auto set = [&](Scalability g, QualityLevel l) { lv[usize(g)] = l; };
    set(Scalability::ViewDistance, both);
    set(Scalability::AntiAliasing, gpu);
    set(Scalability::Shadows, both);
    set(Scalability::GlobalIllumination, gpu);
    set(Scalability::Reflections, gpu);
    set(Scalability::PostProcess, gpu);
    set(Scalability::Textures, QualityLevel(std::min(int(tex), int(gpu) + 1)));
    set(Scalability::Effects, both);
    set(Scalability::Foliage, both);
    set(Scalability::Shading, gpu);
    set(Scalability::Volumetrics, gpu);
    set(Scalability::RayTracing, caps.rayTracingSupported ? gpu : QualityLevel::Low);
    return lv;
}

BenchmarkResult HeuristicBenchmark::run(const RenderingCaps& caps) {
    BenchmarkResult r;
    r.cpuIndex = cpuMicroBenchmark(caps.cpuCores);
    float gpu = 35.0f;
    const double vramGb = double(caps.vramBytes) / (1024.0 * 1024.0 * 1024.0);
    if (caps.discreteGpu) gpu += 30.0f;
    if (caps.unifiedMemory) gpu += 15.0f; // Apple silicon / modern APUs
    gpu += float(std::min(vramGb, 16.0) * 2.0);
    if (caps.rayTracingSupported) gpu += 15.0f;
    if (caps.meshShaders) gpu += 5.0f;
    if (!caps.deviceAvailable) gpu = std::min(gpu, 65.0f); // unknown GPU: stay conservative
    r.gpuIndex = std::clamp(gpu, 10.0f, 250.0f);
    r.levels = levelsFor(r.cpuIndex, r.gpuIndex, caps);
    r.details = QStringLiteral("CPU index %1 (%2 threads), GPU index %3 (%4, %5 VRAM%6)")
                    .arg(r.cpuIndex, 0, 'f', 0)
                    .arg(caps.cpuCores)
                    .arg(r.gpuIndex, 0, 'f', 0)
                    .arg(caps.gpuName)
                    .arg(vramGb, 0, 'f', 1)
                    .arg(caps.deviceAvailable ? QString() : QStringLiteral(", estimated - no GPU benchmark available"));
    return r;
}

} // namespace ox::editor
