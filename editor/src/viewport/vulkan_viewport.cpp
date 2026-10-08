#include "viewport/vulkan_viewport.hpp"

#include "viewport/viewport_panel.hpp"

#include <oxwald/core/paths.hpp>
#if OX_EDITOR_HAS_RHI
#include <oxwald/rhi/device.hpp>
#endif

namespace ox::editor {

namespace {
rhi::Device* g_device = nullptr;
bool g_pending = false;
std::function<void(rhi::DeviceDesc&)>& deviceDescHook() {
    static std::function<void(rhi::DeviceDesc&)> hook;
    return hook;
}
}
void VulkanViewportHub::setDeviceDescHook(std::function<void(rhi::DeviceDesc&)> hook) { deviceDescHook() = std::move(hook); }
void VulkanViewportHub::prepareDeviceDesc(rhi::DeviceDesc& desc) {
#if OX_EDITOR_HAS_RHI
    // Persistent Vulkan pipeline cache in the editor's user data dir (shared by all editor devices; saved atomically).
    if (desc.pipelineCachePath.empty()) desc.pipelineCachePath = paths::userDataDir("OxwaldEditor") / "cache" / "pipeline_cache.bin";
#endif
    if (deviceDescHook()) deviceDescHook()(desc);
}
bool VulkanViewportHub::pending() { return g_pending; }
void VulkanViewportHub::setPending(bool p) { g_pending = p; }
rhi::Device* VulkanViewportHub::device() { return g_device; }
void VulkanViewportHub::setDevice(rhi::Device* d) { g_device = d; }

} // namespace ox::editor

#if OX_EDITOR_HAS_RHI

#include <oxwald/core/log.hpp>
#include <oxwald/rhi/device.hpp>
#include <oxwald/rhi/environment.hpp>
#include <oxwald/rhi/swapchain.hpp>

#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan_metal.h>
#endif

#include "inspector/property_editors.hpp"

#include <QDropEvent>
#include <QExposeEvent>
#include <QMimeData>
#include <QPainter>

#include <cstring>

namespace ox::editor {

// ISurfaceProvider for the QWindow + the per-viewport GPU objects.
class VulkanSurfaceBridge final : public rhi::ISurfaceProvider {
public:
    explicit VulkanSurfaceBridge(QWindow* w) : m_window(w) {}
    ~VulkanSurfaceBridge() override { shutdown(); }

    [[nodiscard]] std::vector<const char*> requiredInstanceExtensions() const override {
#if defined(__APPLE__)
        return {"VK_KHR_surface", "VK_EXT_metal_surface"};
#else
        return {"VK_KHR_surface"};
#endif
    }

    VkSurfaceKHR createSurface(VkInstance instance) override {
#if defined(__APPLE__)
        void* layer = metalLayerForWindow(m_window);
        if (!layer) return VK_NULL_HANDLE;
        auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(rhi::loaderGetInstanceProcAddr()(instance, "vkCreateMetalSurfaceEXT"));
        if (!create) return VK_NULL_HANDLE;
        VkMetalSurfaceCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        info.pLayer = static_cast<const CAMetalLayer*>(layer);
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (create(instance, &info, nullptr, &surface) != VK_SUCCESS) return VK_NULL_HANDLE;
        return surface;
#else
        (void)instance;
        // TODO(editor): xcb / wayland / win32 surfaces for Linux and Windows editor builds.
        return VK_NULL_HANDLE;
#endif
    }

    [[nodiscard]] VkExtent2D framebufferSize() const override {
        const qreal dpr = m_window->devicePixelRatio();
        return {u32(std::max(0.0, m_window->width() * dpr)), u32(std::max(0.0, m_window->height() * dpr))};
    }

    bool init(QString* error) {
        rhi::DeviceDesc desc;
        desc.appName = "OxwaldEditor";
        desc.surface = this;
        desc.framesInFlight = 2;
        desc.shaderHotReload = true;
        VulkanViewportHub::prepareDeviceDesc(desc);
        std::string err;
        device = rhi::Device::create(desc, &err);
        if (!device) {
            if (error) *error = QString::fromStdString(err);
            return false;
        }
        rhi::SwapchainDesc sd;
        sd.presentMode = rhi::PresentMode::Mailbox;
        swapchain = rhi::Swapchain::create(*device, sd);
        if (!swapchain) {
            if (error) *error = QStringLiteral("swapchain creation failed");
            device.reset();
            return false;
        }
        VulkanViewportHub::setDevice(device.get());
        OX_LOG_INFO("editor", "Viewport Vulkan device: {} ({}), swapchain {}x{}", device->caps().gpuName,
                    device->caps().rayTracingSupported() ? "ray tracing" : "no ray tracing", swapchain->extent().width, swapchain->extent().height);
        return true;
    }

    void shutdown() {
        if (!device) return;
        device->waitIdle();
        for (auto& b : staging) {
            if (b.handle) device->destroy(b.handle);
        }
        staging.clear();
        swapchain.reset();
        VulkanViewportHub::setDevice(nullptr);
        device.reset();
    }

    struct Staging {
        rhi::BufferHandle handle;
        u64 size = 0;
    };

