#pragma once

#include "core/common.hpp"

#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/value.hpp>

#include <QWidget>

#include <functional>
#include <string>
#include <vector>

namespace ox::editor {

class EditorContext;

inline constexpr const char* kMimeEntities = "application/x-oxwald-entities";
inline constexpr const char* kMimeAsset = "application/x-oxwald-asset";

// What a property editor edits: one reflected value (component + path) on one or more entities.
struct PropertyContext {
    EditorContext* editor = nullptr;
    UuidList entities;
    std::string component;
    std::string path; // relative to the component
    const reflect::TypeInfo* type = nullptr;
    reflect::Attributes attributes;
};

// Leaf editor for one reflected value. The grid feeds it the current value of every selected entity
// (setValues) and the editor reports edits through commit(): a sub-path ("" or a vector component "x") plus one
// value (applied to all entities) or one value per entity.
class PropertyEditor : public QWidget {
    Q_OBJECT
public:
    using CommitFn = std::function<void(const std::string& subPath, const std::vector<serial::Value>& values, EditPhase phase)>;

    explicit PropertyEditor(PropertyContext ctx, QWidget* parent = nullptr);
    void setCommit(CommitFn fn) { m_commit = std::move(fn); }
    virtual void setValues(const std::vector<serial::Value>& values) = 0;
    // True while the user interacts (refreshes from the world are skipped).
    [[nodiscard]] virtual bool isEditing() const { return false; }
    [[nodiscard]] const PropertyContext& context() const { return m_ctx; }

    // All values equal?
    static bool allEqual(const std::vector<serial::Value>& values);

protected:
    void commit(const std::vector<serial::Value>& values, EditPhase phase, const std::string& subPath = {}) {
        if (m_commit) m_commit(subPath, values, phase);
    }
    void commit(const serial::Value& value, EditPhase phase, const std::string& subPath = {}) {
        commit(std::vector<serial::Value>{value}, phase, subPath);
    }
    PropertyContext m_ctx;
    CommitFn m_commit;
};

// Registry of leaf editors. Custom editors (curves, gradients, layer masks, ...) register with a higher priority:
//   PropertyEditorFactory::registerEditor(100,
//       [](const reflect::TypeInfo& t, const reflect::Attributes& a) { return a.getMeta("editor") == "curve"; },
//       [](const PropertyContext& c, QWidget* p) -> PropertyEditor* { return new CurveEditor(c, p); });
class PropertyEditorFactory {
public:
    using Predicate = std::function<bool(const reflect::TypeInfo&, const reflect::Attributes&)>;
    using Creator = std::function<PropertyEditor*(const PropertyContext&, QWidget*)>;

    static void registerEditor(int priority, Predicate predicate, Creator creator);
    // nullptr for containers/structs (built by the component grid) and unsupported types.
    [[nodiscard]] static PropertyEditor* create(const PropertyContext& ctx, QWidget* parent);
    [[nodiscard]] static bool isLeaf(const reflect::TypeInfo& type, const reflect::Attributes& attrs);

private:
    static void registerBuiltins();
};

// Euler angles (degrees, XYZ = pitch/yaw/roll) <-> quaternion; display convenience only.
glm::vec3 quatToEulerDegrees(const glm::quat& q);
glm::quat eulerDegreesToQuat(const glm::vec3& e);

} // namespace ox::editor
