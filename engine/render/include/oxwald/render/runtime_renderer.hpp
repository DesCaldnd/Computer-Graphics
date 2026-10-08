#pragma once

// ox::IRenderer (runtime module) implemented by the render module. Available when both render and runtime are
// configured (OX_RENDER_HAS_RUNTIME); apps link Oxwald::render and install it:
//
//   engine.setRenderer(ox::render::createRenderer());
//
// init() creates the Vulkan device on the runtime's surface provider (headless → offscreen target), a swapchain, the
// Renderer and one view; extract() fills snapshot slot ctx.slot on the game thread (flushes the DebugDraw service);
// render() draws slot ctx.slot from the primary camera; resize()/settingsChanged() run on the render thread.

#include <oxwald/runtime/renderer.hpp>

#include <memory>

namespace ox::render {

class Renderer;

struct RuntimeRendererOptions {
    bool autoDetectQuality = false; // run the GPU benchmark in init() and apply the recommended levels
    u32 headlessWidth = 1280;       // offscreen target size without a surface
    u32 headlessHeight = 720;
};

std::unique_ptr<IRenderer> createRenderer(const RuntimeRendererOptions& options = {});

// The Renderer behind an IRenderer created by createRenderer() (nullptr for other renderers or before init()).
Renderer* rendererOf(IRenderer& renderer);

} // namespace ox::render
