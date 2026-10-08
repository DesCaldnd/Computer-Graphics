#pragma once

// UiOverlayFeature: draws the latest UiFrame of a UiRenderBridge into SceneColorLDR at the Overlay injection point
// (output resolution, after tonemapping and the built-in debug lines). One pipeline for ImGui and RmlUi: vertex and
// index pulling through BDA, bindless textures, per-command scissor rects and optional transforms, premultiplied
// blending. Also publishes render introspection (stats, memory, caps, render graph) back to the bridge.
//
//   auto bridge = uiSystem.bridgeShared();
//   ox::ui::attachRenderer(renderer, bridge);    // or engine.setRenderer(ox::ui::withUi(render::createRenderer()))

#include <oxwald/render/render_feature.hpp>
#include <oxwald/ui/draw_data.hpp>

#include <memory>
#include <unordered_map>

namespace ox::render {
class Renderer;
}

namespace ox::ui {

class UiOverlayFeature final : public render::IRenderFeature {
public:
    static constexpr std::string_view kName = "UI";

    explicit UiOverlayFeature(std::shared_ptr<UiRenderBridge> bridge);

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] render::InjectionMask injectionPoints() const override {
        return render::maskOf(render::InjectionPoint::Overlay);
    }
    // After the built-in overlays (editor grid 0, debug lines 100): UI is always on top.
    [[nodiscard]] i32 order() const override { return 10000; }

    bool initialize(render::FeatureInitContext& ctx) override;
    void shutdown(rhi::Device& device) override;
    void setup(render::FeatureContext& ctx) override;

    [[nodiscard]] const std::shared_ptr<UiRenderBridge>& bridge() const { return m_bridge; }
    // GPU textures currently resident (tests).
    [[nodiscard]] usize residentTextures() const { return m_textures.size(); }

private:
    struct GpuTexture {
        rhi::TextureHandle handle;
        u32 index = 0;
        u64 version = 0;
        u32 width = 0, height = 0;
    };
    void syncTextures(rhi::Device& device);
    void publishInfo(render::FeatureContext& ctx);

    std::shared_ptr<UiRenderBridge> m_bridge;
    rhi::PipelineHandle m_pipeline;
    std::unordered_map<u32, GpuTexture> m_textures;
    u64 m_syncedVersion = ~0ull;
};

// Adds a UiOverlayFeature drawing `bridge` to `renderer` (replaces an existing "UI" feature).
UiOverlayFeature& attachRenderer(render::Renderer& renderer, std::shared_ptr<UiRenderBridge> bridge);

} // namespace ox::ui
