#include "content/registry_asset_backend.hpp"

#if OX_EDITOR_HAS_ASSETS

#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/assets/asset_registry.hpp>
#include <oxwald/assets/texture.hpp>
#include <oxwald/assets/texture_import.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/serial/format.hpp>

#include <glm/gtc/packing.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace ox::editor {

namespace {

using assets::AssetType;

QString stripExtensions(const QString& fileName) {
    const int dot = fileName.indexOf(QLatin1Char('.'));
    return dot > 0 ? fileName.left(dot) : fileName;
}

QString allExtensions(const QString& fileName) {
    const int dot = fileName.indexOf(QLatin1Char('.'));
    return dot > 0 ? fileName.mid(dot) : QString();
}

nlohmann::json toJson(const nlohmann::ordered_json& j) { return nlohmann::json::parse(j.dump()); }

QString settingsTypeFor(const QString& importer) {
    if (importer == QLatin1String("texture") || importer == QLatin1String("cubemap")) return QStringLiteral("TextureImportSettings");
    if (importer == QLatin1String("model")) return QStringLiteral("ModelImportSettings");
    if (importer == QLatin1String("heightmap")) return QStringLiteral("HeightmapImportSettings");
    return {};
}

u8 toByte(float v) { return u8(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
float toDisplay(float linear) { return std::pow(std::max(0.0f, linear) / (1.0f + std::max(0.0f, linear)) * 2.0f, 1.0f / 2.2f); }

} // namespace

RegistryAssetBackend::RegistryAssetBackend(assets::AssetRegistry& registry, assets::AssetManager* manager)
    : m_registry(registry), m_manager(manager),
      m_root(QDir::cleanPath(QString::fromStdString(registry.assetsDir().string()))) {}

QString RegistryAssetBackend::editorTypeName(int assetType, const QString& importer, const QString& kind) {
    if (importer == QLatin1String("model")) return QStringLiteral("Model");
    switch (AssetType(assetType)) {
    case AssetType::Mesh: return QStringLiteral("Mesh");
    case AssetType::Texture: return QStringLiteral("Texture");
    case AssetType::Material: return QStringLiteral("Material");
    case AssetType::Scene: return QStringLiteral("Scene");
    case AssetType::Prefab: return QStringLiteral("Prefab");
    case AssetType::Script: return QStringLiteral("Script");
    case AssetType::Audio: return QStringLiteral("Audio");
    case AssetType::AnimationClip: return QStringLiteral("Animation");
    case AssetType::Skeleton: return QStringLiteral("Skeleton");
    case AssetType::Font: return QStringLiteral("Font");
    case AssetType::NavMesh: return QStringLiteral("NavMesh");
    case AssetType::Heightmap: return QStringLiteral("Heightmap");
    case AssetType::Raw:
        if (kind == QLatin1String("BehaviorTree")) return QStringLiteral("BehaviorTree");
        if (kind == QLatin1String("AnimatorController")) return QStringLiteral("AnimatorController");
        return {};
    default: return {};
    }
}

QString RegistryAssetBackend::relative(const QString& absolutePath) const {
    return QDir(m_root).relativeFilePath(QDir::cleanPath(absolutePath));
}

std::optional<Uuid> RegistryAssetBackend::uuidOfPath(const QString& absolutePath) const {
    return m_registry.uuidForPath(relative(absolutePath).toStdString());
}

std::optional<AssetInfo> RegistryAssetBackend::infoForUuid(const Uuid& id) const {
    if (id.isNil()) return std::nullopt;
    auto ai = m_registry.info(id);
    if (!ai) return std::nullopt;
    AssetInfo a;
    a.uuid = id;
    a.isSubAsset = ai->parent.isValid();
    const Uuid source = a.isSubAsset ? ai->parent : id;
    a.path = QDir::cleanPath(QString::fromStdString(m_registry.absolutePath(source).string()));
    a.relativePath = QString::fromStdString(ai->path);
    a.importer = QString::fromStdString(ai->importer);
    a.imported = ai->imported;
    a.subName = QString::fromStdString(ai->subName);
    const QFileInfo fi(a.path);
    a.name = stripExtensions(fi.fileName());
    if (a.isSubAsset) a.name += QStringLiteral(" · ") + QString(a.subName).replace(QLatin1Char('/'), QLatin1Char(' '));
    const QString kind = ai->info.is_object() && ai->info.contains("kind") && ai->info["kind"].is_string()
                             ? QString::fromStdString(ai->info["kind"].get<std::string>())
                             : QString();
    QString parentImporter = a.importer;
    if (a.isSubAsset) parentImporter.clear();
    a.type = editorTypeName(int(ai->type), parentImporter, kind);
    if (a.type.isEmpty()) a.type = FileSystemAssetBackend::typeForSuffix(fi.fileName());
    a.size = fi.size();
    a.modified = fi.lastModified();
    return a;
}

std::optional<AssetInfo> RegistryAssetBackend::info(const QString& path) const {
    const QFileInfo fi(path);
    if (!fi.exists()) return std::nullopt;
    if (!fi.isDir()) {
        if (auto id = uuidOfPath(fi.absoluteFilePath())) {
            if (auto a = infoForUuid(*id)) return a;
        }
    }
    AssetInfo a;
    a.path = QDir::cleanPath(fi.absoluteFilePath());
    a.relativePath = relative(a.path);
    a.isFolder = fi.isDir();
    a.type = a.isFolder ? QStringLiteral("Folder") : FileSystemAssetBackend::typeForSuffix(fi.fileName());
    a.name = a.isFolder ? fi.fileName() : stripExtensions(fi.fileName());
    a.uuid = Uuid::fromName(a.relativePath.toStdString());
    a.size = fi.size();
    a.modified = fi.lastModified();
    return a;
}

QList<AssetInfo> RegistryAssetBackend::list(const QString& folder) const {
    QList<AssetInfo> folders, files;
    QDir dir(folder.isEmpty() ? m_root : folder);
    for (const QFileInfo& fi : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        if (fi.fileName().endsWith(QLatin1String(".meta")) || fi.fileName().startsWith(QLatin1Char('.'))) continue;
        if (auto a = info(fi.absoluteFilePath())) (a->isFolder ? folders : files).push_back(*a);
    }
    return folders + files;
}

std::optional<AssetInfo> RegistryAssetBackend::find(const Uuid& id) const { return infoForUuid(id); }

QString RegistryAssetBackend::typeForPath(const QString& path) const {
    if (auto a = info(path)) return a->type;
    return FileSystemAssetBackend::typeForSuffix(QFileInfo(path).fileName());
}

QStringList RegistryAssetBackend::creatableTypes() const {
    return {QStringLiteral("Scene"), QStringLiteral("Prefab"), QStringLiteral("Material"), QStringLiteral("Script"),
            QStringLiteral("BehaviorTree")};
}

QString RegistryAssetBackend::createFolder(const QString& parent, const QString& name, QString* error) {
    const QString path = FileSystemAssetBackend::uniquePath(parent.isEmpty() ? m_root : parent, name, {});
    if (!QDir().mkpath(path)) {
        if (error) *error = QStringLiteral("Cannot create folder %1").arg(path);
        return {};
    }
    return path;
}

QString RegistryAssetBackend::createAsset(const QString& folder, const QString& type, const QString& name, QString* error) {
    const QString path = FileSystemAssetBackend::writeNewAsset(folder.isEmpty() ? m_root : folder, type, name, error);
    if (!path.isEmpty()) m_registry.scan(); // creates the .meta (UUID) right away
    return path;
}

QString RegistryAssetBackend::rename(const QString& path, const QString& newName, QString* error) {
    const QFileInfo fi(path);
    const QString target = fi.dir().filePath(newName + (fi.isDir() ? QString() : allExtensions(fi.fileName())));
    if (QFileInfo::exists(target)) {
        if (error) *error = QStringLiteral("%1 already exists").arg(QFileInfo(target).fileName());
        return {};
    }
    if (!QFile::rename(path, target) && !QDir().rename(path, target)) {
        if (error) *error = QStringLiteral("Cannot rename %1").arg(fi.fileName());
        return {};
    }
    // The .meta travels with the file, so the registry keeps the UUID (references stay valid).
    if (QFile::exists(path + QStringLiteral(".meta"))) QFile::rename(path + QStringLiteral(".meta"), target + QStringLiteral(".meta"));
    m_registry.scan();
    return target;
}

bool RegistryAssetBackend::remove(const QString& path, QString* error) {
    const QFileInfo fi(path);
    const bool ok = fi.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
    QFile::remove(path + QStringLiteral(".meta"));
    if (!ok) {
        if (error) *error = QStringLiteral("Cannot delete %1").arg(fi.fileName());
        return false;
    }
    m_registry.scan(); // drops the record and its cached artifacts
    return true;
}

QString RegistryAssetBackend::move(const QString& path, const QString& destinationFolder, QString* error) {
    const QFileInfo fi(path);
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
    m_registry.scan();
    return target;
}

QStringList RegistryAssetBackend::importFiles(const QStringList& sources, const QString& folder, QString* error) {
    QStringList copied;
    for (const QString& src : sources) {
        const QFileInfo fi(src);
        if (!fi.exists() || fi.isDir()) continue;
        const QString base = stripExtensions(fi.fileName());
        const QString dest = FileSystemAssetBackend::uniquePath(folder.isEmpty() ? m_root : folder, base, allExtensions(fi.fileName()));
        if (QDir::cleanPath(fi.absoluteFilePath()) == QDir::cleanPath(dest)) continue;
        if (!QFile::copy(src, dest)) {
            if (error) *error = QStringLiteral("Cannot copy %1").arg(fi.fileName());
            continue;
        }
        copied << dest;
    }
    if (copied.isEmpty()) return copied;
    m_registry.scan();
    QStringList failed;
    for (const QString& p : copied) {
        auto id = uuidOfPath(p);
        if (!id) continue; // no importer for this extension: kept as a plain file
        if (auto st = m_registry.import(*id); !st) {
            failed << QStringLiteral("%1: %2").arg(QFileInfo(p).fileName(), QString::fromStdString(st.error().message));
        }
    }
    if (!failed.isEmpty() && error) *error = failed.join(QLatin1Char('\n'));
    OX_LOG_INFO("assets", "Imported {} file(s) into {}", copied.size(), relative(folder).toStdString());
    return copied;
}

bool RegistryAssetBackend::reimport(const QString& path, QString* error) {
    auto id = uuidOfPath(path);
    if (!id) {
        if (error) *error = QStringLiteral("%1 is not an imported asset").arg(QFileInfo(path).fileName());
        return false;
    }
    if (auto st = m_registry.import(*id, true); !st) {
        if (error) *error = QString::fromStdString(st.error().message);
        return false;
    }
    return true;
}

void RegistryAssetBackend::rescan() { m_registry.scan(); }

QList<AssetInfo> RegistryAssetBackend::dependents(const QString& path) const {
    QList<AssetInfo> out;
    auto id = uuidOfPath(path);
    if (!id) return out;
    std::vector<Uuid> roots{*id};
    if (auto ai = m_registry.info(*id)) roots.insert(roots.end(), ai->subAssets.begin(), ai->subAssets.end());
    std::set<Uuid> self(roots.begin(), roots.end());
    std::set<Uuid> seen;
    for (const Uuid& r : roots) {
        for (const Uuid& d : m_registry.dependents(r)) {
            auto di = m_registry.info(d);
            const Uuid source = di && di->parent.isValid() ? di->parent : d;
            if (self.contains(source) || self.contains(d) || !seen.insert(source).second) continue;
            if (auto a = infoForUuid(source)) out.push_back(*a);
        }
    }
    return out;
}

QList<AssetInfo> RegistryAssetBackend::dependencies(const QString& path) const {
    QList<AssetInfo> out;
    auto id = uuidOfPath(path);
    if (!id) return out;
    for (const Uuid& d : m_registry.dependencies(*id)) {
        if (auto a = infoForUuid(d)) out.push_back(*a);
    }
    return out;
}

QList<AssetInfo> RegistryAssetBackend::subAssets(const QString& path) const {
    QList<AssetInfo> out;
    auto id = uuidOfPath(path);
    if (!id) return out;
    if (auto ai = m_registry.info(*id)) {
        for (const Uuid& s : ai->subAssets) {
            if (auto a = infoForUuid(s)) out.push_back(*a);
        }
    }
    return out;
}

QList<AssetInfo> RegistryAssetBackend::allOfType(const QString& type) const {
    QList<AssetInfo> out;
    for (const Uuid& id : m_registry.allAssets()) {
        auto a = infoForUuid(id);
        if (a && assetTypeMatches(a->type, type)) out.push_back(*a);
    }
    std::sort(out.begin(), out.end(), [](const AssetInfo& a, const AssetInfo& b) { return a.relativePath.compare(b.relativePath, Qt::CaseInsensitive) < 0; });
    return out;
}

std::optional<ImportSettings> RegistryAssetBackend::importSettings(const QString& path) const {
    auto id = uuidOfPath(path);
    if (!id) return std::nullopt;
    auto meta = m_registry.meta(*id);
    if (!meta) return std::nullopt;
    ImportSettings s;
    s.importer = QString::fromStdString(meta->importer);
    s.typeName = settingsTypeFor(s.importer);
    s.settings = toJson(meta->settings);
    if (auto ai = m_registry.info(*id); ai && ai->info.is_object()) s.info = toJson(ai->info);
    return s;
}

bool RegistryAssetBackend::setImportSettings(const QString& path, const nlohmann::json& settings, QString* error) {
    auto id = uuidOfPath(path);
    if (!id) {
        if (error) *error = QStringLiteral("%1 is not an imported asset").arg(QFileInfo(path).fileName());
        return false;
    }
    if (auto st = m_registry.setSettings(*id, nlohmann::ordered_json::parse(settings.dump())); !st) {
        if (error) *error = QString::fromStdString(st.error().message);
        return false;
    }
    return true;
}

std::optional<serial::Document> RegistryAssetBackend::loadPrefabDocument(const QString& path, QString* error) const {
    const QString lower = path.toLower();
    if (lower.endsWith(QLatin1String(".oxprefab")) || lower.endsWith(QLatin1String(".oxprefab.json"))) {
        return IAssetBackend::loadPrefabDocument(path, error);
    }
    auto id = uuidOfPath(path);
    if (!id) {
        if (error) *error = QStringLiteral("%1 is not an asset").arg(QFileInfo(path).fileName());
        return std::nullopt;
    }
    auto bytes = m_registry.readArtifact(*id); // imports on demand (model -> prefab artifact)
    if (!bytes) {
        if (error) *error = QString::fromStdString(bytes.error().message);
        return std::nullopt;
    }
    auto doc = serial::decodeAny(*bytes);
    if (!doc || doc->kind != "prefab") {
        if (error) *error = QStringLiteral("%1 has no prefab").arg(QFileInfo(path).fileName());
        return std::nullopt;
    }
    return std::move(*doc);
}

QString RegistryAssetBackend::thumbnailCacheDir() const {
    return QDir(QString::fromStdString(m_registry.cacheDir().string())).filePath(QStringLiteral("thumbnails"));
}

Uuid RegistryAssetBackend::resolveReference(const Uuid& id, const QString& assetType, const QString& wantedType) const {
    if (assetTypeMatches(assetType, wantedType)) return id;
    if (assetType != QLatin1String("Model")) return {};
    auto ai = m_registry.info(id);
    if (!ai) return {};
    if (ai->subAssets.empty()) (void)m_registry.import(id); // sub-assets exist after the first import
    ai = m_registry.info(id);
    for (const Uuid& s : ai->subAssets) {
        if (auto a = infoForUuid(s); a && assetTypeMatches(a->type, wantedType)) return s;
    }
    return {};
}

QImage RegistryAssetBackend::decodedPreview(const AssetInfo& asset, int sizePx) const {
    if (asset.type != QLatin1String("Texture") || asset.uuid.isNil()) return {};
    auto bytes = m_registry.readArtifact(asset.uuid);
    if (!bytes) return {};
    auto tex = assets::deserializeTexture(*bytes);
    if (!tex || tex->mips.empty()) return {};
    // Smallest level that still covers the thumbnail.
    usize level = 0;
    while (level + 1 < tex->mips.size() && int(std::max(tex->mips[level + 1].width, tex->mips[level + 1].height)) >= sizePx) ++level;
    assets::TextureData one = *tex;
    one.mips = {tex->mips[level]};
    one.firstMip = tex->firstMip + u32(level);
    one.mipCount = 1;
    one.width = tex->mips[level].width;
    one.height = tex->mips[level].height;
    if (assets::isBlockCompressed(one.format)) one = assets::decompressToRGBA8(one);
    if (one.mips.empty()) return {};
    const assets::TextureMip& m = one.mips[0];
    const int w = int(m.width), h = int(m.height);
    QImage img(w, h, QImage::Format_RGBA8888);
    const std::byte* src = m.data.data(); // first layer/face
    for (int y = 0; y < h; ++y) {
        uchar* dst = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const usize i = usize(y) * usize(w) + usize(x);
            u8 r = 0, g = 0, b = 0, a = 255;
            switch (one.format) {
            case assets::TextureFormat::RGBA8Unorm:
            case assets::TextureFormat::RGBA8Srgb: {
                const auto* p = reinterpret_cast<const u8*>(src) + i * 4;
                r = p[0];
                g = p[1];
                b = p[2];
                a = p[3];
                break;
            }
            case assets::TextureFormat::RG8Unorm: {
                const auto* p = reinterpret_cast<const u8*>(src) + i * 2;
                r = p[0];
                g = p[1];
                break;
            }
            case assets::TextureFormat::R8Unorm: r = g = b = reinterpret_cast<const u8*>(src)[i]; break;
            case assets::TextureFormat::R16Unorm: {
                u16 v;
                std::memcpy(&v, src + i * 2, 2);
                r = g = b = u8(v >> 8);
                break;
            }
            case assets::TextureFormat::R32Float: {
                float v;
                std::memcpy(&v, src + i * 4, 4);
                r = g = b = toByte(v);
                break;
            }
            case assets::TextureFormat::RGBA16Float: {
                u16 hv[4];
                std::memcpy(hv, src + i * 8, 8);
                r = toByte(toDisplay(glm::unpackHalf1x16(hv[0])));
                g = toByte(toDisplay(glm::unpackHalf1x16(hv[1])));
                b = toByte(toDisplay(glm::unpackHalf1x16(hv[2])));
                break;
            }
            case assets::TextureFormat::RGBA32Float: {
                float fv[4];
                std::memcpy(fv, src + i * 16, 16);
                r = toByte(toDisplay(fv[0]));
                g = toByte(toDisplay(fv[1]));
                b = toByte(toDisplay(fv[2]));
                break;
            }
            default: return {};
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }
    return img;
}

} // namespace ox::editor

#endif
