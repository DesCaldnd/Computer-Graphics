#include "settings/preferences_dialog.hpp"

#include "core/editor_context.hpp"
#include "i18n/translator.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ox::editor {

PreferencesDialog::PreferencesDialog(EditorContext* ctx, QWidget* parent)
    : SettingsDialog(tr("Editor Preferences"), tr("Personal settings, stored for your user account only."), parent), m_ctx(ctx),
      m_snapshot(ctx->preferences().values()) {
    setObjectName(QStringLiteral("PreferencesDialog"));
    addGroup(tr("General"));
    buildAppearance();
    buildShortcuts();
    buildTools();
    addGroup(tr("Level Editor"));
    buildViewport();
    buildAutosave();
    buildPerformance();
    showPage(QStringLiteral("appearance"));
    applyButton()->setText(tr("Save"));
}

SettingBinding PreferencesDialog::pref(const QString& key, std::function<QVariant(const PreferenceValues&)> get,
                                      std::function<void(PreferenceValues&, const QVariant&)> set) {
    SettingBinding b;
    b.key = QStringLiteral("pref:") + key;
    EditorPreferences* prefs = &m_ctx->preferences();
    b.get = [prefs, get] { return get(prefs->values()); };
    const bool appearance = key.startsWith(QLatin1String("appearance."));
    b.set = [prefs, set, appearance](const QVariant& v) {
        prefs->modify([&](PreferenceValues& p) { set(p, v); });
        prefs->save();
        if (appearance) prefs->applyAppearance();
    };
    return b;
}

void PreferencesDialog::applyChanges() {
    m_ctx->preferences().save();
    m_snapshot = m_ctx->preferences().values();
}

void PreferencesDialog::revertChanges() {
    const QString lang = m_ctx->preferences().values().language;
    m_ctx->preferences().setValues(m_snapshot);
    m_ctx->preferences().save();
    if (lang != m_snapshot.language || true) m_ctx->preferences().applyAppearance();
}

void PreferencesDialog::buildAppearance() {
    auto* p = new SettingsPage(QStringLiteral("appearance"), tr("Appearance"), QStringLiteral("palette"), tr("Theme, accent colour, typography and language."), this);
    p->addSection(tr("Theme"));
    auto themeB = pref("appearance.theme", [](const auto& v) { return QVariant(v.theme == ThemeMode::Dark ? 0 : 1); },
                       [](auto& v, const QVariant& x) { v.theme = x.toInt() == 0 ? ThemeMode::Dark : ThemeMode::Light; });
    auto* seg = new SegmentedControl({tr("Dark"), tr("Light")}, p);
    seg->setFixedWidth(220);
    p->addRow(tr("Color Theme"), tr("Dark is easier on the eyes in dim rooms; Light suits bright environments."), seg, QStringLiteral("appearance.theme"));
    connect(seg, &SegmentedControl::activated, this, [this, themeB](int i) { commit(themeB, i); });
    p->addRefresher([seg, themeB] { seg->setCurrent(themeB.get().toInt()); });

    // accent swatches + custom
    auto accentB = pref("appearance.accent", [](const auto& v) { return QVariant(v.accent.name()); },
                        [](auto& v, const QVariant& x) { v.accent = QColor(x.toString()); });
    auto* sw = new QWidget(p);
    auto* sl = new QHBoxLayout(sw);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(6);
    const QStringList presets = {"#7C5CFF", "#4F7DFF", "#1FA2FF", "#16B88A", "#E5A00D", "#F2555A", "#E0559E", "#9AA3B5"};
    QVector<QPushButton*> chips;
    for (const QString& hex : presets) {
        auto* b = new QPushButton(sw);
        b->setFixedSize(24, 24);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(hex);
        b->setStyleSheet(QStringLiteral("QPushButton{background:%1;border-radius:12px;border:2px solid transparent;}"
                                        "QPushButton:hover{border-color:%2;}")
                             .arg(hex, cssColor(colors().text)));
        connect(b, &QPushButton::clicked, this, [this, accentB, hex] { commit(accentB, hex); });
        sl->addWidget(b);
        chips.push_back(b);
    }
    auto* custom = new ColorButton(sw);
    custom->setFixedWidth(96);
    connect(custom, &ColorButton::colorEdited, this, [this, accentB](const QColor& c, EditPhase ph) {
        if (ph == EditPhase::Single || ph == EditPhase::End) commit(accentB, c.name());
    });
    sl->addWidget(custom);
    p->addRow(tr("Accent Color"), tr("Used for selection, focus rings and primary buttons."), sw, QStringLiteral("appearance.accent"));
    p->addRefresher([custom, accentB, chips, presets] {
        custom->setColor(QColor(accentB.get().toString()));
        for (int i = 0; i < chips.size(); ++i) {
            const bool sel = QColor(presets[i]) == QColor(accentB.get().toString());
            chips[i]->setStyleSheet(QStringLiteral("QPushButton{background:%1;border-radius:12px;border:2px solid %2;}")
                                        .arg(presets[i], sel ? cssColor(colors().text) : QStringLiteral("transparent")));
        }
    });

    p->addSection(tr("Typography & Scale"));
    p->addNumber(tr("Font Size"), tr("Base UI font size in pixels."),
                 pref("appearance.fontSize", [](const auto& v) { return QVariant(v.fontSize); }, [](auto& v, const QVariant& x) { v.fontSize = x.toInt(); }),
                 10, 20, 1, 0, QStringLiteral(" px"));
    p->addCombo(tr("UI Scale"), tr("Scales the whole interface. Takes effect after restarting the editor."),
                {QStringLiteral("75%"), QStringLiteral("90%"), QStringLiteral("100%"), QStringLiteral("110%"), QStringLiteral("125%"), QStringLiteral("150%"), QStringLiteral("200%")},
                {0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 2.0},
                pref("ui.scale", [](const auto& v) { return QVariant(v.uiScale); }, [](auto& v, const QVariant& x) { v.uiScale = x.toDouble(); }));
    p->addSection(tr("Language"));
    QStringList langs;
    QVariantList codes;
    for (const QString& c : Translator::languages()) {
        langs << Translator::languageName(c);
        codes << c;
    }
    p->addCombo(tr("Editor Language"), tr("Interface language. Applied immediately."), langs, codes,
                pref("appearance.language", [](const auto& v) { return QVariant(v.language); }, [](auto& v, const QVariant& x) { v.language = x.toString(); }));
    p->addToggle(tr("Show Splash Screen"), tr("Show the OxwaldEngine splash while the editor starts."),
                 pref("misc.splash", [](const auto& v) { return QVariant(v.showSplash); }, [](auto& v, const QVariant& x) { v.showSplash = x.toBool(); }));
    addPage(p);
}

