#include "recast_build.hpp"

#include <oxwald/ai/navmesh.hpp>
#include <oxwald/core/log.hpp>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>
#include <type_traits>

namespace ox::ai {

using namespace detail;

namespace {

constexpr u32 kMagic = 0x4D4E584F; // "OXNM"
constexpr u32 kVersion = 1;

// Explicit little-endian-ish field writer (host order; all supported targets are little-endian) — avoids
// serialising struct padding so identical meshes produce identical bytes.
struct Writer {
    std::vector<u8>& out;
    template <class T>
    void put(const T& v) {
        static_assert(std::is_arithmetic_v<T>);
        const auto* b = reinterpret_cast<const u8*>(&v);
        out.insert(out.end(), b, b + sizeof(T));
    }
    void bytes(const void* p, usize n) {
        const auto* b = static_cast<const u8*>(p);
        out.insert(out.end(), b, b + n);
    }
};

struct Reader {
    std::span<const u8> in;
    usize off = 0;
    bool ok = true;
    template <class T>
    T get() {
        T v{};
        if (off + sizeof(T) > in.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, in.data() + off, sizeof(T));
        off += sizeof(T);
        return v;
    }
};

void writeSettings(Writer& w, const NavMeshBuildSettings& s) {
    for (f32 v : {s.cellSize, s.cellHeight, s.agentHeight, s.agentRadius, s.agentMaxClimb, s.agentMaxSlope, s.edgeMaxLen,
                  s.edgeMaxError, s.detailSampleDist, s.detailSampleMaxError}) {
        w.put(v);
    }
    for (i32 v : {s.regionMinSize, s.regionMergeSize, s.vertsPerPoly, s.tileSize}) {
        w.put(v);
    }
    for (u8 v : {static_cast<u8>(s.partition), static_cast<u8>(s.filterLowHangingObstacles), static_cast<u8>(s.filterLedgeSpans),
                 static_cast<u8>(s.filterWalkableLowHeightSpans), static_cast<u8>(s.tiled)}) {
        w.put(v);
    }
    for (u16 f : s.areaFlags) {
        w.put(f);
    }
}

NavMeshBuildSettings readSettings(Reader& r) {
    NavMeshBuildSettings s;
    for (f32* v : {&s.cellSize, &s.cellHeight, &s.agentHeight, &s.agentRadius, &s.agentMaxClimb, &s.agentMaxSlope, &s.edgeMaxLen,
                   &s.edgeMaxError, &s.detailSampleDist, &s.detailSampleMaxError}) {
        *v = r.get<f32>();
    }
    for (i32* v : {&s.regionMinSize, &s.regionMergeSize, &s.vertsPerPoly, &s.tileSize}) {
        *v = r.get<i32>();
    }
    s.partition = static_cast<NavPartition>(r.get<u8>());
    s.filterLowHangingObstacles = r.get<u8>() != 0;
    s.filterLedgeSpans = r.get<u8>() != 0;
    s.filterWalkableLowHeightSpans = r.get<u8>() != 0;
    s.tiled = r.get<u8>() != 0;
    for (u16& f : s.areaFlags) {
        f = r.get<u16>();
    }
    return s;
}

} // namespace

NavMesh::~NavMesh() {
    if (m_mesh != nullptr) {
        dtFreeNavMesh(m_mesh);
    }
}

std::unique_ptr<NavMesh> NavMesh::build(const NavMeshInput& input, const NavMeshBuildSettings& settings) {
    if (input.vertices.empty() || input.indices.size() < 3) {
        OX_LOG_ERROR("ai", "navmesh: empty input");
        return nullptr;
    }
    std::unique_ptr<NavMesh> mesh(new NavMesh());
    const bool ok = settings.tiled ? mesh->initTiled(input, settings) : mesh->initSolo(input, settings);
    if (!ok) {
        return nullptr;
    }
    const NavMeshStats st = mesh->stats();
    OX_LOG_INFO("ai", "navmesh built: {} tile(s), {} polys, {} KiB", st.tiles, st.polygons, st.dataBytes / 1024);
    return mesh;
}

bool NavMesh::initSolo(const NavMeshInput& input, const NavMeshBuildSettings& settings) {
    m_settings = settings;
    m_settings.tiled = false;
    std::tie(m_bmin, m_bmax) = input.bounds();
    rcContext ctx(false);
    rcConfig cfg = makeConfig(m_settings);
    std::memcpy(cfg.bmin, &m_bmin.x, sizeof(float) * 3);
    std::memcpy(cfg.bmax, &m_bmax.x, sizeof(float) * 3);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    unsigned char* data = nullptr;
    i32 size = 0;
    if (!buildTileData(ctx, cfg, input, m_settings, 0, 0, &data, &size)) {
        OX_LOG_ERROR("ai", "navmesh: solo build produced no walkable polygons");
        return false;
    }
    m_mesh = dtAllocNavMesh();
    if (m_mesh == nullptr || dtStatusFailed(m_mesh->init(data, size, DT_TILE_FREE_DATA))) {
        dtFree(data);
        OX_LOG_ERROR("ai", "navmesh: dtNavMesh::init failed");
        return false;
    }
    m_tiles = {1, 1};
    return true;
}

bool NavMesh::initTiled(const NavMeshInput& input, const NavMeshBuildSettings& settings) {
    m_settings = settings;
    m_settings.tiled = true;
    m_settings.tileSize = std::max(settings.tileSize, 8);
    m_input = input;
    std::tie(m_bmin, m_bmax) = input.bounds();

    int gw = 0, gh = 0;
    rcCalcGridSize(&m_bmin.x, &m_bmax.x, m_settings.cellSize, &gw, &gh);
    const i32 ts = m_settings.tileSize;
    m_tiles = {(gw + ts - 1) / ts, (gh + ts - 1) / ts};

    const u32 tileBits = std::min<u32>(ilog2(nextPow2(static_cast<u32>(m_tiles.x * m_tiles.y))), 14);
    const u32 polyBits = 22 - tileBits;
    dtNavMeshParams params{};
    std::memcpy(params.orig, &m_bmin.x, sizeof(float) * 3);
    params.tileWidth = static_cast<f32>(ts) * m_settings.cellSize;
    params.tileHeight = static_cast<f32>(ts) * m_settings.cellSize;
    params.maxTiles = 1 << tileBits;
    params.maxPolys = 1 << polyBits;

    m_mesh = dtAllocNavMesh();
    if (m_mesh == nullptr || dtStatusFailed(m_mesh->init(&params))) {
        OX_LOG_ERROR("ai", "navmesh: tiled init failed");
        return false;
    }
    i32 built = 0;
    for (i32 y = 0; y < m_tiles.y; ++y) {
        for (i32 x = 0; x < m_tiles.x; ++x) {
            built += rebuildTile(x, y) ? 1 : 0;
        }
    }
    if (built == 0) {
        OX_LOG_ERROR("ai", "navmesh: tiled build produced no walkable tiles");
        return false;
    }
    return true;
}

bool NavMesh::rebuildTile(i32 tx, i32 ty) {
    if (m_mesh == nullptr || !m_settings.tiled || tx < 0 || ty < 0 || tx >= m_tiles.x || ty >= m_tiles.y) {
        return false;
    }
    removeTile(tx, ty);
    if (m_input.vertices.empty()) {
        return false;
    }
    rcContext ctx(false);
    const rcConfig cfg = makeTileConfig(m_settings, m_bmin, m_bmax, tx, ty);
    unsigned char* data = nullptr;
    i32 size = 0;
    if (!buildTileData(ctx, cfg, m_input, m_settings, tx, ty, &data, &size)) {
        return false; // empty tile is fine
    }
    if (dtStatusFailed(m_mesh->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr))) {
        dtFree(data);
        OX_LOG_ERROR("ai", "navmesh: addTile({}, {}) failed", tx, ty);
        return false;
    }
    return true;
}

