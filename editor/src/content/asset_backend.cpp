#include "content/asset_backend.hpp"

#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/scene_serializer.hpp>
#include <oxwald/scene/world.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <functional>

namespace ox::editor {

bool assetTypeMatches(const QString& assetType, const QString& wanted) {
    if (wanted.isEmpty() || assetType == wanted) return true;
    if (wanted == QLatin1String("AudioClip")) return assetType == QLatin1String("Audio");
    if (wanted == QLatin1String("AnimationClip")) return assetType == QLatin1String("Animation");
    if (wanted == QLatin1String("Prefab")) return assetType == QLatin1String("Model");
    return false;
}

QStringList IAssetBackend::importFiles(const QStringList& sources, const QString& folder, QString* error) {
    QStringList out;
    for (const QString& src : sources) {
        const QFileInfo fi(src);
        if (!fi.exists()) continue;
        const QString dest = FileSystemAssetBackend::uniquePath(folder, fi.completeBaseName(), fi.fileName().mid(fi.completeBaseName().size()));
        if (QFile::copy(src, dest)) out << dest;
        else if (error) *error = QStringLiteral("Cannot copy %1").arg(fi.fileName());
    }
    return out;
}

QList<AssetInfo> IAssetBackend::allOfType(const QString& type) const {
    QList<AssetInfo> out;
    std::function<void(const QString&, int)> walk = [&](const QString& folder, int depth) {
        if (depth > 12 || out.size() > 1000) return;
        for (const auto& a : list(folder)) {
            if (a.isFolder) walk(a.path, depth + 1);
            else if (assetTypeMatches(a.type, type)) out.push_back(a);
        }
    };
    walk(rootPath(), 0);
    return out;
}

std::optional<serial::Document> IAssetBackend::loadPrefabDocument(const QString& path, QString* error) const {
    auto doc = serial::loadDocument(path.toStdString());
    if (!doc) {
        if (error) *error = QString::fromStdString(doc.error().message);
        return std::nullopt;
    }
    return std::move(*doc);
}

Uuid IAssetBackend::resolveReference(const Uuid& id, const QString& assetType, const QString& wantedType) const {
    return assetTypeMatches(assetType, wantedType) ? id : Uuid{};
}

FileSystemAssetBackend::FileSystemAssetBackend(QString root) : m_root(std::move(root)) {}

QString FileSystemAssetBackend::typeForSuffix(const QString& fileName) {
    const QString n = fileName.toLower();
    auto ends = [&](const char* s) { return n.endsWith(QLatin1String(s)); };
    if (ends(".oxscene") || ends(".oxscene.json")) return QStringLiteral("Scene");
    if (ends(".oxprefab") || ends(".oxprefab.json")) return QStringLiteral("Prefab");
    if (ends(".oxmat") || ends(".oxmat.json") || ends(".material")) return QStringLiteral("Material");
    if (ends(".oxsave") || ends(".oxsave.json")) return QStringLiteral("SaveGame");
    if (ends(".gltf") || ends(".glb") || ends(".fbx") || ends(".obj") || ends(".dae") || ends(".oxmesh"))
        return QStringLiteral("Mesh");
    if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".tga") || ends(".ktx2") || ends(".ktx") || ends(".exr") ||
        ends(".hdr") || ends(".dds") || ends(".bmp"))
        return QStringLiteral("Texture");
    if (ends(".wav") || ends(".ogg") || ends(".mp3") || ends(".flac")) return QStringLiteral("Audio");
    if (ends(".lua")) return QStringLiteral("Script");
    if (ends(".glsl") || ends(".vert") || ends(".frag") || ends(".comp")) return QStringLiteral("Shader");
    if (ends(".ttf") || ends(".otf")) return QStringLiteral("Font");
    if (ends(".oxanim") || ends(".anim")) return QStringLiteral("Animation");
    if (ends(".oxbt")) return QStringLiteral("BehaviorTree");
    if (ends(".oxanimctrl")) return QStringLiteral("AnimatorController");
    if (ends(".oxcube")) return QStringLiteral("Texture");
    if (ends(".oxnav") || ends(".navmesh")) return QStringLiteral("NavMesh");
    if (ends(".r16") || ends(".r32") || ends(".raw")) return QStringLiteral("Heightmap");
    return QStringLiteral("Data");
}

QString FileSystemAssetBackend::typeForPath(const QString& path) const {
    if (QFileInfo(path).isDir()) return QStringLiteral("Folder");
    return typeForSuffix(QFileInfo(path).fileName());
}

