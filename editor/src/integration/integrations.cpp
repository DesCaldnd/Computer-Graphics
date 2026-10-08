#include "integration/integrations.hpp"

#include "content/registry_asset_backend.hpp"
#include "core/editor_context.hpp"
#include "integration/gameplay_inspector.hpp"
#include "integration/render_integration.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/scene/scene.hpp>

#if OX_EDITOR_HAS_GAMEPLAY
#include <oxwald/gameplay/gameplay.hpp>
#endif
#if OX_EDITOR_HAS_ASSETS
#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/assets/asset_registry.hpp>
#include <oxwald/assets/asset_types.hpp>
#endif
#if OX_EDITOR_HAS_RENDER
#include <oxwald/render/register_types.hpp>
#endif
#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/runtime/project.hpp>
#include <oxwald/runtime/save_game.hpp>
#include <oxwald/runtime/settings.hpp>
#endif

#include <QPointer>

namespace ox::editor {

void registerModuleTypes() {
    registerSceneTypes();
#if OX_EDITOR_HAS_GAMEPLAY
    registerGameplayTypes();
#endif
#if OX_EDITOR_HAS_ASSETS
    assets::registerAssetTypes();
#endif
#if OX_EDITOR_HAS_RUNTIME
    registerSettingsTypes();
    registerSaveGameTypes();
#endif
#if OX_EDITOR_HAS_RENDER
    render::registerRenderTypes(); // post-process/fog/cloud volumes, probes, particles, water, terrain render, ...
#endif
}

#if OX_EDITOR_HAS_GAMEPLAY
namespace {

// Play-in-editor through the gameplay module. With the Engine the gameplay systems are already registered (runtime
// GameplayModule); the fallback session gets them added here. Simulate runs physics/animation/world only.
class GameplayPlayRuntime final : public IPlayRuntime {
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("Gameplay"); }
    [[nodiscard]] bool simulatesPhysics() const override { return true; }

    void begin(World& world, SystemScheduler& scheduler, Services& services, PlayMode mode) override {
        if (!scheduler.find(gameplay::systems::kLifecycle)) addGameplaySystems(scheduler, services);
        m_scheduler = &scheduler;
        m_disabled.clear();
        if (mode != PlayMode::Simulate) return;
        using namespace gameplay::systems;
        for (std::string_view s : {kScriptPre, kScriptFixed, kScriptUpdate, kPerception, kBehaviorTrees, kNavigation,
                                   kNetPredict, kSplineFollowers, kNetPre, kNetPost}) {
            if (scheduler.find(s) && scheduler.isEnabled(s)) {
                scheduler.setEnabled(s, false);
                m_disabled.emplace_back(s);
            }
        }
        // Scripts never start in Simulate (the play world is a throw-away clone).
        for (auto [e, sc] : world.registry().view<gameplay::ScriptComponent>().each()) sc.enabled = false;
    }

    void end(World&, Services&) override {
        if (m_scheduler) {
            for (const auto& s : m_disabled) m_scheduler->setEnabled(s, true);
        }
        m_disabled.clear();
        m_scheduler = nullptr;
    }

private:
    SystemScheduler* m_scheduler = nullptr;
    std::vector<std::string> m_disabled;
};

} // namespace
#endif

namespace {

struct IntegrationState : QObject {
    QMetaObject::Connection started;
    QMetaObject::Connection stopping;
#if OX_EDITOR_HAS_RENDER
    std::unique_ptr<RenderIntegration> render;
#endif
};

IntegrationState* stateOf(EditorContext& ctx) {
    return static_cast<IntegrationState*>(ctx.findChild<QObject*>(QStringLiteral("ox.integrations"), Qt::FindDirectChildrenOnly));
}

void wireAssetBackend(EditorContext& ctx) {
#if OX_EDITOR_HAS_ASSETS
    Services* s = ctx.runtime().services();
    auto* registry = s ? s->tryGet<assets::AssetRegistry>() : nullptr;
    if (registry) {
        ctx.services().setAssetBackend(std::make_unique<RegistryAssetBackend>(*registry, s->tryGet<assets::AssetManager>()));
        return;
    }
#endif
    auto fs = std::make_unique<FileSystemAssetBackend>();
    if (ctx.project()) fs->setRootPath(ctx.project()->contentDir());
    ctx.services().setAssetBackend(std::move(fs));
}

} // namespace

void installIntegrations(EditorContext& ctx) {
    auto* state = new IntegrationState();
    state->setObjectName(QStringLiteral("ox.integrations"));
    state->setParent(&ctx);
#if OX_EDITOR_HAS_GAMEPLAY
    ctx.services().addPlayRuntime(std::make_shared<GameplayPlayRuntime>());
#endif
    static const bool extensions = [] {
        registerGameplayInspectorExtensions();
        registerGameplayTemplates();
        return true;
    }();
    (void)extensions;
    RuntimeHost* host = &ctx.runtime();
    QPointer<EditorContext> guard(&ctx);
    state->started = QObject::connect(host, &RuntimeHost::started, state, [guard] {
        if (guard) wireAssetBackend(*guard);
    });
    state->stopping = QObject::connect(host, &RuntimeHost::aboutToStop, state, [guard] {
        if (!guard) return;
        auto fs = std::make_unique<FileSystemAssetBackend>();
        if (guard->project()) fs->setRootPath(guard->project()->contentDir());
        guard->services().setAssetBackend(std::move(fs));
    });
#if OX_EDITOR_HAS_RENDER
    state->render = std::make_unique<RenderIntegration>(ctx);
#endif
}

void uninstallIntegrations(EditorContext& ctx) {
    if (IntegrationState* s = stateOf(ctx)) {
        QObject::disconnect(s->started);
        QObject::disconnect(s->stopping);
#if OX_EDITOR_HAS_RENDER
        s->render.reset();
#endif
        delete s;
    }
}

} // namespace ox::editor
