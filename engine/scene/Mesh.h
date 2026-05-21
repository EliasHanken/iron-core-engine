#pragma once

#include "math/Vec.h"

#include <cstdint>
#include <vector>

namespace iron {

// One vertex of a renderable mesh: position, normal, and texture coordinate.
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
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

// Appends a torus (a ring) to `out`, centered at `center`, lying in the XZ
// plane. The major circle has radius `majorRadius`; the tube cross-section has
// radius `minorRadius`. `majorSegments` / `minorSegments` control resolution.
// Vertices carry outward normals and UVs. Does nothing if either segment count
// is below 3 or either radius is non-positive.
void appendTorus(MeshData& out, Vec3 center, float majorRadius,
                 float minorRadius, int majorSegments, int minorSegments);

} // namespace iron
