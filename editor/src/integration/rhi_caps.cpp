// Rendering caps from a real Vulkan device (rhi module). Compiled only when the rhi module is integrated.
#include "integration/rendering_caps.hpp"

#if OX_EDITOR_HAS_RHI

#include "viewport/vulkan_viewport.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/rhi/device.hpp>

#include <QGuiApplication>

namespace ox::editor {

namespace {

RenderingCaps fromDevice(const rhi::DeviceCaps& d) {
    RenderingCaps c = detectHostCaps();
    c.deviceAvailable = true;
    c.gpuName = QString::fromStdString(d.gpuName);
    c.vendor = QString::fromLatin1(rhi::gpuVendorName(d.vendor));
    c.driver = QString::fromStdString(d.driverName + " " + d.driverInfo);
    c.apiVersion = QStringLiteral("Vulkan %1.%2").arg(d.usedApiVersion >> 22).arg((d.usedApiVersion >> 12) & 0x3ff);
    c.discreteGpu = d.discreteGpu;
    c.unifiedMemory = d.unifiedMemory;
    c.vramBytes = d.deviceLocalMemoryBytes;
    c.rayTracingSupported = d.rayTracingSupported();
    c.rayTracingUnavailableReason = QString::fromStdString(d.whyRayTracingUnavailable());
    c.meshShaders = d.meshShader;
    c.dlssUnavailableReason = dlssUnavailableReason(c.vendor, c.rayTracingSupported, true);
    c.dlssSupported = c.dlssUnavailableReason.isEmpty();
    c.upscalers = defaultUpscalers(c);
    return c;
}

// Queries the caps once: from the viewport's device if one exists, otherwise from a short-lived headless device
// (only one rhi::Device may exist at a time, so it is destroyed right away).
class RhiCapsProvider final : public IRenderingCapsProvider {
public:
    [[nodiscard]] RenderingCaps caps() const override {
        if (m_cached) return *m_cached;
        if (rhi::Device* live = VulkanViewportHub::device()) {
            m_cached = fromDevice(live->caps());
            return *m_cached;
        }
        if (VulkanViewportHub::pending()) {
            // the viewport creates the device once its window is exposed; report host facts until then
            RenderingCaps c = detectHostCaps();
            c.rayTracingUnavailableReason = QStringLiteral("Waiting for the viewport's Vulkan device…");
            return c;
        }
        const QString platform = QGuiApplication::platformName();
        if (qEnvironmentVariableIsSet("OX_EDITOR_NO_VULKAN")) {
            RenderingCaps c = detectHostCaps();
            c.rayTracingUnavailableReason = QStringLiteral("Vulkan disabled for this session (OX_EDITOR_NO_VULKAN).");
            m_cached = c;
            return c;
        }
        rhi::DeviceDesc desc;
        desc.appName = "OxwaldEditor caps probe";
        desc.validation = false;
        desc.shaderHotReload = false;
        std::string error;
        auto device = rhi::Device::create(desc, &error);
        if (!device) {
            RenderingCaps c = detectHostCaps();
            c.rayTracingUnavailableReason = QStringLiteral("No Vulkan device could be created: %1").arg(QString::fromStdString(error));
            OX_LOG_WARN("editor", "Vulkan caps probe failed: {}", error);
            m_cached = c;
            return c;
        }
        m_cached = fromDevice(device->caps());
        (void)platform;
        return *m_cached;
    }

private:
    mutable std::optional<RenderingCaps> m_cached;
};

} // namespace

std::unique_ptr<IRenderingCapsProvider> createRhiCapsProvider() { return std::make_unique<RhiCapsProvider>(); }

} // namespace ox::editor

#endif
