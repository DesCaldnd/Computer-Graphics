#pragma once

#include <QDialog>

#include <memory>

class QLineEdit;
class QListWidget;
class QStackedWidget;

namespace ox::editor {

class EditorContext;
class Project;
class SegmentedControl;

// "Open or create a project" window: recent projects with thumbnails, new project from a template.
class ProjectBrowser : public QDialog {
    Q_OBJECT
public:
    explicit ProjectBrowser(EditorContext* ctx, QWidget* parent = nullptr);
    // The project the user opened/created (null when cancelled).
    std::unique_ptr<Project> takeProject() { return std::move(m_project); }
    void showCreatePage();

private:
    void openPath(const QString& file);
    void create();
    void populateRecent();

    EditorContext* m_ctx;
    QStackedWidget* m_stack;
    QListWidget* m_recent;
    QLineEdit* m_name;
    QLineEdit* m_location;
    SegmentedControl* m_tabs;
    QListWidget* m_templates;
    std::unique_ptr<Project> m_project;
};

} // namespace ox::editor
