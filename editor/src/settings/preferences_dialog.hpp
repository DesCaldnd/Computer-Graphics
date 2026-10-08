#pragma once

#include "core/preferences.hpp"
#include "settings/settings_dialog.hpp"

namespace ox::editor {

class EditorContext;

// Editor Preferences (per user): appearance, viewport, autosave, keyboard shortcuts, source control & tools,
// performance. Every change is saved immediately to the user's EditorPreferences.json; Revert restores the
// values from when the window was opened.
class PreferencesDialog : public SettingsDialog {
    Q_OBJECT
public:
    PreferencesDialog(EditorContext* ctx, QWidget* parent = nullptr);

    // Binding to one preference field.
    SettingBinding pref(const QString& key, std::function<QVariant(const PreferenceValues&)> get,
                        std::function<void(PreferenceValues&, const QVariant&)> set);

protected:
    void applyChanges() override;
    void revertChanges() override;

private:
    void buildAppearance();
    void buildViewport();
    void buildAutosave();
    void buildShortcuts();
    void buildTools();
    void buildPerformance();

    EditorContext* m_ctx;
    PreferenceValues m_snapshot;
};

} // namespace ox::editor