void PreferencesDialog::buildViewport() {
    auto* p = new SettingsPage(QStringLiteral("viewport"), tr("Viewport"), QStringLiteral("viewport"), tr("Camera navigation, grid, gizmos and selection."), this);
    auto num = [this](const QString& key, double PreferenceValues::*m) {
        return pref(key, [m](const auto& v) { return QVariant(v.*m); }, [m](auto& v, const QVariant& x) { v.*m = x.toDouble(); });
    };
    auto col = [this](const QString& key, QColor PreferenceValues::*m) {
        return pref(key, [m](const auto& v) { return QVariant((v.*m).name(QColor::HexArgb)); }, [m](auto& v, const QVariant& x) { v.*m = QColor(x.toString()); });
    };
    p->addSection(tr("Camera"));
    p->addNumber(tr("Camera Speed"), tr("Fly speed (hold the right mouse button + WASD). Scroll while flying to change it."), num("viewport.cameraSpeed", &PreferenceValues::cameraSpeed), 0.1, 200, 0.1, 1, QStringLiteral(" m/s"));
    p->addNumber(tr("Acceleration"), tr("How quickly flying ramps up while a key is held."), num("viewport.cameraAcceleration", &PreferenceValues::cameraAcceleration), 0, 20, 0.1, 1);
    p->addNumber(tr("Field of View"), tr("Vertical FOV of editor viewports."), num("viewport.fov", &PreferenceValues::cameraFov), 20, 120, 1, 0, QStringLiteral("°"));
    p->addNumber(tr("Mouse Sensitivity"), tr("Degrees per pixel when looking around."), num("viewport.sensitivity", &PreferenceValues::mouseSensitivity), 0.05, 2, 0.01, 2);
    p->addToggle(tr("Invert Mouse Y"), {}, pref("viewport.invertY", [](const auto& v) { return QVariant(v.invertY); }, [](auto& v, const QVariant& x) { v.invertY = x.toBool(); }));
    p->addSection(tr("Grid & Gizmos"));
    p->addNumber(tr("Grid Cell Size"), tr("Spacing of the ground grid lines."), num("viewport.gridSize", &PreferenceValues::gridSize), 0.05, 100, 0.05, 2, QStringLiteral(" m"));
    p->addColor(tr("Grid Color"), {}, col("viewport.gridColor", &PreferenceValues::gridColor));
    p->addNumber(tr("Gizmo Size"), tr("Screen size multiplier of transform gizmos."), num("viewport.gizmoSize", &PreferenceValues::gizmoSize), 0.3, 3, 0.05, 2);
    p->addColor(tr("Selection Outline"), tr("Colour of selected objects' outlines."), col("viewport.selectionColor", &PreferenceValues::selectionColor));
    p->addSection(tr("Snapping"), tr("Hold Ctrl while dragging a gizmo to invert the snapping toggle."));
    p->addNumber(tr("Translate Snap"), {}, num("viewport.translateSnap", &PreferenceValues::translateSnap), 0.001, 100, 0.01, 3, QStringLiteral(" m"));
    p->addNumber(tr("Rotate Snap"), {}, num("viewport.rotateSnap", &PreferenceValues::rotateSnap), 0.1, 180, 0.5, 1, QStringLiteral("°"));
    p->addNumber(tr("Scale Snap"), {}, num("viewport.scaleSnap", &PreferenceValues::scaleSnap), 0.001, 10, 0.01, 3);
    p->addSection(tr("Rendering Backend"));
    p->addCombo(tr("Viewport Backend"), tr("Auto uses the Vulkan renderer when available and falls back to the software preview. Applies to new viewports."),
                {tr("Auto"), tr("Software preview"), tr("Vulkan")}, {QStringLiteral("Auto"), QStringLiteral("Software"), QStringLiteral("Vulkan")},
                pref("viewport.backend", [](const auto& v) { return QVariant(v.viewportBackend); }, [](auto& v, const QVariant& x) { v.viewportBackend = x.toString(); }));
    addPage(p);
}

