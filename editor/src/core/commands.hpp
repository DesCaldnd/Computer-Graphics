#pragma once

#include "core/common.hpp"

#include <oxwald/core/math.hpp>
#include <oxwald/core/serial/value.hpp>
#include <oxwald/scene/world.hpp>

#include <QUndoCommand>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Undoable editor operations. Everything is keyed by entity UUID (entt handles change when an entity is
// re-created by undo) and goes through reflection (ComponentRegistry + reflect::ValueRef paths) so commands work
// for components of any module without per-type code.
namespace ox::editor {

enum class EditKind { Structure, Properties };

class ICommandHost {
public:
    virtual ~ICommandHost() = default;
    virtual World& commandWorld() = 0;
    virtual void onWorldEdited(EditKind kind) = 0;
};

// Generic property access: component by registered name, path relative to the component ("" = whole component,
// "position", "position.x", "materials[1]", ...).
namespace props {
[[nodiscard]] std::optional<serial::Value> read(World& world, const Uuid& entity, const std::string& component,
                                                const std::string& path);
bool write(World& world, const Uuid& entity, const std::string& component, const std::string& path,
           const serial::Value& value);
// "Light.intensity" style override path recorded on prefab instances for an edit of component/path.
[[nodiscard]] std::string overridePath(const std::string& component, const std::string& path);
} // namespace props

enum CommandId { kSetPropertyId = 0x0E01 };

class SetPropertyCommand final : public QUndoCommand {
public:
    struct Target {
        Uuid entity;
        serial::Value before;
        serial::Value after;
        std::optional<std::vector<std::string>> overridesBefore; // prefab instance overrides
    };

    // open = part of a continuous edit (slider/gizmo drag): later commands with the same targets merge into it
    // until one with open = false arrives.
    SetPropertyCommand(ICommandHost& host, std::string component, std::string path, std::vector<Target> targets,
                       bool open, const QString& text, QUndoCommand* parent = nullptr);

    void redo() override;
    void undo() override;
    [[nodiscard]] int id() const override { return kSetPropertyId; }
    bool mergeWith(const QUndoCommand* other) override;

    [[nodiscard]] const std::string& component() const { return m_component; }
    [[nodiscard]] const std::string& path() const { return m_path; }
    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] const std::vector<Target>& targets() const { return m_targets; }

private:
    void apply(bool forward);
    ICommandHost& m_host;
    std::string m_component;
    std::string m_path;
    std::vector<Target> m_targets;
    bool m_open;
};

// Serialized subtree used to delete/re-create entities with their original UUIDs and sibling position.
struct SubtreeSnapshot {
    Uuid root;
    Uuid parent;
    i32 index = -1;
    serial::Value entities;
};
SubtreeSnapshot captureSubtree(World& world, Entity root);
Entity restoreSubtree(World& world, const SubtreeSnapshot& snapshot);
// Removes entities whose ancestor is also in the list (subtrees are implied).
UuidList topLevelOnly(World& world, const UuidList& ids);

// Creation through a factory run on the first redo (create, duplicate, paste, prefab instantiate, asset drop);
// later redos restore the captured subtrees with the same UUIDs.
class CreateEntitiesCommand final : public QUndoCommand {
public:
    using Factory = std::function<std::vector<Entity>(World&)>;
    CreateEntitiesCommand(ICommandHost& host, Factory factory, const QString& text, QUndoCommand* parent = nullptr);
    void redo() override;
    void undo() override;
    [[nodiscard]] const UuidList& createdIds() const { return m_created; }

private:
    ICommandHost& m_host;
    Factory m_factory;
    std::vector<SubtreeSnapshot> m_snapshots;
    UuidList m_created;
    bool m_done = false;
};

class DeleteEntitiesCommand final : public QUndoCommand {
public:
    DeleteEntitiesCommand(ICommandHost& host, const UuidList& ids, const QString& text, QUndoCommand* parent = nullptr);
    void redo() override;
    void undo() override;
    [[nodiscard]] bool isEmpty() const { return m_snapshots.empty(); }

private:
    ICommandHost& m_host;
    std::vector<SubtreeSnapshot> m_snapshots;
};

// Replaces whole subtrees with captured "after" states (redo) or "before" states (undo). Used for operations that
// mutate entities through engine helpers (create/revert/apply prefab): capture before, run, capture after.
class ReplaceSubtreesCommand final : public QUndoCommand {
public:
    ReplaceSubtreesCommand(ICommandHost& host, std::vector<SubtreeSnapshot> before, std::vector<SubtreeSnapshot> after,
                           const QString& text, QUndoCommand* parent = nullptr);
    void redo() override;
    void undo() override;

private:
    void swapTo(const std::vector<SubtreeSnapshot>& from, const std::vector<SubtreeSnapshot>& to);
    ICommandHost& m_host;
    std::vector<SubtreeSnapshot> m_before;
    std::vector<SubtreeSnapshot> m_after;
    bool m_first = true; // the change was already applied when the command is pushed
};

class ReparentCommand final : public QUndoCommand {
public:
    // index < 0 appends. Entities that would create a cycle are skipped.
    ReparentCommand(ICommandHost& host, const UuidList& ids, const Uuid& newParent, i32 index, const QString& text,
                    QUndoCommand* parent = nullptr);
    void redo() override;
    void undo() override;
    [[nodiscard]] bool isEmpty() const { return m_items.empty(); }

private:
    struct Item {
        Uuid id;
        Uuid oldParent;
        i32 oldIndex = -1;
        Transform oldLocal;
    };
    ICommandHost& m_host;
    std::vector<Item> m_items;
    Uuid m_newParent;
    i32 m_index;
};

class ComponentCommand final : public QUndoCommand {
public:
    enum class Op { Add, Remove };
    // Add: optional initial value (serialized component). Remove: snapshots the component for undo.
    ComponentCommand(ICommandHost& host, Op op, const UuidList& ids, std::string component,
                     std::optional<serial::Value> initial, const QString& text, QUndoCommand* parent = nullptr);
    void redo() override;
    void undo() override;
    [[nodiscard]] bool isEmpty() const { return m_items.empty(); }

private:
    struct Item {
        Uuid id;
        serial::Value snapshot;
    };
    ICommandHost& m_host;
    Op m_op;
    std::string m_component;
    std::optional<serial::Value> m_initial;
    std::vector<Item> m_items;
};

} // namespace ox::editor
