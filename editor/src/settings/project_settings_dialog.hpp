#pragma once

#include "settings/settings_dialog.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace ox::editor {

class EditorContext;
class ScalabilityWidget;

// Project Settings: General, Maps & Modes, Rendering (RT/upscalers/AA/tonemapping), Scalability, Physics,
// Audio, Input, Networking, Scripting, Packaging. Values are cvars or project JSON; Apply writes them to
// Config/ProjectSettings.json, Revert restores the state from when the window opened.
class ProjectSettingsDialog : public SettingsDialog {
    Q_OBJECT
public:
    ProjectSettingsDialog(EditorContext* ctx, QWidget* parent = nullptr);
    [[nodiscard]] ScalabilityWidget* scalability() const { return m_scalability; }

protected:
    void applyChanges() override;
    void revertChanges() override;

private:
    void buildGeneral();
    void buildMaps();
    void buildRendering();
    void buildScalability();
    void buildPhysics();
    void buildAudio();
    void buildInput();
    void buildNetworking();
    void buildScripting();
    void buildPackaging();
    void captureSnapshot();

    EditorContext* m_ctx;
    Project* m_project;
    nlohmann::json m_settingsSnapshot;
    std::map<std::string, nlohmann::json> m_cvarSnapshot;
    ScalabilityWidget* m_scalability = nullptr;
};

} // namespace ox::editor