std::optional<AssetInfo> FileSystemAssetBackend::info(const QString& path) const {
    QFileInfo fi(path);
    if (!fi.exists()) return std::nullopt;
    AssetInfo a;
    a.path = fi.absoluteFilePath();
    a.relativePath = m_root.isEmpty() ? fi.fileName() : QDir(m_root).relativeFilePath(a.path);
    a.isFolder = fi.isDir();
    a.type = typeForPath(a.path);
    a.name = fi.fileName();
    if (!a.isFolder) {
        // strip compound suffixes: "level.oxscene.json" -> "level"
        const int dot = a.name.indexOf(QLatin1Char('.'));
        if (dot > 0) a.name = a.name.left(dot);
    }
    a.size = fi.size();
    a.modified = fi.lastModified();
    // .meta sidecar written by the assets module ({"uuid": "..."}); fall back to a path-derived id.
    QFile meta(a.path + QStringLiteral(".meta"));
    if (meta.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(meta.readAll());
        if (auto id = Uuid::parse(doc.object().value(QStringLiteral("uuid")).toString().toStdString())) a.uuid = *id;
    }
    if (a.uuid.isNil()) a.uuid = Uuid::fromName(a.relativePath.toStdString());
    return a;
}

QList<AssetInfo> FileSystemAssetBackend::list(const QString& folder) const {
    QList<AssetInfo> folders, files;
    QDir dir(folder.isEmpty() ? m_root : folder);
    for (const QFileInfo& fi : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        if (fi.fileName().endsWith(QLatin1String(".meta")) || fi.fileName().startsWith(QLatin1Char('.'))) continue;
        if (auto a = info(fi.absoluteFilePath())) (a->isFolder ? folders : files).push_back(*a);
    }
    return folders + files;
}

std::optional<AssetInfo> FileSystemAssetBackend::find(const Uuid& id) const {
    if (m_root.isEmpty() || id.isNil()) return std::nullopt;
    QDirIterator it(m_root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        if (p.endsWith(QLatin1String(".meta"))) continue;
        auto a = info(p);
        if (a && a->uuid == id) return a;
    }
    return std::nullopt;
}

QStringList FileSystemAssetBackend::creatableTypes() const {
    return {QStringLiteral("Scene"), QStringLiteral("Prefab"), QStringLiteral("Material"), QStringLiteral("Script"),
            QStringLiteral("BehaviorTree")};
}

QString FileSystemAssetBackend::uniquePath(const QString& folder, const QString& baseName, const QString& suffix) {
    QDir dir(folder);
    QString candidate = dir.filePath(baseName + suffix);
    for (int i = 1; QFileInfo::exists(candidate); ++i) candidate = dir.filePath(QStringLiteral("%1 %2%3").arg(baseName).arg(i).arg(suffix));
    return candidate;
}

QString FileSystemAssetBackend::createFolder(const QString& parent, const QString& name, QString* error) {
    const QString path = uniquePath(parent.isEmpty() ? m_root : parent, name, {});
    if (!QDir().mkpath(path)) {
        if (error) *error = QStringLiteral("Cannot create folder %1").arg(path);
        return {};
    }
    return path;
}

