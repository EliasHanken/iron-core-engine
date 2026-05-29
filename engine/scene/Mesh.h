#pragma once

#include "math/Aabb.h"
#include "math/Vec.h"

#include <cstdint>
#include <vector>

namespace iron {

// One vertex of a renderable mesh: position, normal, texture coordinate, and
// tangent (the in-plane direction matching the U axis of the UV layout).
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec3 tangent;
};

// CPU-side mesh: vertices plus an index list describing triangles. Uploaded to
// the GPU by the renderer (see Renderer::createMesh).
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

// A unit cube centered at the origin (side length 1), with per-face normals and
// UVs so every face can be textured. 24 vertices (4 per face), 36 indices.
MeshData makeCube();

// Axis-aligned bounds (min/max corner) over a mesh's vertex positions. Returns
// a zero box for an empty mesh. Used to build per-entity pick bounds.
Aabb meshBounds(const MeshData& mesh);

// Appends a box (a cuboid) to `out`: 24 vertices (per-face normals + UVs) and
// 36 indices, centered at `center` with full extents `size`. Indices are
// offset so the box references its own vertices when appended to a non-empty
// MeshData.
void appendBox(MeshData& out, Vec3 center, Vec3 size);

// Appends a low-poly tube around the polyline `points` to `out`. Each point
// gets a ring of `sides` vertices at distance `radius`, with outward normals
// and UVs (U around the ring, V tiling along the rope's length); consecutive
// rings are stitched into triangles. Does nothing if there are fewer than 2
// points or fewer than 3 sides.
void appendTube(MeshData& out, const std::vector<Vec3>& points, float radius,
                int sides);

// Appends a single flat quad to `out`. size.x and size.y are the two
// in-plane dimensions. The in-plane axes are derived from `normal`: u
// is the +X world axis projected onto the plane (or +Y if normal is
// near-parallel to +X), and v = cross(normal, u). For normal={0,1,0}
// this gives u=+X and v=-Z. All four vertices share `normal` and UVs
// span 0..1 across the quad (u→s, v→t). Two triangles, CCW seen from
// +normal.
void appendQuad(MeshData& out, Vec3 center, Vec2 size, Vec3 normal);

} // namespace iron
