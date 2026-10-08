#pragma once

#include <QImage>
#include <QString>
#include <QWindow>

#include <memory>

#include <functional>

namespace ox::rhi {
class Device;
class Swapchain;
struct DeviceDesc;
} // namespace ox::rhi

namespace ox::editor {

class ViewportPanel;

// The one rhi::Device of the editor (volk allows a single device per process). Owned by the Vulkan viewport;
// the caps provider reads from it when it exists.
class VulkanViewportHub {
public:
    static rhi::Device* device();
    static void setDevice(rhi::Device* d);
    // A Vulkan viewport exists and will create the device (caps probes then wait for it instead of creating a
    // throw-away headless device first; the old vk-bootstrap crash for that order is fixed in rhi, but one device
    // per process is still what volk supports).
    static bool pending();
    static void setPending(bool p);
    // Applied to every DeviceDesc the editor creates (viewport, offscreen canvas, benchmark): the render module adds
    // the Vulkan extensions its upscalers need (render::appendUpscalerVulkanExtensions, DLSS/NGX).
    static void setDeviceDescHook(std::function<void(rhi::DeviceDesc&)> hook);
    static void prepareDeviceDesc(rhi::DeviceDesc& desc);
};

#if OX_EDITOR_HAS_RHI
class VulkanSurfaceBridge;

// Native window embedded with QWidget::createWindowContainer. Owns the rhi Device + Swapchain for the viewport
// and presents frames. Until the render module registers a GPU IViewportRenderer, each frame is drawn by the
// software renderer into a QImage, copied into the swapchain image (staging buffer -> copyBufferToTexture) and
// presented — the full Vulkan surface/swapchain/frames-in-flight path is exercised either way.
class VulkanViewportWindow : public QWindow {
    Q_OBJECT
public:
    explicit VulkanViewportWindow(ViewportPanel* panel);
    ~VulkanViewportWindow() override;

    // Creates device + swapchain (needs a native, exposed window). False + error on failure.
    bool initialize(QString* error);
    [[nodiscard]] bool initialized() const;
    void renderFrame();
    [[nodiscard]] QString deviceName() const;

Q_SIGNALS:
    void failed(const QString& reason);

protected:
    void exposeEvent(QExposeEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    bool event(QEvent* e) override;

private:
    ViewportPanel* m_panel;
    std::unique_ptr<VulkanSurfaceBridge> m_bridge;
    bool m_failed = false;
};

// Platform glue (vulkan_surface_mac.mm on macOS): CAMetalLayer of a QWindow with MetalSurface type.
void* metalLayerForWindow(QWindow* window);
#endif

} // namespace ox::editor
