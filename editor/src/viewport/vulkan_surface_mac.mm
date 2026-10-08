#include "viewport/vulkan_viewport.hpp"

#if OX_EDITOR_HAS_RHI

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

namespace ox::editor {

void* metalLayerForWindow(QWindow* window) {
    NSView* view = reinterpret_cast<NSView*>(window->winId());
    if (!view) return nullptr;
    // QWindow::MetalSurface backs the view with Qt's QContainerLayer whose `contentLayer` is the CAMetalLayer
    // (MoltenVK needs a real CAMetalLayer). Fall back to installing one ourselves.
    CALayer* root = view.layer;
    CALayer* candidate = root;
    if (root && ![root isMemberOfClass:[CAMetalLayer class]] && [root respondsToSelector:@selector(contentLayer)]) {
        candidate = [root performSelector:@selector(contentLayer)];
    }
    if (![candidate isKindOfClass:[CAMetalLayer class]] || ![candidate respondsToSelector:@selector(setDrawableSize:)]) {
        view.wantsLayer = YES;
        candidate = [CAMetalLayer layer];
        view.layer = candidate;
    }
    CAMetalLayer* layer = (CAMetalLayer*)candidate;
    layer.contentsScale = window->devicePixelRatio();
    return (void*)layer;
}

} // namespace ox::editor

#endif
