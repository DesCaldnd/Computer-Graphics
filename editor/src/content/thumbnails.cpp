#include "content/thumbnails.hpp"

#include "core/editor_context.hpp"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>

namespace ox::editor {

ThumbnailCache& ThumbnailCache::instance() {
    static ThumbnailCache cache;
    return cache;
}

QString ThumbnailCache::cacheFile(EditorContext& ctx, const AssetInfo& asset) {
    const QString dir = ctx.services().assets().thumbnailCacheDir();
    if (dir.isEmpty() || asset.uuid.isNil()) return {};
    return QDir(dir).filePath(qs(asset.uuid.toString()) + QStringLiteral(".png"));
}

void ThumbnailCache::invalidate(const Uuid& id) {
    const QString prefix = qs(id.toString());
    for (auto it = m_memory.begin(); it != m_memory.end();) {
        if (it.key().startsWith(prefix)) it = m_memory.erase(it);
        else ++it;
    }
}

QPixmap ThumbnailCache::get(EditorContext& ctx, const AssetInfo& a, int sizePx) {
    if (a.isFolder) return {};
    const QString key = qs(a.uuid.toString()) + QLatin1Char('|') + QString::number(a.modified.toMSecsSinceEpoch()) + QLatin1Char('|') +
                        QString::number(sizePx) + QLatin1Char('|') + ctx.services().assets().name();
    if (auto it = m_memory.find(key); it != m_memory.end()) return *it;
    QImage img;
    const QString diskFile = cacheFile(ctx, a);
    if (!diskFile.isEmpty()) {
        const QFileInfo fi(diskFile);
        if (fi.exists() && fi.lastModified() >= a.modified) {
            QImage cached(diskFile);
            if (!cached.isNull() && std::max(cached.width(), cached.height()) >= std::min(sizePx, 128)) img = cached;
        }
    }
    bool generated = false;
    if (img.isNull() && a.type == QLatin1String("Texture") && !a.isSubAsset) {
        QImageReader reader(a.path);
        const QSize s = reader.size();
        if (s.isValid()) {
            reader.setScaledSize(s.scaled(QSize(sizePx, sizePx), Qt::KeepAspectRatio));
            img = reader.read();
        }
        generated = !img.isNull();
    }
    if (img.isNull() && a.type == QLatin1String("Texture")) {
        img = ctx.services().assets().decodedPreview(a, sizePx);
        if (!img.isNull()) img = img.scaled(QSize(sizePx, sizePx), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        generated = !img.isNull();
    }
    if (img.isNull()) {
        if (auto* r = ctx.services().thumbnails()) {
            img = r->render({a.uuid, a.path, a.type, sizePx});
            generated = !img.isNull();
        }
    }
    if (generated && !diskFile.isEmpty()) {
        QDir().mkpath(QFileInfo(diskFile).absolutePath());
        img.save(diskFile);
    }
    QPixmap pm = img.isNull() ? QPixmap() : QPixmap::fromImage(img);
    m_memory.insert(key, pm);
    return pm;
}

} // namespace ox::editor
