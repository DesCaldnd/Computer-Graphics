#include "detour_util.hpp"

#include <oxwald/ai/nav_query.hpp>
#include <oxwald/core/log.hpp>

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <cfloat>
#include <cmath>
#include <cstring>
#include <random>

namespace ox::ai {

using namespace detail;

f32 NavPath::length() const {
    f32 len = 0.f;
    for (usize i = 1; i < points.size(); ++i) {
        len += glm::distance(points[i - 1], points[i]);
    }
    return len;
}

namespace {

// Detour's random API takes a plain function pointer, so the generator is thread-local.
thread_local std::mt19937* tRng = nullptr;
float frand() {
    std::uniform_real_distribution<float> d(0.f, 1.f);
    return d(*tRng);
}

bool inRange(const float* a, const float* b, float r, float h) {
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return (dx * dx + dz * dz) < r * r && std::fabs(dy) < h;
}

int fixupCorridor(dtPolyRef* path, int npath, int maxPath, const dtPolyRef* visited, int nvisited) {
    int furthestPath = -1, furthestVisited = -1;
    for (int i = npath - 1; i >= 0; --i) {
        bool found = false;
        for (int j = nvisited - 1; j >= 0; --j) {
            if (path[i] == visited[j]) {
                furthestPath = i;
                furthestVisited = j;
                found = true;
            }
        }
        if (found) {
            break;
        }
    }
    if (furthestPath == -1 || furthestVisited == -1) {
        return npath;
    }
    const int req = nvisited - furthestVisited;
    const int orig = std::min(furthestPath + 1, npath);
    int size = std::max(0, npath - orig);
    if (req + size > maxPath) {
        size = maxPath - req;
    }
    if (size > 0) {
        std::memmove(path + req, path + orig, static_cast<usize>(size) * sizeof(dtPolyRef));
    }
    for (int i = 0; i < req; ++i) {
        path[i] = visited[(nvisited - 1) - i];
    }
    return req + size;
}

// Collapses small U-turns: if path[0] neighbours path[i] (i small), skip the detour.
int fixupShortcuts(dtPolyRef* path, int npath, const dtNavMeshQuery* q) {
    if (npath < 3) {
        return npath;
    }
    constexpr int kMaxNeis = 16;
    dtPolyRef neis[kMaxNeis];
    int nneis = 0;
    const dtMeshTile* tile = nullptr;
    const dtPoly* poly = nullptr;
    if (dtStatusFailed(q->getAttachedNavMesh()->getTileAndPolyByRef(path[0], &tile, &poly))) {
        return npath;
    }
    for (unsigned int k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next) {
        const dtLink* link = &tile->links[k];
        if (link->ref != 0 && nneis < kMaxNeis) {
            neis[nneis++] = link->ref;
        }
    }
    constexpr int kMaxLookAhead = 6;
    int cut = 0;
    for (int i = std::min(kMaxLookAhead, npath) - 1; i > 1 && cut == 0; i--) {
        for (int j = 0; j < nneis; j++) {
            if (path[i] == neis[j]) {
                cut = i;
                break;
            }
        }
    }
    if (cut > 1) {
        const int offset = cut - 1;
        npath -= offset;
        for (int i = 1; i < npath; i++) {
            path[i] = path[i + offset];
        }
    }
    return npath;
}

bool getSteerTarget(const dtNavMeshQuery* q, const float* startPos, const float* endPos, float minTargetDist,
                    const dtPolyRef* path, int pathSize, float* steerPos, unsigned char& steerPosFlag,
                    dtPolyRef& steerPosRef) {
    constexpr int kMaxSteer = 3;
    float steerPath[kMaxSteer * 3];
    unsigned char steerFlags[kMaxSteer];
    dtPolyRef steerPolys[kMaxSteer];
    int n = 0;
    q->findStraightPath(startPos, endPos, path, pathSize, steerPath, steerFlags, steerPolys, &n, kMaxSteer);
    if (n == 0) {
        return false;
    }
    int ns = 0;
    while (ns < n) {
        if ((steerFlags[ns] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) || !inRange(&steerPath[ns * 3], startPos, minTargetDist, 1000.f)) {
            break;
        }
        ns++;
    }
    if (ns >= n) {
        return false;
    }
    dtVcopy(steerPos, &steerPath[ns * 3]);
    steerPos[1] = startPos[1];
    steerPosFlag = steerFlags[ns];
    steerPosRef = steerPolys[ns];
    return true;
}

} // namespace

NavQuery::NavQuery(const NavMesh& mesh, i32 maxNodes) : m_mesh(mesh) {
    m_query = dtAllocNavMeshQuery();
    if (m_query == nullptr || mesh.detour() == nullptr || dtStatusFailed(m_query->init(mesh.detour(), maxNodes))) {
        OX_LOG_ERROR("ai", "NavQuery: init failed");
        dtFreeNavMeshQuery(m_query);
        m_query = nullptr;
    }
}

NavQuery::~NavQuery() { dtFreeNavMeshQuery(m_query); }

std::optional<NavPoint> NavQuery::nearestPoint(const glm::vec3& pos, const NavQueryFilter& filter) const {
    if (m_query == nullptr) {
        return std::nullopt;
    }
    dtQueryFilter qf;
    toDetour(filter, qf);
    dtPolyRef ref = 0;
    glm::vec3 p{};
    if (dtStatusFailed(m_query->findNearestPoly(ptr(pos), ptr(m_extents), &qf, &ref, &p.x)) || ref == 0) {
        return std::nullopt;
    }
    return NavPoint{p, ref};
}

NavPath NavQuery::findPath(const glm::vec3& start, const glm::vec3& end, const NavQueryFilter& filter, i32 maxPolys) const {
    NavPath result;
    const auto s = nearestPoint(start, filter);
    const auto e = nearestPoint(end, filter);
    if (!s || !e) {
        return result;
    }
    dtQueryFilter qf;
    toDetour(filter, qf);
    std::vector<dtPolyRef> polys(static_cast<usize>(std::max(maxPolys, 1)));
    int n = 0;
    const dtStatus st = m_query->findPath(static_cast<dtPolyRef>(s->poly), static_cast<dtPolyRef>(e->poly), ptr(s->position),
                                          ptr(e->position), &qf, polys.data(), &n, maxPolys);
    if (dtStatusFailed(st) || n == 0) {
        return result;
    }
    const bool partial = dtStatusDetail(st, DT_PARTIAL_RESULT) || polys[static_cast<usize>(n - 1)] != static_cast<dtPolyRef>(e->poly);
    glm::vec3 endPos = e->position;
    if (polys[static_cast<usize>(n - 1)] != static_cast<dtPolyRef>(e->poly)) {
        m_query->closestPointOnPoly(polys[static_cast<usize>(n - 1)], ptr(e->position), &endPos.x, nullptr);
    }
    std::vector<float> pts(static_cast<usize>(maxPolys) * 3);
    std::vector<unsigned char> flags(static_cast<usize>(maxPolys));
    std::vector<dtPolyRef> refs(static_cast<usize>(maxPolys));
    int count = 0;
    m_query->findStraightPath(ptr(s->position), ptr(endPos), polys.data(), n, pts.data(), flags.data(), refs.data(), &count,
                              maxPolys, 0);
    result.status = partial ? PathStatus::Partial : PathStatus::Complete;
    result.corridor.assign(polys.begin(), polys.begin() + n);
    for (int i = 0; i < count; ++i) {
        result.points.push_back(vec(&pts[static_cast<usize>(i) * 3]));
        result.pointFlags.push_back(flags[static_cast<usize>(i)]);
    }
    return result;
}

NavRaycastHit NavQuery::raycast(const glm::vec3& start, const glm::vec3& end, const NavQueryFilter& filter) const {
    NavRaycastHit hit;
    hit.position = end;
    const auto s = nearestPoint(start, filter);
    if (!s) {
        hit.hit = true;
        hit.t = 0.f;
        hit.position = start;
        return hit;
    }
    dtQueryFilter qf;
    toDetour(filter, qf);
    float t = 0.f;
    glm::vec3 normal{};
    dtPolyRef visited[256];
    int nvisited = 0;
    if (dtStatusFailed(m_query->raycast(static_cast<dtPolyRef>(s->poly), ptr(s->position), ptr(end), &qf, &t, &normal.x,
                                        visited, &nvisited, 256))) {
        return hit;
    }
    hit.visited.assign(visited, visited + nvisited);
    if (t == FLT_MAX) {
        hit.hit = false;
        hit.t = 1.f;
        return hit;
    }
    hit.hit = true;
    hit.t = t;
    hit.normal = normal;
    hit.position = s->position + (end - s->position) * t;
    return hit;
}

std::optional<NavPoint> NavQuery::randomPoint(u32 seed, const NavQueryFilter& filter) const {
    if (m_query == nullptr) {
        return std::nullopt;
    }
    std::mt19937 rng(seed);
    tRng = &rng;
    dtQueryFilter qf;
    toDetour(filter, qf);
    dtPolyRef ref = 0;
    glm::vec3 p{};
    const dtStatus st = m_query->findRandomPoint(&qf, frand, &ref, &p.x);
    tRng = nullptr;
    if (dtStatusFailed(st) || ref == 0) {
        return std::nullopt;
    }
    return NavPoint{p, ref};
}

std::optional<NavPoint> NavQuery::randomPointInRadius(const glm::vec3& center, f32 radius, u32 seed,
                                                      const NavQueryFilter& filter) const {
    const auto c = nearestPoint(center, filter);
    if (!c) {
        return std::nullopt;
    }
    std::mt19937 rng(seed);
    tRng = &rng;
    dtQueryFilter qf;
    toDetour(filter, qf);
    dtPolyRef ref = 0;
    glm::vec3 p{};
    const dtStatus st =
        m_query->findRandomPointAroundCircle(static_cast<dtPolyRef>(c->poly), ptr(c->position), radius, &qf, frand, &ref, &p.x);
    tRng = nullptr;
    if (dtStatusFailed(st) || ref == 0) {
        return std::nullopt;
    }
    return NavPoint{p, ref};
}

std::optional<f32> NavQuery::heightAt(const glm::vec3& pos, const NavQueryFilter& filter) const {
    const auto n = nearestPoint(pos, filter);
    if (!n) {
        return std::nullopt;
    }
    float h = 0.f;
    if (dtStatusFailed(m_query->getPolyHeight(static_cast<dtPolyRef>(n->poly), ptr(pos), &h))) {
        return n->position.y;
    }
    return h;
}

std::vector<glm::vec3> NavQuery::smoothPath(const glm::vec3& start, const glm::vec3& end, const NavQueryFilter& filter,
                                            f32 stepSize, i32 maxPoints) const {
    std::vector<glm::vec3> out;
    const NavPath path = findPath(start, end, filter);
    if (!path.found()) {
        return out;
    }
    constexpr int kMaxPolys = 512;
    dtPolyRef polys[kMaxPolys];
    int npolys = static_cast<int>(std::min<usize>(path.corridor.size(), kMaxPolys));
    for (int i = 0; i < npolys; ++i) {
        polys[i] = static_cast<dtPolyRef>(path.corridor[static_cast<usize>(i)]);
    }
    dtQueryFilter qf;
    toDetour(filter, qf);
    const dtNavMesh* nav = m_query->getAttachedNavMesh();
    float iterPos[3], targetPos[3];
    m_query->closestPointOnPoly(polys[0], ptr(path.points.front()), iterPos, nullptr);
    m_query->closestPointOnPoly(polys[npolys - 1], ptr(path.points.back()), targetPos, nullptr);
    constexpr float kSlop = 0.01f;
    out.push_back(vec(iterPos));

    while (npolys > 0 && static_cast<i32>(out.size()) < maxPoints) {
        float steerPos[3];
        unsigned char steerFlag = 0;
        dtPolyRef steerRef = 0;
        if (!getSteerTarget(m_query, iterPos, targetPos, kSlop, polys, npolys, steerPos, steerFlag, steerRef)) {
            break;
        }
        const bool endOfPath = (steerFlag & DT_STRAIGHTPATH_END) != 0;
        const bool offMesh = (steerFlag & DT_STRAIGHTPATH_OFFMESH_CONNECTION) != 0;
        float delta[3];
        dtVsub(delta, steerPos, iterPos);
        float len = std::sqrt(dtVdot(delta, delta));
        len = ((endOfPath || offMesh) && len < stepSize) ? 1.f : stepSize / len;
        float moveTgt[3];
        dtVmad(moveTgt, iterPos, delta, len);

        float result[3];
        dtPolyRef visited[16];
        int nvisited = 0;
        m_query->moveAlongSurface(polys[0], iterPos, moveTgt, &qf, result, visited, &nvisited, 16);
        npolys = fixupCorridor(polys, npolys, kMaxPolys, visited, nvisited);
        npolys = fixupShortcuts(polys, npolys, m_query);
        float h = 0.f;
        if (dtStatusSucceed(m_query->getPolyHeight(polys[0], result, &h))) {
            result[1] = h;
        }
        dtVcopy(iterPos, result);

        if (endOfPath && inRange(iterPos, steerPos, kSlop, 1.f)) {
            dtVcopy(iterPos, targetPos);
            out.push_back(vec(iterPos));
            break;
        }
        if (offMesh && inRange(iterPos, steerPos, kSlop, 1.f)) {
            dtPolyRef prevRef = 0, polyRef = polys[0];
            int npos = 0;
            while (npos < npolys && polyRef != steerRef) {
                prevRef = polyRef;
                polyRef = polys[npos];
                npos++;
            }
            for (int i = npos; i < npolys; ++i) {
                polys[i - npos] = polys[i];
            }
            npolys -= npos;
            float sp[3], ep[3];
            if (dtStatusSucceed(nav->getOffMeshConnectionPolyEndPoints(prevRef, polyRef, sp, ep))) {
                out.push_back(vec(sp));
                dtVcopy(iterPos, ep);
                float eh = 0.f;
                if (npolys > 0 && dtStatusSucceed(m_query->getPolyHeight(polys[0], iterPos, &eh))) {
                    iterPos[1] = eh;
                }
            }
        }
        out.push_back(vec(iterPos));
    }
    return out;
}

} // namespace ox::ai
