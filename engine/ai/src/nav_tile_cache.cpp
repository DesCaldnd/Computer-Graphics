#include "recast_build.hpp"

#include <oxwald/ai/nav_tile_cache.hpp>
#include <oxwald/core/log.hpp>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourTileCache.h>
#include <DetourTileCacheBuilder.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace ox::ai {

using namespace detail;

namespace {

// Layers are small and short-lived in memory; storing them uncompressed avoids a FastLZ dependency.
struct PassThroughCompressor final : dtTileCacheCompressor {
    int maxCompressedSize(const int bufferSize) override { return bufferSize; }
    dtStatus compress(const unsigned char* buffer, const int bufferSize, unsigned char* compressed,
                      const int maxCompressedSize, int* compressedSize) override {
        if (bufferSize > maxCompressedSize) {
            return DT_FAILURE | DT_BUFFER_TOO_SMALL;
        }
        std::memcpy(compressed, buffer, static_cast<usize>(bufferSize));
        *compressedSize = bufferSize;
        return DT_SUCCESS;
    }
    dtStatus decompress(const unsigned char* compressed, const int compressedSize, unsigned char* buffer,
                        const int maxBufferSize, int* bufferSize) override {
        if (compressedSize > maxBufferSize) {
            return DT_FAILURE | DT_BUFFER_TOO_SMALL;
        }
        std::memcpy(buffer, compressed, static_cast<usize>(compressedSize));
        *bufferSize = compressedSize;
        return DT_SUCCESS;
    }
};

struct MeshProcess final : dtTileCacheMeshProcess {
    const NavMeshBuildSettings* settings = nullptr;
    const NavMeshInput* input = nullptr;
    std::vector<float> omVerts, omRad;
    std::vector<unsigned char> omDir, omArea;
    std::vector<unsigned short> omFlags;
    std::vector<unsigned int> omId;

    void prepareLinks() {
        for (const OffMeshLink& l : input->offMeshLinks) {
            omVerts.insert(omVerts.end(), {l.start.x, l.start.y, l.start.z, l.end.x, l.end.y, l.end.z});
            omRad.push_back(l.radius);
            omDir.push_back(l.bidirectional ? DT_OFFMESH_CON_BIDIR : 0);
            omArea.push_back(l.area);
            omFlags.push_back(l.flags != 0 ? l.flags : settings->areaFlags[l.area % NavArea::Count]);
            omId.push_back(l.userId);
        }
    }

    void process(dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags) override {
        for (int i = 0; i < params->polyCount; ++i) {
            if (polyAreas[i] == DT_TILECACHE_WALKABLE_AREA) {
                polyAreas[i] = NavArea::Ground;
            }
            polyFlags[i] = settings->areaFlags[polyAreas[i] % NavArea::Count];
        }
        params->offMeshConVerts = omVerts.empty() ? nullptr : omVerts.data();
        params->offMeshConRad = omRad.empty() ? nullptr : omRad.data();
        params->offMeshConDir = omDir.empty() ? nullptr : omDir.data();
        params->offMeshConAreas = omArea.empty() ? nullptr : omArea.data();
        params->offMeshConFlags = omFlags.empty() ? nullptr : omFlags.data();
        params->offMeshConUserID = omId.empty() ? nullptr : omId.data();
        params->offMeshConCount = static_cast<int>(omRad.size());
    }
};

} // namespace

struct NavTileCache::Helpers {
    dtTileCacheAlloc alloc;
    PassThroughCompressor compressor;
    MeshProcess process;
    NavMeshInput input;
};

NavTileCache::~NavTileCache() {
    if (m_cache != nullptr) {
        dtFreeTileCache(m_cache);
    }
}

