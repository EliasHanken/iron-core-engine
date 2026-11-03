#include "ai/NavMesh.h"

#include "core/Log.h"

#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iron {

namespace {

constexpr int kMaxPath = 512;
constexpr int kMaxAgents = 64;
constexpr unsigned short kWalkable = 1;  // The one polygon flag: walk here.

const float* xyz(const Vec3& v) { return &v.x; }

// Detour's random queries take a plain function; one generator, seeded per query.
thread_local std::uint32_t g_random = 1u;
float nextRandom() {
    g_random = g_random * 1664525u + 1013904223u;
    return static_cast<float>(g_random >> 8) * (1.0f / 16777216.0f);
}

}  // namespace

struct NavMesh::Impl {
    dtNavMesh*      mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtCrowd*        crowd = nullptr;
    dtQueryFilter   filter;
    float           agentRadius = 0.35f;
    float           agentHeight = 1.8f;
    // Each polygon's island, by tile then polygon; filled on first asking.
    mutable std::vector<std::vector<int>> islands;

    ~Impl() { clear(); }

    void clear() {
        islands.clear();
        dtFreeCrowd(crowd);
        dtFreeNavMeshQuery(query);
        dtFreeNavMesh(mesh);
        crowd = nullptr;
        query = nullptr;
        mesh = nullptr;
    }

    // The polygon nearest `point` within `reach`, and the point on it; 0 if none.
    dtPolyRef nearest(const Vec3& point, const Vec3& reach, Vec3& on) const {
        dtPolyRef ref = 0;
        if (!query || dtStatusFailed(query->findNearestPoly(xyz(point), xyz(reach), &filter, &ref, &on.x))) return 0;
        return ref;
    }
};

NavMesh::NavMesh() : impl_(std::make_unique<Impl>()) {
    impl_->filter.setIncludeFlags(kWalkable);
    impl_->filter.setExcludeFlags(0);
}

NavMesh::~NavMesh() = default;

bool NavMesh::ready() const { return impl_->query != nullptr; }

bool NavMesh::build(std::span<const Vec3> vertices, std::span<const std::uint32_t> indices,
                    const NavMeshSettings& s) {
    impl_->clear();
    if (vertices.empty() || indices.size() < 3) return false;
    impl_->agentRadius = s.agentRadius;
    impl_->agentHeight = s.agentHeight;

    // Recast takes floats and ints.
    std::vector<float> verts;
    verts.reserve(vertices.size() * 3);
    for (const Vec3& v : vertices) verts.insert(verts.end(), {v.x, v.y, v.z});
    std::vector<int> tris(indices.begin(), indices.end());
    const int nverts = static_cast<int>(vertices.size());
    const int ntris = static_cast<int>(tris.size() / 3);

    rcContext ctx(false);
    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.agentMaxSlopeDegrees;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(s.agentMaxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch;
    rcCalcBounds(verts.data(), nverts, cfg.bmin, cfg.bmax);
    for (int i = 0; i < 3; ++i) {
        cfg.bmin[i] = std::max(cfg.bmin[i], (&s.boundsMin.x)[i]);
        cfg.bmax[i] = std::min(cfg.bmax[i], (&s.boundsMax.x)[i]);
    }
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    // Voxelized: walkable surfaces marked by slope, then rasterized.
    rcHeightfield* solid = rcAllocHeightfield();
    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    rcContourSet* contours = rcAllocContourSet();
    rcPolyMesh* poly = rcAllocPolyMesh();
    rcPolyMeshDetail* detail = rcAllocPolyMeshDetail();
    const auto release = [&]() {
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(contours);
        rcFreePolyMesh(poly);
        rcFreePolyMeshDetail(detail);
    };
    std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
    bool ok = rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);
    if (ok) {
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris, areas.data());
        ok = rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *solid,
                                  cfg.walkableClimb);
    }
    if (ok) {
        // What a body can't stand on or fit under is filtered out.
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);
        ok = rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf) &&
             rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) && rcBuildDistanceField(&ctx, *chf) &&
             rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea) &&
             rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours) &&
             rcBuildPolyMesh(&ctx, *contours, cfg.maxVertsPerPoly, *poly) &&
             rcBuildPolyMeshDetail(&ctx, *poly, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *detail);
    }
    if (!ok || poly->nverts >= 0xffff || poly->npolys == 0) {
        Log::error("NavMesh: Recast couldn't build a mesh (%d polygons)", ok ? poly->npolys : 0);
        release();
        return false;
    }
    for (int i = 0; i < poly->npolys; ++i) {
        poly->flags[i] = poly->areas[i] == RC_WALKABLE_AREA ? kWalkable : 0;
    }

    // Into Detour's form, one tile.
    dtNavMeshCreateParams params{};
    params.verts = poly->verts;
    params.vertCount = poly->nverts;
    params.polys = poly->polys;
    params.polyAreas = poly->areas;
    params.polyFlags = poly->flags;
    params.polyCount = poly->npolys;
    params.nvp = poly->nvp;
    params.detailMeshes = detail->meshes;
    params.detailVerts = detail->verts;
    params.detailVertsCount = detail->nverts;
    params.detailTris = detail->tris;
    params.detailTriCount = detail->ntris;
    params.walkableHeight = s.agentHeight;
    params.walkableRadius = s.agentRadius;
    params.walkableClimb = s.agentMaxClimb;
    std::memcpy(params.bmin, poly->bmin, sizeof(params.bmin));
    std::memcpy(params.bmax, poly->bmax, sizeof(params.bmax));
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    unsigned char* data = nullptr;
    int size = 0;
    ok = dtCreateNavMeshData(&params, &data, &size);
    const int polys = poly->npolys;
    release();
    if (!ok) {
        Log::error("NavMesh: Detour couldn't take the mesh");
        return false;
    }
    impl_->mesh = dtAllocNavMesh();
    if (dtStatusFailed(impl_->mesh->init(data, size, DT_TILE_FREE_DATA))) {
        dtFree(data);
        impl_->clear();
        return false;
    }
    impl_->query = dtAllocNavMeshQuery();
    impl_->crowd = dtAllocCrowd();
    if (dtStatusFailed(impl_->query->init(impl_->mesh, 4096)) ||
        !impl_->crowd->init(kMaxAgents, s.agentRadius, impl_->mesh)) {
        impl_->clear();
        return false;
    }
    Log::info("NavMesh: %d polygons over %.0f x %.0f m", polys, cfg.bmax[0] - cfg.bmin[0], cfg.bmax[2] - cfg.bmin[2]);
    return true;
}

