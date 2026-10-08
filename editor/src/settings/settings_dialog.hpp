#pragma once

#include "core/common.hpp"

#include <QDialog>
#include <QUndoStack>
#include <QVariant>
#include <QWidget>

#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace ox::editor {

class ColorButton;
class NumberField;
class Project;
class SearchField;
class SettingsDialog;
class ToggleSwitch;

// How a settings row reads/writes its value (a cvar, a project JSON key, a preference, ...).
struct SettingBinding {
    QString key;
    std::function<QVariant()> get;
    std::function<void(const QVariant&)> set;
};

// Binds to a console variable by name (values written with CVarSource::Config).
SettingBinding cvarBinding(const QString& name);
// Binds to "section.key" in the project settings JSON.
SettingBinding projectBinding(Project* project, const QString& path);

// One page of a settings window: sections of rows (title + description on the left, control on the right).
class SettingsPage : public QWidget {
    Q_OBJECT
public:
    SettingsPage(QString id, QString title, QString icon, QString description, SettingsDialog* dialog);

    [[nodiscard]] const QString& id() const { return m_id; }
    [[nodiscard]] const QString& title() const { return m_title; }
    [[nodiscard]] const QString& icon() const { return m_icon; }
    [[nodiscard]] const QString& description() const { return m_description; }

    void addSection(const QString& title, const QString& description = {});
    QWidget* addRow(const QString& title, const QString& description, QWidget* control, const QString& key = {},
                    const QStringList& keywords = {});
    void addFullWidth(QWidget* w, const QStringList& keywords = {});
    QWidget* addBanner(const QString& text, bool warning, const QString& icon = {});

    ToggleSwitch* addToggle(const QString& title, const QString& description, const SettingBinding& b);
    QComboBox* addCombo(const QString& title, const QString& description, const QStringList& labels,
                        const QVariantList& values, const SettingBinding& b);
    NumberField* addNumber(const QString& title, const QString& description, const SettingBinding& b, double min,
                           double max, double step, int decimals = 2, const QString& suffix = {});
    QLineEdit* addText(const QString& title, const QString& description, const SettingBinding& b);
    ColorButton* addColor(const QString& title, const QString& description, const SettingBinding& b);
    QLineEdit* addPath(const QString& title, const QString& description, const SettingBinding& b, bool directory,
                       const QString& filter = {});

    // Hides rows not matching; returns the number of matching rows.
    int filter(const QString& text);
    void refresh();
    void addRefresher(std::function<void()> fn) { m_refreshers.push_back(std::move(fn)); }
    [[nodiscard]] SettingsDialog* dialog() const { return m_dialog; }

private:
    struct Row {
        QWidget* widget;
        QString text;
        QWidget* section; // section header the row belongs to
    };
    QString m_id, m_title, m_icon, m_description;
    SettingsDialog* m_dialog;
    QVBoxLayout* m_layout;
    QWidget* m_currentSection = nullptr;
    std::vector<Row> m_rows;
    std::vector<QWidget*> m_sections;
    std::vector<std::function<void()>> m_refreshers;
};

// Searchable two-pane settings window (UE "Project Settings"/"Editor Preferences" style). Every change goes
// through an in-dialog undo stack; Revert restores the state captured when the window opened.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const QString& title, const QString& subtitle, QWidget* parent = nullptr);

    void addGroup(const QString& name);
    void addPage(SettingsPage* page);
    [[nodiscard]] SettingsPage* page(const QString& id) const;
    void showPage(const QString& id);
    void setSearchText(const QString& text);
    [[nodiscard]] QUndoStack& undoStack() { return m_undo; }

    void commit(const SettingBinding& b, const QVariant& value, EditPhase phase = EditPhase::Single);
    void refreshAll();
    [[nodiscard]] bool isDirty() const { return m_dirty; }

    // Footer buttons (subclasses decide what Apply/Revert mean).
    QPushButton* applyButton() const { return m_apply; }
    QPushButton* revertButton() const { return m_revert; }

Q_SIGNALS:
    void settingChanged(const QString& key);

protected:
    virtual void applyChanges() {}
    virtual void revertChanges() {}
    void setDirty(bool d);
    void closeEvent(QCloseEvent* e) override;
    void reject() override;

    QUndoStack m_undo;

private:
    QListWidget* m_sidebar;
    SearchField* m_search;
    QStackedWidget* m_stack;
    QLabel* m_pageTitle;
    QLabel* m_pageDescription;
    QLabel* m_pageIcon;
    QPushButton* m_apply;
    QPushButton* m_revert;
    std::vector<SettingsPage*> m_pages;
    bool m_dirty = false;
};

} // namespace ox::editor