void NavMesh::removeTile(i32 tx, i32 ty) {
    if (m_mesh == nullptr) {
        return;
    }
    const dtTileRef ref = m_mesh->getTileRefAt(tx, ty, 0);
    if (ref != 0) {
        m_mesh->removeTile(ref, nullptr, nullptr);
    }
}

glm::ivec2 NavMesh::tileCoord(const glm::vec3& pos) const {
    if (!m_settings.tiled) {
        return {0, 0};
    }
    const f32 tcs = static_cast<f32>(m_settings.tileSize) * m_settings.cellSize;
    return {static_cast<i32>(std::floor((pos.x - m_bmin.x) / tcs)), static_cast<i32>(std::floor((pos.z - m_bmin.z) / tcs))};
}

glm::ivec2 NavMesh::tileGridSize() const { return m_tiles; }

i32 NavMesh::rebuildTiles(const NavMeshInput& newInput, const glm::vec3& bmin, const glm::vec3& bmax) {
    if (!m_settings.tiled) {
        OX_LOG_WARN("ai", "navmesh: rebuildTiles requires a tiled navmesh");
        return 0;
    }
    m_input = newInput;
    // The XZ tile grid is fixed; the vertical range follows the new geometry (e.g. a tall obstacle).
    const auto [nmin, nmax] = newInput.bounds();
    m_bmin.y = std::min(m_bmin.y, nmin.y);
    m_bmax.y = std::max(m_bmax.y, nmax.y);
    const glm::ivec2 a = glm::clamp(tileCoord(bmin), glm::ivec2(0), m_tiles - 1);
    const glm::ivec2 b = glm::clamp(tileCoord(bmax), glm::ivec2(0), m_tiles - 1);
    i32 count = 0;
    for (i32 y = a.y; y <= b.y; ++y) {
        for (i32 x = a.x; x <= b.x; ++x) {
            rebuildTile(x, y);
            ++count;
        }
    }
    return count;
}

