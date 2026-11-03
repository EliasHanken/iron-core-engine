#pragma once

// Where things that walk can go: a navigation mesh built by Recast from the
// world's geometry, Detour's path queries over it, and a DetourCrowd steering
// agents along their paths round one another. The same pair Unreal uses.
//
// Build it once from a triangle soup of the static world (floors, walls, ramps,
// stairs; y up), within bounds; then ask for paths, or add agents and give them
// targets and let the crowd move them each frame.

#include "math/Vec.h"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace iron {

struct NavMeshSettings {
    // The grid the world is voxelized on, in metres.
    float cellSize = 0.15f;
    float cellHeight = 0.1f;
    // Who walks it: how tall, how wide, how high a step it takes, how steep a slope.
    float agentHeight = 1.8f;
    float agentRadius = 0.35f;
    float agentMaxClimb = 0.45f;
    float agentMaxSlopeDegrees = 45.0f;
    // Only what's within these bounds is built (the world may run on far beyond).
    Vec3 boundsMin{-1.0e9f, -1.0e9f, -1.0e9f};
    Vec3 boundsMax{1.0e9f, 1.0e9f, 1.0e9f};
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Builds from triangles (three indices each) over `vertices`, in world space.
    bool build(std::span<const Vec3> vertices, std::span<const std::uint32_t> indices,
               const NavMeshSettings& settings = {});
    bool ready() const;

    // The corners of the straightest path from `from` to `to`, each end first
    // snapped onto the mesh. False if either end is off it or nothing joins them.
    bool findPath(Vec3 from, Vec3 to, std::vector<Vec3>& corners) const;
    // The nearest point on the mesh within `reach` (half-extents) of `point`.
    bool nearest(Vec3 point, Vec3& onMesh, Vec3 reach = {2.0f, 4.0f, 2.0f}) const;
    // A random point on the mesh, reachable from `around`, within `radius` of it.
    bool randomPointNear(Vec3 around, float radius, std::uint32_t seed, Vec3& out) const;
    // Every polygon's edges, for drawing the mesh.
    void edges(std::vector<std::pair<Vec3, Vec3>>& out) const;
    int polygonCount() const;

    // --- The crowd ---
    // Adds an agent standing at `at` (snapped onto the mesh); -1 if it can't.
    int addAgent(Vec3 at, float radius, float maxSpeed);
    void removeAgent(int agent);
    // Sends an agent toward `target`; false if there's no point on the mesh near it.
    bool setTarget(int agent, Vec3 target);
    void stop(int agent);
    void setMaxSpeed(int agent, float maxSpeed);
    // Moves every agent along its path by `dt`, steering round the others.
    void updateCrowd(float dt);
    Vec3 agentPosition(int agent) const;
    Vec3 agentVelocity(int agent) const;
    // Within `within` metres of its target, or without one.
    bool arrived(int agent, float within) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iron