bool NavMesh::findPath(Vec3 from, Vec3 to, std::vector<Vec3>& corners) const {
    corners.clear();
    const Vec3 reach{2.0f, 4.0f, 2.0f};
    Vec3 start, end;
    const dtPolyRef a = impl_->nearest(from, reach, start);
    const dtPolyRef b = impl_->nearest(to, reach, end);
    if (!a || !b) return false;
    dtPolyRef path[kMaxPath];
    int count = 0;
    if (dtStatusFailed(impl_->query->findPath(a, b, xyz(start), xyz(end), &impl_->filter, path, &count, kMaxPath)) ||
        count == 0) {
        return false;
    }
    // A partial path (the target unreachable) ends at its last polygon, not the target.
    Vec3 goal = end;
    if (path[count - 1] != b) impl_->query->closestPointOnPoly(path[count - 1], xyz(end), &goal.x, nullptr);
    float points[kMaxPath * 3];
    int n = 0;
    impl_->query->findStraightPath(xyz(start), xyz(goal), path, count, points, nullptr, nullptr, &n, kMaxPath);
    for (int i = 0; i < n; ++i) corners.push_back({points[i * 3], points[i * 3 + 1], points[i * 3 + 2]});
    return !corners.empty() && path[count - 1] == b;
}

bool NavMesh::nearest(Vec3 point, Vec3& onMesh, Vec3 reach) const {
    return impl_->nearest(point, reach, onMesh) != 0;
}

bool NavMesh::randomPointNear(Vec3 around, float radius, std::uint32_t seed, Vec3& out) const {
    Vec3 start;
    const dtPolyRef ref = impl_->nearest(around, {2.0f, 4.0f, 2.0f}, start);
    if (!ref) return false;
    g_random = seed * 2654435761u + 1u;
    dtPolyRef picked = 0;
    return dtStatusSucceed(impl_->query->findRandomPointAroundCircle(ref, xyz(start), radius, &impl_->filter,
                                                                      nextRandom, &picked, &out.x));
}

int NavMesh::island(Vec3 point) const {
    Vec3 on;
    const dtPolyRef ref = impl_->nearest(point, {2.0f, 4.0f, 2.0f}, on);
    if (!ref) return -1;
    const dtNavMesh& mesh = *impl_->mesh;
    auto& islands = impl_->islands;
    if (islands.empty()) {
        // Flood each unvisited polygon's island through its links.
        islands.resize(static_cast<std::size_t>(mesh.getMaxTiles()));
        for (int t = 0; t < mesh.getMaxTiles(); ++t) {
            const dtMeshTile* tile = mesh.getTile(t);
            if (tile && tile->header) islands[t].assign(static_cast<std::size_t>(tile->header->polyCount), -1);
        }
        int next = 0;
        std::vector<std::pair<unsigned int, unsigned int>> open;
        for (int t = 0; t < mesh.getMaxTiles(); ++t) {
            for (std::size_t p = 0; p < islands[t].size(); ++p) {
                if (islands[t][p] >= 0) continue;
                islands[t][p] = next;
                open.push_back({static_cast<unsigned int>(t), static_cast<unsigned int>(p)});
                while (!open.empty()) {
                    const auto [ct, cp] = open.back();
                    open.pop_back();
                    const dtMeshTile* tile = mesh.getTile(static_cast<int>(ct));
                    for (unsigned int k = tile->polys[cp].firstLink; k != DT_NULL_LINK; k = tile->links[k].next) {
                        unsigned int salt = 0, nt = 0, np = 0;
                        mesh.decodePolyId(tile->links[k].ref, salt, nt, np);
                        if (nt < islands.size() && np < islands[nt].size() && islands[nt][np] < 0) {
                            islands[nt][np] = next;
                            open.push_back({nt, np});
                        }
                    }
                }
                ++next;
            }
        }
    }
    unsigned int salt = 0, t = 0, p = 0;
    mesh.decodePolyId(ref, salt, t, p);
    return t < islands.size() && p < islands[t].size() ? islands[t][p] : -1;
}

