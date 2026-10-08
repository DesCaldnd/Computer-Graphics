#include "core/editor_context.hpp"

#include "integration/integrations.hpp"
#include "settings/render_cvars.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace ox::editor {

void EditorContext::registerEngineTypes() {
    reflect::registerCoreReflection();
    registerSceneTypes();
    registerModuleTypes();
    ensureRenderingCVars();
    qRegisterMetaType<ox::Uuid>();
}

EditorContext::EditorContext(QObject* parent) : QObject(parent), m_runtime(std::make_unique<RuntimeHost>()) {
    registerEngineTypes();
    m_selection.setParent(nullptr);
    m_services.setRuntime(m_runtime.get());
    installIntegrations(*this);
    m_runtime->setQuitHandler([this] { stopPlay(); });
    connect(m_runtime.get(), &RuntimeHost::worldReplaced, this, &EditorContext::onEngineWorldReplaced);
    m_runtime->start(nullptr, nullptr);
    m_play = std::make_unique<PlaySession>(*m_runtime, m_services);
    connectPlaySession();
    connect(&m_selection, &Selection::changed, this, [this] {
        if (!m_selection.ids().empty()) inspectAsset({});
        if (m_tools.tool() != ViewportTool::Transform && m_selection.primary() != m_tools.target()) m_tools.reset();
    });
    connect(&m_undo, &QUndoStack::cleanChanged, this, [this] { Q_EMIT sceneChanged(); });
    connect(&m_prefs, &EditorPreferences::changed, this, &EditorContext::updateAutosaveTimer);
    connect(&m_autosave, &QTimer::timeout, this, [this] { autosave(); });
    m_undo.setUndoLimit(500);
    updateAutosaveTimer();
}

EditorContext::~EditorContext() {
    m_play->stop();
    m_undo.clear();
    m_play.reset();
    m_runtime->stop();
    uninstallIntegrations(*this);
}

Services& EditorContext::engineServices() {
    if (Services* s = m_runtime->services()) return *s;
    return m_services.engine();
}

void EditorContext::onEngineWorldReplaced() {
    // The engine swapped the edit world by itself (console "level", save game load): editor state is stale.
    m_undo.clear();
    m_selection.clear();
    m_hidden.clear();
    m_locked.clear();
    editWorld().updateTransforms();
    m_extraDirty = true;
    Q_EMIT worldReset();
    Q_EMIT structureChanged();
    Q_EMIT sceneChanged();
}

void EditorContext::connectPlaySession() {
    m_playWasActive = false;
    connect(m_play.get(), &PlaySession::stateChanged, this, [this] {
        const bool active = m_play->active();
        if (active != m_playWasActive) {
            m_playWasActive = active;
            Q_EMIT worldReset();
            Q_EMIT structureChanged();
        }
        Q_EMIT playStateChanged();
    });
    connect(m_play.get(), &PlaySession::ticked, this, &EditorContext::propertiesChanged);
}

World& EditorContext::world() {
    if (m_play->active() && m_play->world()) return *m_play->world();
    return editWorld();
}

void EditorContext::setProject(std::unique_ptr<Project> project) {
    m_play->stop();
    m_project = std::move(project);
    // The engine is recreated for the project (asset database, project settings, input mappings); the current edit
    // world moves over as a clone (same UUIDs, so selection and undo history stay valid).
    QString err;
    if (RuntimeHost::compiledIn() && !m_runtime->start(m_project.get(), editWorld().clone(), &err)) {
        Q_EMIT statusMessage(tr("Engine failed to start: %1").arg(err), 6000);
    }
    if (m_project) {
        m_services.assets().setRootPath(m_project->contentDir());
        m_project->applyCVarSettings();
    }
    m_prefabs.clear();
    editWorld().updateTransforms();
    Q_EMIT worldReset();
    Q_EMIT projectChanged();
}

QString EditorContext::sceneName() const {
    if (m_scenePath.isEmpty()) return tr("Untitled");
    QString n = QFileInfo(m_scenePath).fileName();
    const int dot = n.indexOf(QLatin1Char('.'));
    return dot > 0 ? n.left(dot) : n;
}

