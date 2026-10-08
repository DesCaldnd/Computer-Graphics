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

// Native surface glue. volk is built without VK_USE_PLATFORM_*, so the platform headers are included here and the
// create functions are resolved through the loader (same as the Metal path). <windows.h> comes last on purpose: no
// Qt or engine header is parsed after its macros.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#elif defined(__linux__)
#include <QGuiApplication>
#include <QtGui/qtguiglobal.h>
#if QT_CONFIG(xcb)
// Only the handle types vulkan_xcb.h needs (identical to <xcb/xcb.h>, which is not required to build the editor).
struct xcb_connection_t;
typedef uint32_t xcb_window_t;
typedef uint32_t xcb_visualid_t;
#include <vulkan/vulkan_xcb.h>
#endif
#endif

namespace ox::editor {

#if defined(__linux__) && QT_CONFIG(xcb)
namespace {
// X11 connection of the running Qt platform plugin (nullptr on Wayland and other platforms).
xcb_connection_t* xcbConnection() {
    if (QGuiApplication::platformName() != QLatin1String("xcb")) return nullptr;
    auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}
} // namespace
#endif

// ISurfaceProvider for the QWindow + the per-viewport GPU objects.
class VulkanSurfaceBridge final : public rhi::ISurfaceProvider {
public:
    explicit VulkanSurfaceBridge(QWindow* w) : m_window(w) {}
    ~VulkanSurfaceBridge() override { shutdown(); }

    [[nodiscard]] std::vector<const char*> requiredInstanceExtensions() const override {
#if defined(__APPLE__)
        return {"VK_KHR_surface", "VK_EXT_metal_surface"};
#elif defined(_WIN32)
        return {"VK_KHR_surface", "VK_KHR_win32_surface"};
#elif defined(__linux__) && QT_CONFIG(xcb)
        if (xcbConnection()) return {"VK_KHR_surface", "VK_KHR_xcb_surface"};
        return {"VK_KHR_surface"};
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
#elif defined(_WIN32)
        const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
        if (!hwnd) return VK_NULL_HANDLE;
        auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(rhi::loaderGetInstanceProcAddr()(instance, "vkCreateWin32SurfaceKHR"));
        if (!create) return VK_NULL_HANDLE;
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hinstance = GetModuleHandleW(nullptr);
        info.hwnd = hwnd;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (create(instance, &info, nullptr, &surface) != VK_SUCCESS) return VK_NULL_HANDLE;
        return surface;
#elif defined(__linux__) && QT_CONFIG(xcb)
        xcb_connection_t* connection = xcbConnection();
        if (!connection) {
            // Wayland needs the wl_surface, which Qt only exposes through private API.
            OX_LOG_WARN("editor", "No Vulkan surface for the Qt platform '{}' (run with QT_QPA_PLATFORM=xcb)", QGuiApplication::platformName().toStdString());
            return VK_NULL_HANDLE;
        }
        auto create = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(rhi::loaderGetInstanceProcAddr()(instance, "vkCreateXcbSurfaceKHR"));
        if (!create) return VK_NULL_HANDLE;
        VkXcbSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        info.connection = connection;
        info.window = xcb_window_t(m_window->winId());
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (create(instance, &info, nullptr, &surface) != VK_SUCCESS) return VK_NULL_HANDLE;
        return surface;
#else
        (void)instance;
        return VK_NULL_HANDLE;
#endif
    }

    [[nodiscard]] VkExtent2D framebufferSize() const override {
#if defined(_WIN32)
        // The real client area in physical pixels: width() * devicePixelRatio() can be one pixel off at fractional
        // scale factors (125 %, 150 %).
        RECT rc{};
        if (m_window->handle() && GetClientRect(reinterpret_cast<HWND>(m_window->winId()), &rc)) {
            return {u32(std::max<LONG>(0, rc.right - rc.left)), u32(std::max<LONG>(0, rc.bottom - rc.top))};
        }
#endif
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
#else
    // Win32 / xcb: a plain native window that Qt never paints into (no backing store, no GL visual).
    setSurfaceType(QSurface::VulkanSurface);
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