void PreferencesDialog::buildAutosave() {
    auto* p = new SettingsPage(QStringLiteral("autosave"), tr("Autosave"), QStringLiteral("clock"), tr("Automatic backups of the open scene into Saved/Autosaves."), this);
    p->addSection(tr("Autosave"));
    p->addToggle(tr("Enable Autosave"), {}, pref("autosave.enabled", [](const auto& v) { return QVariant(v.autosaveEnabled); }, [](auto& v, const QVariant& x) { v.autosaveEnabled = x.toBool(); }));
    p->addNumber(tr("Interval"), tr("Minutes between autosaves (only when the scene has unsaved changes)."),
                 pref("autosave.interval", [](const auto& v) { return QVariant(v.autosaveMinutes); }, [](auto& v, const QVariant& x) { v.autosaveMinutes = x.toInt(); }),
                 1, 120, 1, 0, QStringLiteral(" min"));
    p->addNumber(tr("Backups to Keep"), tr("Older autosaves of the same scene are deleted."),
                 pref("autosave.backups", [](const auto& v) { return QVariant(v.autosaveBackups); }, [](auto& v, const QVariant& x) { v.autosaveBackups = x.toInt(); }),
                 1, 100, 1, 0);
    addPage(p);
}

void PreferencesDialog::buildShortcuts() {
    auto* p = new SettingsPage(QStringLiteral("shortcuts"), tr("Keyboard Shortcuts"), QStringLiteral("keyboard"),
                               tr("Rebind editor commands. Conflicting shortcuts are highlighted."), this);
    auto* tree = new QTreeWidget(p);
    tree->setObjectName(QStringLiteral("ShortcutTree"));
    tree->setColumnCount(3);
    tree->setHeaderLabels({tr("Command"), tr("Shortcut"), QString()});
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree->header()->resizeSection(1, 220);
    tree->header()->resizeSection(2, 40);
    tree->setMinimumHeight(520);
    tree->setRootIsDecorated(true);
    tree->setIndentation(12);
    ActionRegistry& reg = m_ctx->actions();
    QMap<QString, QTreeWidgetItem*> cats;
    for (const auto& e : reg.entries()) {
        if (!e.action) continue;
        QTreeWidgetItem*& cat = cats[e.category];
        if (!cat) {
            cat = new QTreeWidgetItem(tree, {e.category});
            QFont f = cat->font(0);
            f.setWeight(QFont::DemiBold);
            cat->setFont(0, f);
            cat->setExpanded(true);
        }
        auto* it = new QTreeWidgetItem(cat, {e.action->text().remove(QLatin1Char('&')).remove(QStringLiteral("…"))});
        it->setIcon(0, e.action->icon());
        it->setToolTip(0, e.id);
        auto* edit = new QKeySequenceEdit(e.action->shortcut(), tree);
        edit->setMaximumSequenceLength(1);
        edit->setObjectName(QStringLiteral("shortcut:") + e.id);
        tree->setItemWidget(it, 1, edit);
        auto* reset = makeToolButton(QStringLiteral("refresh"), tr("Reset to default (%1)").arg(e.defaultShortcut.toString(QKeySequence::NativeText)), tree);
        tree->setItemWidget(it, 2, reset);
        const QString id = e.id;
        auto keyB = pref(QStringLiteral("shortcuts.") + id,
                         [id, &reg](const PreferenceValues& v) {
                             return QVariant(v.shortcuts.contains(id) ? v.shortcuts.value(id).toString(QKeySequence::PortableText)
                                                                      : reg.defaultShortcut(id).toString(QKeySequence::PortableText));
                         },
                         [id, &reg](PreferenceValues& v, const QVariant& x) {
                             const QKeySequence ks = QKeySequence::fromString(x.toString(), QKeySequence::PortableText);
                             if (ks == reg.defaultShortcut(id)) v.shortcuts.remove(id);
                             else v.shortcuts.insert(id, ks);
                         });
        connect(edit, &QKeySequenceEdit::editingFinished, this, [this, edit, keyB] { commit(keyB, edit->keySequence().toString(QKeySequence::PortableText)); });
        connect(reset, &QToolButton::clicked, this, [this, keyB, id, &reg] { commit(keyB, reg.defaultShortcut(id).toString(QKeySequence::PortableText)); });
        p->addRefresher([this, edit, keyB, id, it, &reg] {
            const QKeySequence ks = QKeySequence::fromString(keyB.get().toString(), QKeySequence::PortableText);
            if (edit->keySequence() != ks) edit->setKeySequence(ks);
            reg.applyOverrides(m_ctx->preferences().values().shortcuts);
            const QStringList c = reg.conflicts(id, ks);
            it->setForeground(0, c.isEmpty() ? colors().text : colors().error);
            it->setToolTip(1, c.isEmpty() ? QString() : tr("Conflicts with: %1").arg(c.join(QStringLiteral(", "))));
        });
    }
    if (reg.entries().isEmpty()) {
        p->addBanner(tr("Open the main window to edit its shortcuts."), false);
    }
    p->addFullWidth(tree, {tr("shortcut"), tr("key"), tr("hotkey"), tr("binding")});
    addPage(p);
}