std::unique_ptr<NavTileCache> NavTileCache::build(const NavMeshInput& input, const NavTileCacheSettings& settingsIn) {
    if (input.vertices.empty()) {
        OX_LOG_ERROR("ai", "tile cache: empty input");
        return nullptr;
    }
    NavTileCacheSettings settings = settingsIn;
    NavMeshBuildSettings& s = settings.build;
    s.tiled = true;
    s.partition = NavPartition::Layers;
    s.tileSize = std::max(s.tileSize, 8);

    std::unique_ptr<NavTileCache> tc(new NavTileCache());
    tc->m_helpers = std::make_unique<Helpers>();
    Helpers& h = *tc->m_helpers;
    h.input = input;
    tc->m_navMesh.reset(new NavMesh());
    NavMesh& nm = *tc->m_navMesh;
    nm.m_settings = s;
    std::tie(nm.m_bmin, nm.m_bmax) = input.bounds();
    h.process.settings = &nm.m_settings;
    h.process.input = &h.input;
    h.process.prepareLinks();

    int gw = 0, gh = 0;
    rcCalcGridSize(&nm.m_bmin.x, &nm.m_bmax.x, s.cellSize, &gw, &gh);
    const i32 ts = s.tileSize;
    nm.m_tiles = {(gw + ts - 1) / ts, (gh + ts - 1) / ts};

    dtTileCacheParams tcp{};
    std::memcpy(tcp.orig, &nm.m_bmin.x, sizeof(float) * 3);
    tcp.cs = s.cellSize;
    tcp.ch = s.cellHeight;
    tcp.width = ts;
    tcp.height = ts;
    tcp.walkableHeight = s.agentHeight;
    tcp.walkableRadius = s.agentRadius;
    tcp.walkableClimb = s.agentMaxClimb;
    tcp.maxSimplificationError = s.edgeMaxError;
    tcp.maxTiles = nm.m_tiles.x * nm.m_tiles.y * settings.expectedLayersPerTile;
    tcp.maxObstacles = settings.maxObstacles;

    tc->m_cache = dtAllocTileCache();
    if (tc->m_cache == nullptr || dtStatusFailed(tc->m_cache->init(&tcp, &h.alloc, &h.compressor, &h.process))) {
        OX_LOG_ERROR("ai", "tile cache: init failed");
        return nullptr;
    }

    const u32 tileBits =
        std::min<u32>(ilog2(nextPow2(static_cast<u32>(nm.m_tiles.x * nm.m_tiles.y * settings.expectedLayersPerTile))), 14);
    const u32 polyBits = 22 - tileBits;
    dtNavMeshParams params{};
    std::memcpy(params.orig, &nm.m_bmin.x, sizeof(float) * 3);
    params.tileWidth = static_cast<f32>(ts) * s.cellSize;
    params.tileHeight = static_cast<f32>(ts) * s.cellSize;
    params.maxTiles = 1 << tileBits;
    params.maxPolys = 1 << polyBits;
    nm.m_mesh = dtAllocNavMesh();
    if (nm.m_mesh == nullptr || dtStatusFailed(nm.m_mesh->init(&params))) {
        OX_LOG_ERROR("ai", "tile cache: navmesh init failed");
        return nullptr;
    }

    rcContext ctx(false);
    i32 layers = 0;
    for (i32 ty = 0; ty < nm.m_tiles.y; ++ty) {
        for (i32 tx = 0; tx < nm.m_tiles.x; ++tx) {
            const rcConfig cfg = makeTileConfig(s, nm.m_bmin, nm.m_bmax, tx, ty);
            RcPtr<rcCompactHeightfield> chf = buildCompactHeightfield(ctx, cfg, h.input, s);
            if (!chf) {
                continue;
            }
            RcPtr<rcHeightfieldLayerSet> lset(rcAllocHeightfieldLayerSet());
            if (!lset || !rcBuildHeightfieldLayers(&ctx, *chf, cfg.borderSize, cfg.walkableHeight, *lset)) {
                continue;
            }
            for (int i = 0; i < std::min(lset->nlayers, settings.expectedLayersPerTile); ++i) {
                const rcHeightfieldLayer* layer = &lset->layers[i];
                dtTileCacheLayerHeader header{};
                header.magic = DT_TILECACHE_MAGIC;
                header.version = DT_TILECACHE_VERSION;
                header.tx = tx;
                header.ty = ty;
                header.tlayer = i;
                std::memcpy(header.bmin, layer->bmin, sizeof(header.bmin));
                std::memcpy(header.bmax, layer->bmax, sizeof(header.bmax));
                header.width = static_cast<unsigned char>(layer->width);
                header.height = static_cast<unsigned char>(layer->height);
                header.minx = static_cast<unsigned char>(layer->minx);
                header.maxx = static_cast<unsigned char>(layer->maxx);
                header.miny = static_cast<unsigned char>(layer->miny);
                header.maxy = static_cast<unsigned char>(layer->maxy);
                header.hmin = static_cast<unsigned short>(layer->hmin);
                header.hmax = static_cast<unsigned short>(layer->hmax);
                unsigned char* data = nullptr;
                int dataSize = 0;
                if (dtStatusFailed(dtBuildTileCacheLayer(&h.compressor, &header, layer->heights, layer->areas, layer->cons,
                                                         &data, &dataSize))) {
                    continue;
                }
                if (dtStatusFailed(tc->m_cache->addTile(data, dataSize, DT_COMPRESSEDTILE_FREE_DATA, nullptr))) {
                    dtFree(data);
                    continue;
                }
                ++layers;
            }
            tc->m_cache->buildNavMeshTilesAt(tx, ty, nm.m_mesh);
        }
    }
    if (layers == 0) {
        OX_LOG_ERROR("ai", "tile cache: no walkable layers");
        return nullptr;
    }
    OX_LOG_INFO("ai", "tile cache built: {}x{} tiles, {} layers", nm.m_tiles.x, nm.m_tiles.y, layers);
    return tc;
}

