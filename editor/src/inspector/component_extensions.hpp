#pragma once

#include "core/common.hpp"

#include <QWidget>

#include <functional>
#include <string>
#include <vector>

namespace ox::editor {

class EditorContext;

// Extra UI appended to a component card (buttons, tool toggles, script properties). refresh() is called whenever
// the card refreshes from the world.
class ComponentExtensionWidget : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    virtual void refresh() {}
};

struct ComponentExtension {
    // Fields of the component the generic reflection grid does not show (the footer edits them instead).
    std::vector<std::string> hiddenFields;
    std::function<ComponentExtensionWidget*(EditorContext* ctx, const UuidList& entities, QWidget* parent)> footer;
};

// Registry keyed by ComponentRegistry name ("Script", "Collider", ...). Modules register at startup.
class ComponentExtensions {
public:
    static void add(const std::string& component, ComponentExtension ext);
    [[nodiscard]] static const ComponentExtension* find(const std::string& component);
};

} // namespace ox::editor