void NavMesh::edges(std::vector<std::pair<Vec3, Vec3>>& out) const {
    out.clear();
    if (!impl_->mesh) return;
    const dtNavMesh& mesh = *impl_->mesh;
    for (int t = 0; t < mesh.getMaxTiles(); ++t) {
        const dtMeshTile* tile = mesh.getTile(t);
        if (!tile || !tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() != DT_POLYTYPE_GROUND) continue;
            for (int j = 0; j < poly.vertCount; ++j) {
                const float* a = &tile->verts[poly.verts[j] * 3];
                const float* b = &tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
                out.push_back({{a[0], a[1], a[2]}, {b[0], b[1], b[2]}});
            }
        }
    }
}

int NavMesh::polygonCount() const {
    if (!impl_->mesh) return 0;
    const dtNavMesh& mesh = *impl_->mesh;
    int count = 0;
    for (int t = 0; t < mesh.getMaxTiles(); ++t) {
        const dtMeshTile* tile = mesh.getTile(t);
        if (tile && tile->header) count += tile->header->polyCount;
    }
    return count;
}

int NavMesh::addAgent(Vec3 at, float radius, float maxSpeed) {
    if (!impl_->crowd) return -1;
    Vec3 on;
    if (!impl_->nearest(at, {2.0f, 4.0f, 2.0f}, on)) return -1;
    dtCrowdAgentParams p{};
    p.radius = radius;
    p.height = impl_->agentHeight;
    p.maxAcceleration = 8.0f;
    p.maxSpeed = maxSpeed;
    p.collisionQueryRange = radius * 12.0f;
    p.pathOptimizationRange = radius * 30.0f;
    p.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO |
                    DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
    p.obstacleAvoidanceType = 3;
    p.separationWeight = 2.0f;
    return impl_->crowd->addAgent(xyz(on), &p);
}

void NavMesh::removeAgent(int agent) {
    if (impl_->crowd && agent >= 0) impl_->crowd->removeAgent(agent);
}

bool NavMesh::setTarget(int agent, Vec3 target) {
    if (!impl_->crowd || agent < 0) return false;
    Vec3 on;
    const dtPolyRef ref = impl_->nearest(target, {2.0f, 4.0f, 2.0f}, on);
    return ref && impl_->crowd->requestMoveTarget(agent, ref, xyz(on));
}

void NavMesh::stop(int agent) {
    if (impl_->crowd && agent >= 0) impl_->crowd->resetMoveTarget(agent);
}

void NavMesh::setMaxSpeed(int agent, float maxSpeed) {
    if (!impl_->crowd || agent < 0) return;
    const dtCrowdAgent* a = impl_->crowd->getAgent(agent);
    if (!a || !a->active) return;
    dtCrowdAgentParams p = a->params;
    p.maxSpeed = maxSpeed;
    impl_->crowd->updateAgentParameters(agent, &p);
}

void NavMesh::updateCrowd(float dt) {
    if (impl_->crowd && dt > 0.0f) impl_->crowd->update(dt, nullptr);
}

Vec3 NavMesh::agentPosition(int agent) const {
    const dtCrowdAgent* a = impl_->crowd && agent >= 0 ? impl_->crowd->getAgent(agent) : nullptr;
    return a ? Vec3{a->npos[0], a->npos[1], a->npos[2]} : Vec3{0.0f, 0.0f, 0.0f};
}

Vec3 NavMesh::agentVelocity(int agent) const {
    const dtCrowdAgent* a = impl_->crowd && agent >= 0 ? impl_->crowd->getAgent(agent) : nullptr;
    return a ? Vec3{a->vel[0], a->vel[1], a->vel[2]} : Vec3{0.0f, 0.0f, 0.0f};
}

bool NavMesh::arrived(int agent, float within) const {
    const dtCrowdAgent* a = impl_->crowd && agent >= 0 ? impl_->crowd->getAgent(agent) : nullptr;
    if (!a || a->targetState == DT_CROWDAGENT_TARGET_NONE) return true;
    const float dx = a->npos[0] - a->targetPos[0];
    const float dz = a->npos[2] - a->targetPos[2];
    return dx * dx + dz * dz < within * within;
}

}  // namespace iron
