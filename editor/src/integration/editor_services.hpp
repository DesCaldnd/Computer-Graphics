#pragma once

#include "content/asset_backend.hpp"
#include "integration/rendering_caps.hpp"
#include "viewport/viewport_renderer.hpp"

#include <oxwald/core/services.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace ox {
class SystemScheduler;
class World;
} // namespace ox

namespace ox::editor {

enum class PlayMode { Play, Simulate };

// Gameplay/runtime hook for play-in-editor: adds the game's systems to the play world's scheduler.
// PlayMode::Simulate should only add simulation (physics, animation) systems — the scheduler additionally skips
// ISystem::playModeOnly() systems in that mode.
class IPlayRuntime {
public:
    virtual ~IPlayRuntime() = default;
    [[nodiscard]] virtual QString name() const = 0;
    virtual void begin(World& playWorld, SystemScheduler& scheduler, Services& services, PlayMode mode) = 0;
    virtual void end(World& playWorld, Services& services) {
        (void)playWorld;
        (void)services;
    }
};

// Pluggable back-ends of the editor. Defaults: host caps + heuristic benchmark + file-system assets + software
// viewport. Modules replace them as they become available (see docs/dev/modules/editor.md).
class EditorServices {
public:
    using ViewportRendererFactory = std::function<std::unique_ptr<IViewportRenderer>()>;

    EditorServices();
    ~EditorServices();

    [[nodiscard]] IRenderingCapsProvider& caps() { return *m_caps; }
    void setCapsProvider(std::unique_ptr<IRenderingCapsProvider> p) { m_caps = std::move(p); }
    [[nodiscard]] IQualityBenchmark& benchmark() { return *m_benchmark; }
    void setBenchmark(std::unique_ptr<IQualityBenchmark> b) { m_benchmark = std::move(b); }
    [[nodiscard]] IAssetBackend& assets() { return *m_assets; }
    void setAssetBackend(std::unique_ptr<IAssetBackend> a) { m_assets = std::move(a); }
    [[nodiscard]] IThumbnailRenderer* thumbnails() { return m_thumbnails.get(); }
    void setThumbnailRenderer(std::unique_ptr<IThumbnailRenderer> t) { m_thumbnails = std::move(t); }

    // Null factory = software viewport renderer.
    void setViewportRendererFactory(ViewportRendererFactory f) { m_viewportFactory = std::move(f); }
    [[nodiscard]] const ViewportRendererFactory& viewportRendererFactory() const { return m_viewportFactory; }

    void addPlayRuntime(std::shared_ptr<IPlayRuntime> r) { m_runtimes.push_back(std::move(r)); }
    [[nodiscard]] const std::vector<std::shared_ptr<IPlayRuntime>>& playRuntimes() const { return m_runtimes; }

    // Engine services shared with gameplay systems (DebugDraw, JobSystem, ...).
    [[nodiscard]] Services& engine() { return m_engine; }

private:
    std::unique_ptr<IRenderingCapsProvider> m_caps;
    std::unique_ptr<IQualityBenchmark> m_benchmark;
    std::unique_ptr<IAssetBackend> m_assets;
    std::unique_ptr<IThumbnailRenderer> m_thumbnails;
    ViewportRendererFactory m_viewportFactory;
    std::vector<std::shared_ptr<IPlayRuntime>> m_runtimes;
    Services m_engine;
};

} // namespace ox::editor