std::vector<u8> NavMesh::serialize() const {
    std::vector<u8> out;
    if (m_mesh == nullptr) {
        return out;
    }
    const dtNavMesh& nav = *m_mesh;
    Writer w{out};
    w.put(kMagic);
    w.put(kVersion);
    writeSettings(w, m_settings);
    for (f32 v : {m_bmin.x, m_bmin.y, m_bmin.z, m_bmax.x, m_bmax.y, m_bmax.z}) {
        w.put(v);
    }
    w.put(m_tiles.x);
    w.put(m_tiles.y);
    const dtNavMeshParams* p = nav.getParams();
    for (f32 v : {p->orig[0], p->orig[1], p->orig[2], p->tileWidth, p->tileHeight}) {
        w.put(v);
    }
    w.put(static_cast<i32>(p->maxTiles));
    w.put(static_cast<i32>(p->maxPolys));
    u32 tileCount = 0;
    for (int i = 0; i < nav.getMaxTiles(); ++i) {
        const dtMeshTile* t = nav.getTile(i);
        tileCount += (t != nullptr && t->header != nullptr && t->dataSize > 0) ? 1 : 0;
    }
    w.put(tileCount);
    for (int i = 0; i < nav.getMaxTiles(); ++i) {
        const dtMeshTile* t = nav.getTile(i);
        if (t == nullptr || t->header == nullptr || t->dataSize <= 0) {
            continue;
        }
        w.put(static_cast<u64>(nav.getTileRef(t)));
        w.put(static_cast<i32>(t->dataSize));
        w.bytes(t->data, static_cast<usize>(t->dataSize));
    }
    return out;
}