void PreferencesDialog::buildTools() {
    auto* p = new SettingsPage(QStringLiteral("tools"), tr("Source Control & Tools"), QStringLiteral("source-control"), tr("Version control and external programs."), this);
    p->addSection(tr("Source Control"));
    p->addCombo(tr("Provider"), tr("Shows file status in the Content Browser and checks out files on save."), {tr("None"), QStringLiteral("Git"), QStringLiteral("Perforce")},
                {QStringLiteral("None"), QStringLiteral("Git"), QStringLiteral("Perforce")},
                pref("tools.sourceControl", [](const auto& v) { return QVariant(v.sourceControl); }, [](auto& v, const QVariant& x) { v.sourceControl = x.toString(); }));
    p->addSection(tr("External Code Editor"));
    p->addPath(tr("Code Editor"), tr("Program used to open scripts and shaders (VS Code, CLion, …). Empty = system default."),
               pref("tools.codeEditor", [](const auto& v) { return QVariant(v.codeEditorPath); }, [](auto& v, const QVariant& x) { v.codeEditorPath = x.toString(); }), false);
    p->addText(tr("Arguments"), tr("%f = file, %l = line."),
               pref("tools.codeEditorArgs", [](const auto& v) { return QVariant(v.codeEditorArgs); }, [](auto& v, const QVariant& x) { v.codeEditorArgs = x.toString(); }));
    addPage(p);
}

void PreferencesDialog::buildPerformance() {
    auto* p = new SettingsPage(QStringLiteral("performance"), tr("Performance"), QStringLiteral("speedometer"), tr("Keep the editor responsive and your laptop cool."), this);
    p->addSection(tr("Frame Rate"));
    p->addNumber(tr("Editor Frame Limit"), tr("Maximum viewport refresh rate (0 = unlimited)."),
                 pref("perf.frameLimit", [](const auto& v) { return QVariant(v.frameLimit); }, [](auto& v, const QVariant& x) { v.frameLimit = x.toInt(); }),
                 0, 480, 1, 0, QStringLiteral(" fps"));
    p->addToggle(tr("Throttle When Unfocused"), tr("Lower the viewport frame rate while another application is active."),
                 pref("perf.throttle", [](const auto& v) { return QVariant(v.throttleWhenUnfocused); }, [](auto& v, const QVariant& x) { v.throttleWhenUnfocused = x.toBool(); }));
    p->addNumber(tr("Unfocused Frame Rate"), {},
                 pref("perf.unfocusedFps", [](const auto& v) { return QVariant(v.unfocusedFps); }, [](auto& v, const QVariant& x) { v.unfocusedFps = x.toInt(); }),
                 1, 60, 1, 0, QStringLiteral(" fps"));
    addPage(p);
}

} // namespace ox::editor
