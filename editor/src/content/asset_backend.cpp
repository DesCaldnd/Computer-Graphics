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

namespace ox::editor {

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
    return {QStringLiteral("Scene"), QStringLiteral("Prefab"), QStringLiteral("Material"), QStringLiteral("Script")};
}

QString FileSystemAssetBackend::uniquePath(const QString& folder, const QString& baseName, const QString& suffix) const {
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

QString FileSystemAssetBackend::createAsset(const QString& folder, const QString& type, const QString& name, QString* error) {
    const QString dir = folder.isEmpty() ? m_root : folder;
    QString path;
    bool ok = false;
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
        path = uniquePath(dir, name, QStringLiteral(".oxmat.json"));
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly);
        if (ok) {
            f.write(R"({
  "kind": "material",
  "shadingModel": "DefaultLit",
  "baseColor": [0.8, 0.8, 0.8, 1.0],
  "metallic": 0.0,
  "roughness": 0.5,
  "emissive": [0.0, 0.0, 0.0]
}
)");
        }
    } else if (type == QLatin1String("Script")) {
        path = uniquePath(dir, name, QStringLiteral(".lua"));
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly);
        if (ok) {
            f.write(QStringLiteral("-- %1.lua\nlocal M = {}\n\nfunction M:onStart()\nend\n\nfunction M:onUpdate(dt)\nend\n\nreturn M\n")
                        .arg(name)
                        .toUtf8());
        }
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