void EditorContext::markDirty() {
    if (m_extraDirty) return;
    m_extraDirty = true;
    Q_EMIT sceneChanged();
}

void EditorContext::setScenePath(const QString& path) {
    m_scenePath = path;
    Q_EMIT sceneChanged();
}

void EditorContext::resetWorld(std::unique_ptr<World> world, const QString& level) {
    m_play->stop();
    m_undo.clear();
    m_selection.clear();
    m_hidden.clear();
    m_locked.clear();
    m_runtime->setEditWorld(std::move(world), level);
    editWorld().updateTransforms();
    m_extraDirty = false;
    Q_EMIT worldReset();
    Q_EMIT structureChanged();
}

void EditorContext::newScene(bool withDefaults) {
    auto w = std::make_unique<World>();
    if (withDefaults) populateDefaultScene(*w);
    resetWorld(std::move(w));
    setScenePath({});
}

bool EditorContext::openScene(const QString& path, QString* error) {
    auto w = std::make_unique<World>();
    auto st = loadScene(*w, fsPath(path));
    if (!st) {
        if (error) *error = QString::fromStdString(st.error().message);
        OX_LOG_ERROR("editor", "cannot open scene {}: {}", path.toStdString(), st.error().message);
        return false;
    }
    // The level is recorded as a project:// URI when the scene lives in the project (save games, runtime loads).
    const bool inProject = m_project && QFileInfo(path).absoluteFilePath().startsWith(QDir(m_project->contentDir()).absolutePath() + QLatin1Char('/'));
    resetWorld(std::move(w), inProject ? m_project->uriForPath(QFileInfo(path).absoluteFilePath()) : path);
    setScenePath(path);
    OX_LOG_INFO("editor", "Opened scene {} ({} entities)", path.toStdString(), editWorld().entityCount());
    return true;
}

bool EditorContext::saveScene(const QString& path, QString* error) {
    const QString target = path.isEmpty() ? m_scenePath : path;
    if (target.isEmpty()) {
        if (error) *error = tr("No file name");
        return false;
    }
    QDir().mkpath(QFileInfo(target).absolutePath());
    auto st = ox::saveScene(editWorld(), fsPath(target));
    if (!st) {
        if (error) *error = QString::fromStdString(st.error().message);
        OX_LOG_ERROR("editor", "cannot save scene {}: {}", target.toStdString(), st.error().message);
        return false;
    }
    m_extraDirty = false;
    m_undo.setClean();
    setScenePath(target);
    OX_LOG_INFO("editor", "Saved scene {}", target.toStdString());
    Q_EMIT statusMessage(tr("Saved %1").arg(QFileInfo(target).fileName()), 3000);
    return true;
}

