#pragma once

#include "inspector/property_editors.hpp"

#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/value.hpp>

#include <QWidget>

#include <string>
#include <vector>

class QGridLayout;

namespace ox::editor {

class EditorContext;

// Reflection-driven grid for a standalone object (asset import settings, materials): the same leaf editors as the
// component inspector, editing an in-memory instance of `type`. Nested structs become groups, categories become
// sub-sections, arrays of numbers/strings are edited as comma separated text.
class ReflectedObjectEditor : public QWidget {
    Q_OBJECT
public:
    ReflectedObjectEditor(EditorContext* ctx, const reflect::TypeInfo& type, QWidget* parent = nullptr);
    ~ReflectedObjectEditor() override;

    void setValue(const serial::Value& value);
    [[nodiscard]] serial::Value value() const;
    [[nodiscard]] const reflect::TypeInfo& type() const { return m_type; }
    [[nodiscard]] PropertyEditor* editorForPath(const std::string& path) const;
    // Sets one field (path relative to the object) as if edited in the UI.
    bool setField(const std::string& path, const serial::Value& value);

Q_SIGNALS:
    void edited();

private:
    void build();
    void addStruct(const reflect::TypeInfo& type, const std::string& base, int depth);
    void addField(const QString& label, const reflect::TypeInfo& type, const reflect::Attributes& attrs, const std::string& path, int depth);
    void refreshEditors();

    EditorContext* m_ctx;
    const reflect::TypeInfo& m_type;
    void* m_object = nullptr;
    QGridLayout* m_grid = nullptr;
    int m_row = 0;
    struct Row {
        std::string path;
        PropertyEditor* editor = nullptr;
        std::function<void()> refresh;
    };
    std::vector<Row> m_rows;
};

} // namespace ox::editor
