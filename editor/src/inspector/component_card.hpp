#pragma once

#include "core/common.hpp"
#include "inspector/property_editors.hpp"
#include "widgets/widgets.hpp"

#include <QGridLayout>
#include <QLabel>

#include <string>
#include <vector>

namespace ox {
struct ComponentInfo;
}

namespace ox::editor {

class EditorContext;

// One component of the selected entities as a collapsible card with a reflection-generated property grid.
// Structs become nested groups, arrays/maps/optionals get add/remove controls, field categories become
// sub-sections and prefab overrides are highlighted with a "Revert to Prefab" context action.
class ComponentCard : public CollapsibleSection {
    Q_OBJECT
public:
    ComponentCard(EditorContext* ctx, const ComponentInfo* info, UuidList entities, QWidget* parent = nullptr);

    // Re-reads values from the world; rebuilds rows when container sizes changed.
    void refresh();
    void setFilter(const QString& text);
    [[nodiscard]] const ComponentInfo* info() const { return m_info; }
    [[nodiscard]] PropertyEditor* editorForPath(const std::string& path) const;
    [[nodiscard]] int rowCount() const { return int(m_rows.size()); }

private:
    struct Row {
        std::string path;
        std::string overridePath;
        QLabel* label = nullptr;
        QWidget* field = nullptr;
        PropertyEditor* editor = nullptr; // null for group/container header rows
        QString searchText;
    };

    void build();
    void addStruct(const reflect::TypeInfo& type, const std::string& base, int depth);
    void addValue(const std::string& name, const QString& label, const reflect::TypeInfo& type,
                  const reflect::Attributes& attrs, const std::string& path, int depth);
    void addContainer(const QString& label, const reflect::TypeInfo& type, const reflect::Attributes& attrs,
                      const std::string& path, int depth);
    QLabel* makeLabel(const QString& text, const QString& tooltip, int depth, const std::string& path);
    void addRow(QLabel* label, QWidget* field, PropertyEditor* editor, const std::string& path, const QString& search);
    void addSpanningRow(QWidget* w);
    void updateOverrideMarkers();
    void showLabelMenu(const std::string& path, const QPoint& globalPos);
    [[nodiscard]] std::vector<serial::Value> readValues(const std::string& path) const;
    [[nodiscard]] std::string shapeSignature() const;
    // Applies a container mutation to a copy of each entity's value and commits it.
    void mutateContainer(const std::string& path, const reflect::TypeInfo& type,
                         const std::function<void(void* container)>& fn, const QString& what);

    EditorContext* m_ctx;
    const ComponentInfo* m_info;
    UuidList m_entities;
    QWidget* m_grid = nullptr;
    QGridLayout* m_layout = nullptr;
    int m_rowIndex = 0;
    std::vector<Row> m_rows;
    std::string m_shape;
    QString m_filter;
};

} // namespace ox::editor
