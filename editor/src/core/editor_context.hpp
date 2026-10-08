#pragma once

#include "core/action_registry.hpp"
#include "core/commands.hpp"
#include "core/common.hpp"
#include "core/play_session.hpp"
#include "core/preferences.hpp"
#include "core/project.hpp"
#include "core/scene_templates.hpp"
#include "core/selection.hpp"
#include "integration/editor_services.hpp"

#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/serial/value.hpp>
#include <oxwald/scene/world.hpp>

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUndoStack>

#include <map>
#include <memory>
#include <optional>

namespace ox::editor {

struct PrefabAsset {
    QString path;
    serial::Document document;
};

// Owns the edited scene and everything around it: undo stack, selection, play session, project, preferences and
// pluggable services. Panels never mutate the World directly; they call the edit API below which records undo
// commands (or applies directly to the play world while playing) and emits change signals.
class EditorContext : public QObject, public ICommandHost {
    Q_OBJECT
public:
    explicit EditorContext(QObject* parent = nullptr);
    ~EditorContext() override;

    // Reflection + component registration of every linked module. Idempotent.
    static void registerEngineTypes();

    // ---- worlds ----
    [[nodiscard]] World& editWorld() { return *m_editWorld; }
    // The world the viewport/outliner/inspector show: the play world while playing.
    [[nodiscard]] World& world();
    [[nodiscard]] bool isPlaying() const { return m_play->active(); }
    [[nodiscard]] PlaySession& play() { return *m_play; }

    [[nodiscard]] QUndoStack& undoStack() { return m_undo; }
    [[nodiscard]] Selection& selection() { return m_selection; }
    [[nodiscard]] EditorServices& services() { return m_services; }
    [[nodiscard]] EditorPreferences& preferences() { return m_prefs; }
    [[nodiscard]] ActionRegistry& actions() { return m_actions; }
    [[nodiscard]] DebugDraw& debugDraw() { return m_services.engine().get<DebugDraw>(); }

    // ---- project ----
    [[nodiscard]] Project* project() const { return m_project.get(); }
    void setProject(std::unique_ptr<Project> project);

    // ---- scene document ----
    [[nodiscard]] QString scenePath() const { return m_scenePath; }
    [[nodiscard]] QString sceneName() const;
    [[nodiscard]] bool isDirty() const { return !m_undo.isClean() || m_extraDirty; }
    void markDirty();
    void newScene(bool withDefaults = true);
    bool openScene(const QString& path, QString* error = nullptr);
    // Empty path = current path. ".json" suffix saves the JSON form of the archive.
    bool saveScene(const QString& path = {}, QString* error = nullptr);
    bool autosave();

    // ---- entity editing (undoable) ----
    Uuid createEntity(CreateKind kind, const Uuid& parent = {});
    Uuid createEntity(const QString& name, const Uuid& parent = {});
    UuidList createEntities(const QString& undoText, CreateEntitiesCommand::Factory factory);
    void deleteEntities(const UuidList& ids);
    UuidList duplicateEntities(const UuidList& ids);
    void reparentEntities(const UuidList& ids, const Uuid& newParent, int index = -1);
    void renameEntity(const Uuid& id, const QString& name);
    void setActive(const UuidList& ids, bool active);
    // newValues has one value (applied to all) or one per entity.
    void setProperty(const UuidList& ids, const std::string& component, const std::string& path,
                     const std::vector<serial::Value>& newValues, EditPhase phase = EditPhase::Single);
    void setProperty(const UuidList& ids, const std::string& component, const std::string& path,
                     const serial::Value& value, EditPhase phase = EditPhase::Single) {
        setProperty(ids, component, path, std::vector<serial::Value>{value}, phase);
    }
    bool addComponent(const UuidList& ids, const std::string& component);
    void removeComponent(const UuidList& ids, const std::string& component);
    void resetComponent(const UuidList& ids, const std::string& component);

    // ---- clipboard ----
    void copy(const UuidList& ids);
    void cut(const UuidList& ids);
    UuidList paste(const Uuid& parent = {});
    [[nodiscard]] bool canPaste() const { return !m_clipboard.empty(); }

    // ---- prefabs ----
    // Saves the subtree as a prefab asset and links the subtree as its instance.
    bool createPrefab(const Uuid& root, const QString& path, QString* error = nullptr);
    const PrefabAsset* loadPrefab(const QString& path, QString* error = nullptr);
    [[nodiscard]] const PrefabAsset* prefabFor(const Uuid& prefabId) const;
    // Prefab document for the instance containing entity id (nullptr if not an instance / prefab not loaded).
    [[nodiscard]] const PrefabAsset* prefabOfEntity(const Uuid& id);
    Uuid instantiatePrefab(const QString& path, const Uuid& parent = {}, std::optional<glm::vec3> position = {});
    bool applyPrefab(const Uuid& instanceEntity, QString* error = nullptr);
    void revertPrefabOverride(const Uuid& entity, const std::string& propertyPath);
    void revertAllPrefabOverrides(const Uuid& instanceEntity);
    [[nodiscard]] bool isOverridden(const Uuid& entity, const std::string& propertyPath);

    // ---- editor-only entity state (outliner eye / lock) ----
    [[nodiscard]] bool isHidden(const Uuid& id) const { return m_hidden.contains(id); }
    [[nodiscard]] bool isLocked(const Uuid& id) const { return m_locked.contains(id); }
    void setHidden(const Uuid& id, bool hidden);
    void setLocked(const Uuid& id, bool locked);
    [[nodiscard]] UuidList hiddenIds() const;

    // ---- play ----
    bool startPlay(PlayMode mode);
    void stopPlay();

    // ICommandHost
    World& commandWorld() override { return *m_editWorld; }
    void onWorldEdited(EditKind kind) override;

Q_SIGNALS:
    void worldReset();        // a different World instance is shown (scene load, play start/stop)
    void structureChanged();  // entities created/destroyed/reparented/renamed, components added/removed
    void propertiesChanged(); // component values changed
    void sceneChanged();      // path, name or dirty state
    void projectChanged();
    void visibilityChanged();
    void playStateChanged();
    void statusMessage(const QString& text, int timeoutMs);

private:
    void setScenePath(const QString& path);
    void connectPlaySession();
    void resetWorld(std::unique_ptr<World> world);
    void updateAutosaveTimer();
    std::vector<SubtreeSnapshot> snapshotRoots(const UuidList& roots);

    EditorPreferences m_prefs;
    ActionRegistry m_actions;
    EditorServices m_services;
    std::unique_ptr<World> m_editWorld;
    std::unique_ptr<PlaySession> m_play;
    QUndoStack m_undo;
    Selection m_selection;
    std::unique_ptr<Project> m_project;
    QString m_scenePath;
    bool m_extraDirty = false;
    std::vector<std::byte> m_clipboard;
    std::map<Uuid, PrefabAsset> m_prefabs;
    QSet<Uuid> m_hidden;
    QSet<Uuid> m_locked;
    QTimer m_autosave;
    bool m_playWasActive = false;
};

} // namespace ox::editor