bool EditorContext::autosave() {
    if (!m_project || isPlaying() || !isDirty()) return false;
    const QString dir = QDir(m_project->savedDir()).filePath(QStringLiteral("Autosaves"));
    QDir().mkpath(dir);
    const QString file = QDir(dir).filePath(QStringLiteral("%1-%2.oxscene")
                                                .arg(sceneName(), QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
    if (!ox::saveScene(editWorld(), fsPath(file))) return false;
    QFileInfoList backups = QDir(dir).entryInfoList({sceneName() + QStringLiteral("-*.oxscene")}, QDir::Files, QDir::Time);
    for (int i = std::max(1, m_prefs.values().autosaveBackups); i < backups.size(); ++i) QFile::remove(backups[i].absoluteFilePath());
    Q_EMIT statusMessage(tr("Autosaved to %1").arg(QFileInfo(file).fileName()), 2500);
    return true;
}

void EditorContext::updateAutosaveTimer() {
    const auto& v = m_prefs.values();
    if (v.autosaveEnabled && v.autosaveMinutes > 0) m_autosave.start(v.autosaveMinutes * 60 * 1000);
    else m_autosave.stop();
}

void EditorContext::onWorldEdited(EditKind kind) {
    if (kind == EditKind::Structure) {
        World& w = world();
        m_selection.prune([&](const Uuid& id) { return bool(w.find(id)); });
        Q_EMIT structureChanged();
    }
    Q_EMIT propertiesChanged();
}

// ---- creation / deletion ------------------------------------------------------------------------------------

UuidList EditorContext::createEntities(const QString& undoText, CreateEntitiesCommand::Factory factory) {
    if (isPlaying()) {
        std::vector<Entity> made = factory(world());
        UuidList ids;
        for (Entity e : made) ids.push_back(e.uuid());
        onWorldEdited(EditKind::Structure);
        return ids;
    }
    auto* cmd = new CreateEntitiesCommand(*this, std::move(factory), undoText);
    m_undo.push(cmd);
    return cmd->createdIds();
}

Uuid EditorContext::createEntity(CreateKind kind, const Uuid& parent) {
    auto ids = createEntities(tr("Create %1").arg(createKindName(kind)), [kind, parent](World& w) {
        Entity p = parent.isNil() ? Entity{} : w.find(parent);
        return std::vector<Entity>{createPreset(w, kind, p)};
    });
    if (!ids.empty()) m_selection.select(ids.front());
    return ids.empty() ? Uuid{} : ids.front();
}

Uuid EditorContext::createEntity(const QString& name, const Uuid& parent) {
    const std::string n = name.toStdString();
    auto ids = createEntities(tr("Create %1").arg(name), [n, parent](World& w) {
        Entity p = parent.isNil() ? Entity{} : w.find(parent);
        return std::vector<Entity>{w.create(n, p)};
    });
    if (!ids.empty()) m_selection.select(ids.front());
    return ids.empty() ? Uuid{} : ids.front();
}

void EditorContext::deleteEntities(const UuidList& ids) {
    if (ids.empty()) return;
    if (isPlaying()) {
        for (const auto& id : topLevelOnly(world(), ids)) {
            if (Entity e = world().find(id)) world().destroyImmediate(e);
        }
        onWorldEdited(EditKind::Structure);
        return;
    }
    auto* cmd = new DeleteEntitiesCommand(*this, ids, ids.size() == 1 ? tr("Delete Entity") : tr("Delete %n Entities", nullptr, int(ids.size())));
    if (cmd->isEmpty()) {
        delete cmd;
        return;
    }
    m_undo.push(cmd);
}

UuidList EditorContext::duplicateEntities(const UuidList& ids) {
    World& w = world();
    UuidList top = topLevelOnly(w, ids);
    if (top.empty()) return {};
    std::vector<std::pair<std::vector<std::byte>, Uuid>> clips;
    for (const auto& id : top) {
        Entity e = w.find(id);
        Entity sel[] = {e};
        clips.emplace_back(copyEntities(w, sel), e.parent() ? e.parent().uuid() : Uuid{});
    }
    UuidList created = createEntities(top.size() == 1 ? tr("Duplicate") : tr("Duplicate %n Entities", nullptr, int(top.size())),
                                      [clips](World& world) {
                                          std::vector<Entity> out;
                                          for (const auto& [bytes, parent] : clips) {
                                              Entity p = parent.isNil() ? Entity{} : world.find(parent);
                                              if (auto r = pasteEntities(world, bytes, p)) {
                                                  for (Entity e : *r) {
                                                      e.setName(e.name()); // keep name; outliner shows duplicates
                                                      out.push_back(e);
                                                  }
                                              }
                                          }
                                          return out;
                                      });
    m_selection.set(created);
    return created;
}

void EditorContext::reparentEntities(const UuidList& ids, const Uuid& newParent, int index) {
    if (isPlaying()) {
        World& w = world();
        Entity np = newParent.isNil() ? Entity{} : w.find(newParent);
        for (const auto& id : topLevelOnly(w, ids)) {
            Entity e = w.find(id);
            if (e && (!np || !e.isAncestorOf(np)) && id != newParent) e.setParent(np, true, index);
        }
        onWorldEdited(EditKind::Structure);
        return;
    }
    auto* cmd = new ReparentCommand(*this, ids, newParent, index, tr("Reparent"));
    if (cmd->isEmpty()) {
        delete cmd;
        return;
    }
    m_undo.push(cmd);
}

void EditorContext::renameEntity(const Uuid& id, const QString& name) {
    setProperty({id}, "Name", "name", serial::Value::makeString(name.toStdString()));
}

void EditorContext::setActive(const UuidList& ids, bool active) {
    setProperty(ids, "Active", "active", serial::Value::makeBool(active));
}

void EditorContext::setProperty(const UuidList& ids, const std::string& component, const std::string& path,
                                const std::vector<serial::Value>& newValues, EditPhase phase) {
    if (ids.empty() || newValues.empty()) return;
    World& w = world();
    auto valueFor = [&](size_t i) -> const serial::Value& { return newValues.size() == ids.size() ? newValues[i] : newValues.front(); };
    if (isPlaying()) {
        for (size_t i = 0; i < ids.size(); ++i) props::write(w, ids[i], component, path, valueFor(i));
        onWorldEdited(component == "Name" ? EditKind::Structure : EditKind::Properties);
        return;
    }
    std::vector<SetPropertyCommand::Target> targets;
    bool changed = false;
    for (size_t i = 0; i < ids.size(); ++i) {
        auto before = props::read(w, ids[i], component, path);
        if (!before) continue;
        SetPropertyCommand::Target t;
        t.entity = ids[i];
        t.before = *before;
        t.after = valueFor(i);
        if (!(t.before == t.after)) changed = true;
        targets.push_back(std::move(t));
    }
    if (targets.empty()) return;
    if (!changed && phase == EditPhase::Single) return;
    const bool open = phase == EditPhase::Begin || phase == EditPhase::Update;
    QString label = component == "Name" ? tr("Rename") : tr("Edit %1").arg(prettifyName(path.empty() ? component : path.substr(0, path.find('.'))));
    m_undo.push(new SetPropertyCommand(*this, component, path, std::move(targets), open, label));
}

bool EditorContext::addComponent(const UuidList& ids, const std::string& component) {
    if (isPlaying()) {
        const ComponentInfo* info = ComponentRegistry::instance().find(component);
        if (!info) return false;
        for (const auto& id : ids) {
            if (Entity e = world().find(id)) info->add(world(), e.handle());
        }
        onWorldEdited(EditKind::Structure);
        return true;
    }
    auto* cmd = new ComponentCommand(*this, ComponentCommand::Op::Add, ids, component, std::nullopt,
                                     tr("Add %1").arg(prettifyName(component)));
    if (cmd->isEmpty()) {
        delete cmd;
        return false;
    }
    m_undo.push(cmd);
    return true;
}

void EditorContext::removeComponent(const UuidList& ids, const std::string& component) {
    if (isPlaying()) {
        const ComponentInfo* info = ComponentRegistry::instance().find(component);
        if (!info) return;
        for (const auto& id : ids) {
            if (Entity e = world().find(id)) info->remove(world(), e.handle());
        }
        onWorldEdited(EditKind::Structure);
        return;
    }
    auto* cmd = new ComponentCommand(*this, ComponentCommand::Op::Remove, ids, component, std::nullopt,
                                     tr("Remove %1").arg(prettifyName(component)));
    if (cmd->isEmpty()) {
        delete cmd;
        return;
    }
    m_undo.push(cmd);
}

void EditorContext::resetComponent(const UuidList& ids, const std::string& component) {
    const ComponentInfo* info = ComponentRegistry::instance().find(component);
    if (!info || !info->type || !info->type->construct) return;
    void* tmp = info->type->create();
    serial::Value def = serial::toValue(tmp, *info->type);
    info->type->destroy(tmp);
    setProperty(ids, component, "", def);
}

// ---- clipboard ----------------------------------------------------------------------------------------------

void EditorContext::copy(const UuidList& ids) {
    World& w = world();
    std::vector<Entity> sel;
    for (const auto& id : topLevelOnly(w, ids)) sel.push_back(w.find(id));
    if (sel.empty()) return;
    m_clipboard = copyEntities(w, sel);
    Q_EMIT statusMessage(tr("Copied %n entities", nullptr, int(sel.size())), 2000);
}

void EditorContext::cut(const UuidList& ids) {
    copy(ids);
    deleteEntities(ids);
}

UuidList EditorContext::paste(const Uuid& parent) {
    if (m_clipboard.empty()) return {};
    auto bytes = m_clipboard;
    UuidList created = createEntities(tr("Paste"), [bytes, parent](World& w) {
        Entity p = parent.isNil() ? Entity{} : w.find(parent);
        auto r = pasteEntities(w, bytes, p);
        return r ? *r : std::vector<Entity>{};
    });
    m_selection.set(created);
    return created;
}

// ---- prefabs ------------------------------------------------------------------------------------------------

std::vector<SubtreeSnapshot> EditorContext::snapshotRoots(const UuidList& roots) {
    std::vector<SubtreeSnapshot> out;
    for (const auto& id : roots) {
        if (Entity e = editWorld().find(id)) out.push_back(captureSubtree(editWorld(), e));
    }
    return out;
}

bool EditorContext::createPrefab(const Uuid& rootId, const QString& path, QString* error) {
    if (isPlaying()) {
        if (error) *error = tr("Stop play mode first");
        return false;
    }
    Entity root = editWorld().find(rootId);
    if (!root) {
        if (error) *error = tr("Nothing selected");
        return false;
    }
    auto before = snapshotRoots({rootId});
    serial::Document doc = ox::createPrefab(editWorld(), root);
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto st = serial::saveDocument(fsPath(path), doc, serial::formatForPath(fsPath(path)));
    if (!st) {
        if (error) *error = QString::fromStdString(st.error().message);
        return false;
    }
    m_prefabs[prefabId(doc)] = PrefabAsset{path, doc};
    auto after = snapshotRoots({rootId});
    m_undo.push(new ReplaceSubtreesCommand(*this, std::move(before), std::move(after), tr("Create Prefab")));
    OX_LOG_INFO("editor", "Created prefab {}", path.toStdString());
    return true;
}

const PrefabAsset* EditorContext::loadPrefab(const QString& path, QString* error) {
    for (const auto& [id, asset] : m_prefabs) {
        if (QFileInfo(asset.path) == QFileInfo(path)) return &asset;
    }
    // .oxprefab files or imported models (their prefab artifact) through the asset backend.
    QString err;
    auto doc = m_services.assets().loadPrefabDocument(path, &err);
    if (!doc) {
        if (error) *error = err;
        return nullptr;
    }
    if (doc->kind != "prefab") {
        if (error) *error = tr("%1 is not a prefab").arg(QFileInfo(path).fileName());
        return nullptr;
    }
    const Uuid id = prefabId(*doc);
    m_prefabs[id] = PrefabAsset{path, std::move(*doc)};
    return &m_prefabs[id];
}

const PrefabAsset* EditorContext::prefabFor(const Uuid& prefabId) const {
    auto it = m_prefabs.find(prefabId);
    return it == m_prefabs.end() ? nullptr : &it->second;
}

const PrefabAsset* EditorContext::prefabOfEntity(const Uuid& id) {
    Entity e = world().find(id);
    auto* pi = e.tryGet<PrefabInstanceComponent>();
    if (!pi) return nullptr;
    if (auto* a = prefabFor(pi->prefab)) return a;
    // Not loaded yet: look the prefab up through the asset backend.
    if (auto info = m_services.assets().find(pi->prefab)) return loadPrefab(info->path);
    if (m_project) {
        QDir content(m_project->contentDir());
        for (const QString& f : content.entryList({"*.oxprefab", "*.oxprefab.json"}, QDir::Files)) {
            if (auto* a = loadPrefab(content.filePath(f)); a && prefabId(a->document) == pi->prefab) return a;
        }
    }
    return nullptr;
}

Uuid EditorContext::instantiatePrefab(const QString& path, const Uuid& parent, std::optional<glm::vec3> position) {
    QString err;
    const PrefabAsset* asset = loadPrefab(path, &err);
    if (!asset) {
        Q_EMIT statusMessage(tr("Cannot instantiate prefab: %1").arg(err), 4000);
        return {};
    }
    serial::Document doc = asset->document;
    auto ids = createEntities(tr("Instantiate Prefab"), [doc, parent, position](World& w) {
        Entity p = parent.isNil() ? Entity{} : w.find(parent);
        auto r = ox::instantiatePrefab(w, doc, p);
        if (!r) return std::vector<Entity>{};
        if (position) r->setPosition(*position);
        return std::vector<Entity>{*r};
    });
    if (!ids.empty()) m_selection.select(ids.front());
    return ids.empty() ? Uuid{} : ids.front();
}

bool EditorContext::applyPrefab(const Uuid& instanceEntity, QString* error) {
    Entity e = editWorld().find(instanceEntity);
    Entity root = prefabInstanceRoot(e);
    const PrefabAsset* asset = prefabOfEntity(instanceEntity);
    if (!root || !asset) {
        if (error) *error = tr("The prefab asset of this instance was not found");
        return false;
    }
    serial::Document updated = applyInstanceToPrefab(editWorld(), root, asset->document);
    const QString path = asset->path;
    auto st = serial::saveDocument(fsPath(path), updated, serial::formatForPath(fsPath(path)));
    if (!st) {
        if (error) *error = QString::fromStdString(st.error().message);
        return false;
    }
    m_prefabs[prefabId(updated)] = PrefabAsset{path, updated};
    const usize n = updatePrefabInstances(editWorld(), updated);
    markDirty();
    onWorldEdited(EditKind::Structure);
    Q_EMIT statusMessage(tr("Applied prefab %1 (%2 instances updated)").arg(QFileInfo(path).fileName()).arg(n), 3000);
    return true;
}

void EditorContext::revertPrefabOverride(const Uuid& entity, const std::string& propertyPath) {
    Entity e = editWorld().find(entity);
    const PrefabAsset* asset = prefabOfEntity(entity);
    Entity root = prefabInstanceRoot(e);
    if (!e || !asset || !root) return;
    const Uuid rootId = root.uuid();
    auto before = snapshotRoots({rootId});
    revertOverride(e, propertyPath, asset->document);
    auto after = snapshotRoots({rootId});
    m_undo.push(new ReplaceSubtreesCommand(*this, std::move(before), std::move(after), tr("Revert to Prefab")));
}

void EditorContext::revertAllPrefabOverrides(const Uuid& instanceEntity) {
    Entity root = prefabInstanceRoot(editWorld().find(instanceEntity));
    const PrefabAsset* asset = prefabOfEntity(instanceEntity);
    if (!root || !asset) return;
    const Uuid rootId = root.uuid();
    auto before = snapshotRoots({rootId});
    revertAllOverrides(root, asset->document);
    auto after = snapshotRoots({rootId});
    m_undo.push(new ReplaceSubtreesCommand(*this, std::move(before), std::move(after), tr("Revert All Overrides")));
}

bool EditorContext::isOverridden(const Uuid& entity, const std::string& propertyPath) {
    Entity e = world().find(entity);
    return e && e.has<PrefabInstanceComponent>() && ox::isOverridden(e, propertyPath);
}

// ---- editor-only state --------------------------------------------------------------------------------------

void EditorContext::setHidden(const Uuid& id, bool hidden) {
    if (hidden == m_hidden.contains(id)) return;
    if (hidden) m_hidden.insert(id);
    else m_hidden.remove(id);
    Q_EMIT visibilityChanged();
}

void EditorContext::setLocked(const Uuid& id, bool locked) {
    if (locked == m_locked.contains(id)) return;
    if (locked) m_locked.insert(id);
    else m_locked.remove(id);
    Q_EMIT visibilityChanged();
}

void EditorContext::inspectAsset(const QString& path) {
    if (path == m_inspectedAsset) return;
    m_inspectedAsset = path;
    Q_EMIT assetInspected(path);
}

UuidList EditorContext::hiddenIds() const { return UuidList(m_hidden.begin(), m_hidden.end()); }

// ---- play ---------------------------------------------------------------------------------------------------

bool EditorContext::startPlay(PlayMode mode) { return m_play->start(mode); }

void EditorContext::stopPlay() {
    m_play->stop();
    World& w = editWorld();
    m_selection.prune([&](const Uuid& id) { return bool(w.find(id)); });
}

} // namespace ox::editor
