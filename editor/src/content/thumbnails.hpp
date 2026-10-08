#pragma once

#include "content/asset_backend.hpp"

#include <QHash>
#include <QPixmap>

namespace ox::editor {

class EditorContext;

// Asset thumbnails for the content browser and the asset inspector. Order: memory cache -> disk cache
// (<backend thumbnail dir>/<uuid>.png, newer than the source) -> textures through Qt image readers or the decoded
// imported artifact -> IThumbnailRenderer (render module: meshes, models, prefabs, materials). Generated images
// are written to the disk cache. Null pixmap = typed placeholder tile.
class ThumbnailCache {
public:
    static ThumbnailCache& instance();

    QPixmap get(EditorContext& ctx, const AssetInfo& asset, int sizePx = 128);
    // Path of the disk cache file of an asset ("" without a cache dir).
    [[nodiscard]] static QString cacheFile(EditorContext& ctx, const AssetInfo& asset);
    void invalidate(const Uuid& id);
    void clear() { m_memory.clear(); }

private:
    QHash<QString, QPixmap> m_memory;
};

} // namespace ox::editor