std::unique_ptr<NavMesh> NavMesh::deserialize(std::span<const u8> bytes) {
    Reader r{bytes};
    if (r.get<u32>() != kMagic || r.get<u32>() != kVersion || !r.ok) {
        OX_LOG_ERROR("ai", "navmesh: bad blob header");
        return nullptr;
    }
    std::unique_ptr<NavMesh> mesh(new NavMesh());
    mesh->m_settings = readSettings(r);
    for (f32* v : {&mesh->m_bmin.x, &mesh->m_bmin.y, &mesh->m_bmin.z, &mesh->m_bmax.x, &mesh->m_bmax.y, &mesh->m_bmax.z}) {
        *v = r.get<f32>();
    }
    mesh->m_tiles.x = r.get<i32>();
    mesh->m_tiles.y = r.get<i32>();
    dtNavMeshParams params{};
    for (f32* v : {&params.orig[0], &params.orig[1], &params.orig[2], &params.tileWidth, &params.tileHeight}) {
        *v = r.get<f32>();
    }
    params.maxTiles = r.get<i32>();
    params.maxPolys = r.get<i32>();
    const u32 tileCount = r.get<u32>();
    if (!r.ok) {
        OX_LOG_ERROR("ai", "navmesh: truncated blob header");
        return nullptr;
    }
    mesh->m_mesh = dtAllocNavMesh();
    if (mesh->m_mesh == nullptr || dtStatusFailed(mesh->m_mesh->init(&params))) {
        OX_LOG_ERROR("ai", "navmesh: init from blob failed");
        return nullptr;
    }
    for (u32 i = 0; i < tileCount; ++i) {
        const u64 ref = r.get<u64>();
        const i32 size = r.get<i32>();
        if (!r.ok || size <= 0 || r.off + static_cast<usize>(size) > bytes.size()) {
            OX_LOG_ERROR("ai", "navmesh: truncated tile data");
            return nullptr;
        }
        auto* data = static_cast<unsigned char*>(dtAlloc(size, DT_ALLOC_PERM));
        std::memcpy(data, bytes.data() + r.off, static_cast<usize>(size));
        r.off += static_cast<usize>(size);
        if (dtStatusFailed(mesh->m_mesh->addTile(data, size, DT_TILE_FREE_DATA, static_cast<dtTileRef>(ref), nullptr))) {
            dtFree(data);
            OX_LOG_ERROR("ai", "navmesh: addTile from blob failed");
            return nullptr;
        }
    }
    return mesh;
}

NavMeshStats NavMesh::stats() const {
    NavMeshStats s;
    if (m_mesh == nullptr) {
        return s;
    }
    const dtNavMesh& nav = *m_mesh;
    for (int i = 0; i < nav.getMaxTiles(); ++i) {
        const dtMeshTile* t = nav.getTile(i);
        if (t == nullptr || t->header == nullptr) {
            continue;
        }
        ++s.tiles;
        s.polygons += static_cast<u32>(t->header->polyCount - t->header->offMeshConCount);
        s.vertices += static_cast<u32>(t->header->vertCount);
        s.offMeshLinks += static_cast<u32>(t->header->offMeshConCount);
        s.dataBytes += static_cast<u32>(t->dataSize);
    }
    return s;
}

bool NavMesh::setPolyFlags(NavPolyRef ref, u16 flags) {
    return m_mesh != nullptr && dtStatusSucceed(m_mesh->setPolyFlags(static_cast<dtPolyRef>(ref), flags));
}

u16 NavMesh::polyFlags(NavPolyRef ref) const {
    unsigned short f = 0;
    if (m_mesh != nullptr) {
        m_mesh->getPolyFlags(static_cast<dtPolyRef>(ref), &f);
    }
    return f;
}

u8 NavMesh::polyArea(NavPolyRef ref) const {
    unsigned char a = 0;
    if (m_mesh != nullptr) {
        m_mesh->getPolyArea(static_cast<dtPolyRef>(ref), &a);
    }
    return a;
}

void NavMesh::debugDraw(const DebugLineFn& line) const {
    if (m_mesh == nullptr || !line) {
        return;
    }
    const dtNavMesh& nav = *m_mesh;
    const glm::vec4 edge(0.f, 0.8f, 1.f, 1.f), water(0.2f, 0.3f, 1.f, 1.f), link(1.f, 0.f, 1.f, 1.f);
    const glm::vec3 lift(0.f, 0.05f, 0.f);
    for (int i = 0; i < nav.getMaxTiles(); ++i) {
        const dtMeshTile* t = nav.getTile(i);
        if (t == nullptr || t->header == nullptr) {
            continue;
        }
        for (int p = 0; p < t->header->polyCount; ++p) {
            const dtPoly& poly = t->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
                const dtOffMeshConnection* con = &t->offMeshCons[p - t->header->offMeshBase];
                line({con->pos[0], con->pos[1], con->pos[2]}, {con->pos[3], con->pos[4], con->pos[5]}, link);
                continue;
            }
            const glm::vec4 c = poly.getArea() == NavArea::Water ? water : edge;
            for (int v = 0; v < poly.vertCount; ++v) {
                const float* a = &t->verts[poly.verts[v] * 3];
                const float* b = &t->verts[poly.verts[(v + 1) % poly.vertCount] * 3];
                line(glm::vec3(a[0], a[1], a[2]) + lift, glm::vec3(b[0], b[1], b[2]) + lift, c);
            }
        }
    }
}

} // namespace ox::ai