QString FileSystemAssetBackend::writeNewAsset(const QString& dir, const QString& type, const QString& name, QString* error) {
    QString path;
    bool ok = false;
    auto writeText = [&](const QByteArray& text) {
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly) && f.write(text) == text.size();
    };
    if (type == QLatin1String("Scene")) {
        path = uniquePath(dir, name, QStringLiteral(".oxscene"));
        World empty;
        ok = saveScene(empty, path.toStdString()).hasValue();
    } else if (type == QLatin1String("Prefab")) {
        path = uniquePath(dir, name, QStringLiteral(".oxprefab"));
        World w;
        Entity root = w.create(name.toStdString());
        auto doc = createPrefab(w, root, {.linkSource = false});
        ok = serial::saveDocument(path.toStdString(), doc, serial::Format::Binary).hasValue();
    } else if (type == QLatin1String("Material")) {
        // Runtime material format (assets MaterialAsset as plain JSON, see docs/dev/modules/assets.md).
        path = uniquePath(dir, name, QStringLiteral(".oxmat"));
        writeText(R"({
  "oxmat": 1,
  "shadingModel": "Lit",
  "blendMode": "Opaque",
  "baseColor": [0.8, 0.8, 0.8, 1.0],
  "metallic": 0.0,
  "roughness": 0.5,
  "emissive": [0.0, 0.0, 0.0],
  "emissiveStrength": 1.0
}
)");
    } else if (type == QLatin1String("Script")) {
        path = uniquePath(dir, name, QStringLiteral(".lua"));
        writeText(QString::fromUtf8(R"LUA(-- %1.lua
-- Declared properties show up in the inspector of the Script component and can be overridden per entity.
properties = {
    speed = { type = "float", default = 1.5, min = 0, max = 20, tooltip = "Turns per second (radians)" },
    spin = { type = "bool", default = true, tooltip = "Rotate around Y while playing" },
}

function onStart(self)
    log.info("%1 started on", self.entity.name)
end

function onUpdate(self, dt)
    if self.spin then
        self.entity.transform:rotate(vec3(0, 1, 0), self.speed * dt)
    end
end

-- Other callbacks: onCreate, onFixedUpdate(self, dt), onDestroy, onTriggerEnter(self, other),
-- onCollisionEnter(self, other, info), onEvent(self, name, payload). Coroutines: spawn(function() await(...) end)
)LUA")
                      .arg(name)
                      .toUtf8());
    } else if (type == QLatin1String("BehaviorTree")) {
        path = uniquePath(dir, name, QStringLiteral(".oxbt"));
        writeText(R"({
  "root": {
    "type": "Sequence",
    "name": "Patrol",
    "children": [
      { "type": "Wait", "name": "Idle", "seconds": 1.0 },
      { "type": "SetBlackboard", "name": "Mark", "key": "visited", "value": true }
    ]
  }
}
)");
    } else {
        if (error) *error = QStringLiteral("Unknown asset type %1").arg(type);
        return {};
    }
    if (!ok) {
        if (error) *error = QStringLiteral("Cannot write %1").arg(path);
        return {};
    }
    return path;
}

QString FileSystemAssetBackend::createAsset(const QString& folder, const QString& type, const QString& name, QString* error) {
    return writeNewAsset(folder.isEmpty() ? m_root : folder, type, name, error);
}

QString FileSystemAssetBackend::rename(const QString& path, const QString& newName, QString* error) {
    QFileInfo fi(path);
    QString suffix;
    if (!fi.isDir()) {
        const QString file = fi.fileName();
        const int dot = file.indexOf(QLatin1Char('.'));
        if (dot > 0) suffix = file.mid(dot);
    }
    const QString target = fi.dir().filePath(newName + suffix);
    if (QFileInfo::exists(target)) {
        if (error) *error = QStringLiteral("%1 already exists").arg(newName + suffix);
        return {};
    }
    if (!QFile::rename(path, target)) {
        if (error) *error = QStringLiteral("Cannot rename %1").arg(fi.fileName());
        return {};
    }
    if (QFile::exists(path + QStringLiteral(".meta"))) QFile::rename(path + QStringLiteral(".meta"), target + QStringLiteral(".meta"));
    return target;
}

bool FileSystemAssetBackend::remove(const QString& path, QString* error) {
    QFileInfo fi(path);
    bool ok = fi.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
    QFile::remove(path + QStringLiteral(".meta"));
    if (!ok && error) *error = QStringLiteral("Cannot delete %1").arg(fi.fileName());
    return ok;
}

QString FileSystemAssetBackend::move(const QString& path, const QString& destinationFolder, QString* error) {
    QFileInfo fi(path);
    const QString target = QDir(destinationFolder).filePath(fi.fileName());
    if (QDir::cleanPath(target) == QDir::cleanPath(path)) return path;
    if (fi.isDir() && QDir::cleanPath(destinationFolder).startsWith(QDir::cleanPath(path) + QLatin1Char('/'))) {
        if (error) *error = QStringLiteral("Cannot move a folder into itself");
        return {};
    }
    if (QFileInfo::exists(target)) {
        if (error) *error = QStringLiteral("%1 already exists in the destination").arg(fi.fileName());
        return {};
    }
    if (!QFile::rename(path, target) && !QDir().rename(path, target)) {
        if (error) *error = QStringLiteral("Cannot move %1").arg(fi.fileName());
        return {};
    }
    if (QFile::exists(path + QStringLiteral(".meta"))) QFile::rename(path + QStringLiteral(".meta"), target + QStringLiteral(".meta"));
    return target;
}

} // namespace ox::editor