    QWindow* m_window;
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Swapchain> swapchain;
    std::vector<Staging> staging;
};

VulkanViewportWindow::VulkanViewportWindow(ViewportPanel* panel) : m_panel(panel), m_bridge(std::make_unique<VulkanSurfaceBridge>(this)) {
#if defined(__APPLE__)
    setSurfaceType(QSurface::MetalSurface);
#endif
}

VulkanViewportWindow::~VulkanViewportWindow() { m_bridge.reset(); }

bool VulkanViewportWindow::initialized() const { return m_bridge && m_bridge->device != nullptr; }

QString VulkanViewportWindow::deviceName() const {
    return initialized() ? QString::fromStdString(m_bridge->device->caps().gpuName) : QString();
}

bool VulkanViewportWindow::initialize(QString* error) {
    if (initialized()) return true;
    if (m_failed) return false;
    if (!m_bridge->init(error)) {
        m_failed = true;
        return false;
    }
    return true;
}

void VulkanViewportWindow::renderFrame() {
    if (!isExposed()) return;
    if (!initialized()) {
        QString err;
        if (!initialize(&err)) {
            Q_EMIT failed(err);
            return;
        }
        m_panel->onDeviceReady(*m_bridge->device);
    }
    rhi::Device& dev = *m_bridge->device;
    rhi::Swapchain& sc = *m_bridge->swapchain;
    dev.beginFrame();
    if (!sc.acquire()) {
        dev.endFrame();
        return;
    }
    const VkExtent2D ext = sc.extent();
    const qreal dpr = devicePixelRatio();
    IViewportRenderer* gpu = m_panel->gpuRenderer();
    rhi::CommandList& cmd = dev.commandList(rhi::QueueType::Graphics, "editor.viewport");
    const rhi::TextureHandle backbuffer = sc.currentTexture();
    if (gpu && !gpu->usesPainter()) {
        // A GPU renderer (render module) draws the frame itself.
        ViewportTarget target;
        target.device = &dev;
        target.swapchain = &sc;
        target.commandList = &cmd;
        m_panel->renderGpuFrame(target, QSize(int(ext.width), int(ext.height)));
        cmd.transition(backbuffer, rhi::Access::Present);
    } else {
        // Software frame -> staging buffer -> swapchain image.
        const bool bgra = sc.format() == VK_FORMAT_B8G8R8A8_SRGB || sc.format() == VK_FORMAT_B8G8R8A8_UNORM;
        QImage img(int(ext.width), int(ext.height), bgra ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
        img.setDevicePixelRatio(dpr);
        {
            QPainter p(&img);
            m_panel->paintCanvas(p, QSize(int(ext.width / dpr), int(ext.height / dpr)));
        }
        const u64 bytes = u64(img.sizeInBytes());
        auto& slots = m_bridge->staging;
        if (slots.size() < dev.framesInFlight()) slots.resize(dev.framesInFlight());
        auto& slot = slots[dev.frameIndex() % slots.size()];
        if (!slot.handle || slot.size < bytes) {
            if (slot.handle) dev.destroy(slot.handle);
            rhi::BufferDesc bd;
            bd.size = bytes;
            bd.usage = rhi::BufferUsage::TransferSrc;
            bd.memory = rhi::MemoryUsage::Upload;
            bd.name = "editor.viewport.staging";
            slot.handle = dev.createBuffer(bd);
            slot.size = bytes;
        }
        std::memcpy(dev.mapped(slot.handle), img.constBits(), size_t(bytes));
        cmd.transition(backbuffer, rhi::Access::TransferWrite, true);
        cmd.copyBufferToTexture(slot.handle, 0, backbuffer);
        cmd.transition(backbuffer, rhi::Access::Present);
    }
    rhi::SubmitInfo si;
    si.swapchain = &sc;
    dev.submit(cmd, si);
    sc.present();
    dev.endFrame();
}

void VulkanViewportWindow::exposeEvent(QExposeEvent*) {
    if (isExposed()) m_panel->requestRedraw();
}
void VulkanViewportWindow::resizeEvent(QResizeEvent*) {
    if (initialized()) m_bridge->swapchain->recreate();
    m_panel->requestRedraw();
}
void VulkanViewportWindow::mousePressEvent(QMouseEvent* e) { m_panel->onMousePress(e); }
void VulkanViewportWindow::mouseMoveEvent(QMouseEvent* e) { m_panel->onMouseMove(e); }
void VulkanViewportWindow::mouseReleaseEvent(QMouseEvent* e) { m_panel->onMouseRelease(e); }
void VulkanViewportWindow::wheelEvent(QWheelEvent* e) { m_panel->onWheel(e); }
void VulkanViewportWindow::keyPressEvent(QKeyEvent* e) { m_panel->onKey(e, true); }
void VulkanViewportWindow::keyReleaseEvent(QKeyEvent* e) { m_panel->onKey(e, false); }
void VulkanViewportWindow::focusOutEvent(QFocusEvent* e) {
    m_panel->clearInputState();
    QWindow::focusOutEvent(e);
}
bool VulkanViewportWindow::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride && m_panel->isFlying()) {
        e->accept();
        return true;
    }
    // Content browser drops (QWindow has no drag handlers; the platform still delivers the events).
    if (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove) {
        auto* de = static_cast<QDragMoveEvent*>(e);
        if (de->mimeData()->hasFormat(kMimeAsset)) {
            de->acceptProposedAction();
            return true;
        }
    } else if (e->type() == QEvent::Drop) {
        auto* de = static_cast<QDropEvent*>(e);
        for (const QString& line : QString::fromUtf8(de->mimeData()->data(kMimeAsset)).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            m_panel->dropAsset(line, de->position());
        }
        de->acceptProposedAction();
        return true;
    }
    return QWindow::event(e);
}

} // namespace ox::editor

#endif