NavObstacleId NavTileCache::addCylinder(const glm::vec3& basePosition, f32 radius, f32 height) {
    dtObstacleRef ref = 0;
    if (dtStatusFailed(m_cache->addObstacle(&basePosition.x, radius, height, &ref))) {
        OX_LOG_WARN("ai", "tile cache: addObstacle failed (request queue full?)");
        return 0;
    }
    return ref;
}

NavObstacleId NavTileCache::addBox(const glm::vec3& bmin, const glm::vec3& bmax) {
    dtObstacleRef ref = 0;
    if (dtStatusFailed(m_cache->addBoxObstacle(&bmin.x, &bmax.x, &ref))) {
        return 0;
    }
    return ref;
}

NavObstacleId NavTileCache::addOrientedBox(const glm::vec3& center, const glm::vec3& halfExtents, f32 yawRadians) {
    dtObstacleRef ref = 0;
    if (dtStatusFailed(m_cache->addBoxObstacle(&center.x, &halfExtents.x, yawRadians, &ref))) {
        return 0;
    }
    return ref;
}

bool NavTileCache::removeObstacle(NavObstacleId id) {
    return id != 0 && dtStatusSucceed(m_cache->removeObstacle(id));
}

i32 NavTileCache::obstacleCount() const {
    i32 n = 0;
    for (int i = 0; i < m_cache->getObstacleCount(); ++i) {
        const dtTileCacheObstacle* ob = m_cache->getObstacle(i);
        n += (ob != nullptr && ob->state != DT_OBSTACLE_EMPTY && ob->state != DT_OBSTACLE_REMOVING) ? 1 : 0;
    }
    return n;
}

bool NavTileCache::update(f32 dt) {
    bool upToDate = false;
    m_cache->update(dt, m_navMesh->m_mesh, &upToDate);
    return upToDate;
}

void NavTileCache::flush() {
    for (int i = 0; i < 1024 && !update(0.f); ++i) {
    }
}

void NavTileCache::debugDraw(const DebugLineFn& line) const {
    if (!line) {
        return;
    }
    const glm::vec4 c(1.f, 0.5f, 0.f, 1.f);
    for (int i = 0; i < m_cache->getObstacleCount(); ++i) {
        const dtTileCacheObstacle* ob = m_cache->getObstacle(i);
        if (ob == nullptr || ob->state == DT_OBSTACLE_EMPTY) {
            continue;
        }
        glm::vec3 mn, mx;
        m_cache->getObstacleBounds(ob, &mn.x, &mx.x);
        const glm::vec3 p[4] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}};
        for (int k = 0; k < 4; ++k) {
            const glm::vec3 a = p[k], b = p[(k + 1) % 4];
            line(a, b, c);
            line(a + glm::vec3(0, mx.y - mn.y, 0), b + glm::vec3(0, mx.y - mn.y, 0), c);
            line(a, a + glm::vec3(0, mx.y - mn.y, 0), c);
        }
    }
}

} // namespace ox::ai
